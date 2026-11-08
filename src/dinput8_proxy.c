/*
 * rtr-mousefix - dinput8.dll proxy for Rogue Trooper Redux (Steam, RTR_x64.exe).
 *
 * WHAT IT FIXES
 *   The game sends mouse look through a gamepad-stick response curve
 *   (fcn at RVA 0x178860). Horizontally that curve ends in a hard clamp, so
 *   past a certain speed a faster flick turns the camera LESS than a slow
 *   sweep over the same distance ("negative mouse acceleration").
 *   This proxy redirects the single call to that function and replaces the
 *   curve with a linear mapping (strength configurable in dinput8-proxy.ini)
 *   whenever the mouse is the input source. Gamepad input is untouched.
 *   See docs/ANALYSIS.md for the full reverse-engineering notes.
 *
 * HOW IT LOADS
 *   The game imports DirectInput8Create from dinput8.dll. This DLL exports the
 *   same function, loads the real dinput8.dll from the system directory and
 *   forwards the call. It patches COM vtables in place (no wrapper objects)
 *   and one 5-byte call instruction in the game's code. The EXE on disk is
 *   never modified.
 *
 * ENVIRONMENT VARIABLES (all optional)
 *   RTR_MOUSE_LINEAR  1 (default) = all fixes on, 0 = everything off (pure pass-through)
 *   RTR_MOUSE_GAIN    initial look strength for both axes (default 0.5); dinput8-proxy.ini overrides it
 *   RTR_DINPUT_FIX    1 = merge buffered mouse events and enlarge the DirectInput
 *                     buffer to 4096 (only matters for very high polling rates)
 *
 * dinput8-proxy.log (next to the DLL) records what happened at start-up: which patches were
 * installed or why one was skipped, and every ini reload.
 *
 * Built without a C runtime (kernel32/user32 only), see build.sh.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "config.h"

/* ---- Addresses inside RTR_x64.exe (RVA = VA - 0x140000000). See docs/ANALYSIS.md ---- */
#define RVA_TEXT_START   0x1000    /* .text start */
#define RVA_TEXT_SIZE    0x2d11b3  /* .text virtual size */
#define RVA_LOOK_CURVE   0x178860  /* stick response curve */
#define RVA_LOOK_CALL    0x179130  /* the only call of the curve ("call rel32") */
#define RVA_FOV_FN       0x147d40  /* returns the horizontal FOV in radians, as 64 bits in xmm0 */
#define RVA_FOV_CALL     0x147525  /* its only call ("call rel32") */
#define RVA_MENU_MOUSE   0x29a500  /* Asura_MenuMouseInput update: moves the menu pointer */
#define RVA_MENU_CALL    0x29b39a  /* its call in the per-frame menu input update ("call rel32") */
#define RVA_ASPECT       0x3d72f4  /* float: render width / height */
#define RVA_LOOK_X       0x41cce4  /* float: horizontal look input (mouse) */
#define RVA_LOOK_Y       0x41cce8  /* float: vertical look input (mouse)   */
#define RVA_INVERT_FLAG  0x41fbfb  /* byte: invert flag applied to the horizontal axis */

#define LOOK_SPIKE 150.0f /* legitimate |look| stays below ~60; cursor warps produce > 4000 */
#define FIX_BUFFER_EVENTS 4096

typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_CreateDevice)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *PFN_GetDeviceData)(void *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (WINAPI *PFN_SetProperty)(void *, REFGUID, LPCDIPROPHEADER);
typedef ULONGLONG (WINAPI *PFN_menu)(void *);
typedef double (WINAPI *PFN_fov)(DWORD); /* float in the low 32 bits of xmm0 + 32 more bits */
typedef void (WINAPI *PFN_look)(float *, float *, float, float, ULONGLONG, ULONGLONG, ULONGLONG, ULONGLONG, float *);

static const GUID k_SysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static PFN_Create real_Create;
static PFN_CreateDevice real_CreateDevice;
static PFN_GetDeviceData real_GetDeviceData;
static PFN_SetProperty real_SetProperty;
static PFN_look real_look;
static PFN_menu real_menu;
static PFN_fov real_fov;

static HINSTANCE g_dll;
static ULONG_PTR g_base;           /* image base of the game EXE */
static void *g_mouse;              /* the IDirectInputDevice8 of the system mouse (event merging only) */
static int g_patch_tried;          /* the patches are attempted once */
static int g_linear = 1;           /* see RTR_MOUSE_LINEAR (master switch) */
static int g_fix;                  /* see RTR_DINPUT_FIX */
static rtr_config g_cfg = {RTR_GAIN_DEFAULT, RTR_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, RTR_MENU_EQUALIZE_DEFAULT, 0.0f};

static HANDLE g_log = INVALID_HANDLE_VALUE;

static char g_ini_path[MAX_PATH];
static FILETIME g_ini_mtime;
static DWORD g_ini_tick;

static const char k_ini_default[] =
    "; rtr-mousefix - Rogue Trooper Redux mouse fix (dinput8-proxy)\r\n"
    "; Changes are picked up while the game is running, within about a second.\r\n"
    "; No restart needed.\r\n"
    ";\r\n"
    "; Mouse look strength. Higher = faster, lower = slower. Allowed: 0.05 to 20.\r\n"
    ";   gain_x: horizontal (left/right)    gain_y: vertical (up/down)\r\n"
    "; The in-game SENSITIVITY X/Y options still apply on top (they multiply).\r\n"
    "gain_x=0.5\r\n"
    "gain_y=0.5\r\n"
    ";\r\n"
    "; Menu pointer (the mouse cursor in the game's menus).\r\n"
    "; The game moves it by the same fraction of the screen per mouse movement on both axes, so on a\r\n"
    "; wide screen it travels more pixels sideways than up/down (about 2.4 times on 21:9).\r\n"
    "; menu_equalize=1 makes it equally fast in pixels on both axes (the sideways speed stays as it is);\r\n"
    "; 0 keeps the game's own behaviour. menu_gain_x/y then scale the speed of each axis (1.0 = unchanged).\r\n"
    "menu_equalize=1\r\n"
    "menu_gain_x=1.0\r\n"
    "menu_gain_y=1.0\r\n"
    ";\r\n"
    "; Field of view. fov_horizontal = the HORIZONTAL FOV you want, in degrees; 0 = leave the game alone.\r\n"
    "; The game picks its own value from the aspect ratio: about 62 degrees at 4:3, 75 at 16:9, 84 at\r\n"
    "; 21:9 and 101 at 32:9. Try 90 to 110. Allowed 30 to 150. Modes that are narrower in the game,\r\n"
    "; such as aiming, stay proportionally narrower. Applies to cutscenes too.\r\n"
    "fov_horizontal=0\r\n";

static int same_guid(const GUID *a, const GUID *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static float absf(float x) { return x < 0 ? -x : x; }

/* ------------------------------------------------------------------ logging */

static void logf_line(const char *fmt, ...) {
    char buf[640];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);
    __builtin_va_end(ap);
    if (g_log != INVALID_HANDLE_VALUE && n > 0) {
        DWORD w;
        WriteFile(g_log, buf, n, &w, NULL);
    }
}

/* Opens "<dll directory>/<name>" for writing. */
static HANDLE open_beside_dll(HINSTANCE inst, const char *name, DWORD disposition, DWORD access) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    path[n] = 0;
    lstrcatA(path, name);
    return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* -------------------------------------------------- optional: buffer merging */

/*
 * RTR_DINPUT_FIX=1. Drains every queued event, merges X and Y movement into one
 * entry each, forwards buttons/wheel unchanged and always returns DI_OK. The game
 * discards a whole frame of mouse movement when GetDeviceData returns
 * DI_BUFFEROVERFLOW (64-entry buffer); with a normal mouse this never happened in
 * testing, so this is only a safety net for very high polling rates.
 */
static HRESULT merge_mouse(void *This, DWORD cb, LPDIDEVICEOBJECTDATA out, LPDWORD pn, DWORD flags) {
    static DIDEVICEOBJECTDATA tmp[256];
    DWORD cap = *pn, outn = 0, ts = 0, seq = 0;
    LONG sx = 0, sy = 0;
    int got_any = 0;

    for (int round = 0; round < 64; round++) {
        DWORD n = 256;
        HRESULT hr = real_GetDeviceData(This, cb, tmp, &n, flags);
        if (FAILED(hr)) {
            if (!got_any) return hr; /* e.g. DIERR_NOTACQUIRED: report unchanged */
            break;
        }
        got_any = 1;
        for (DWORD i = 0; i < n; i++) {
            ts = tmp[i].dwTimeStamp;
            seq = tmp[i].dwSequence;
            if (tmp[i].dwOfs == DIMOFS_X) sx += (LONG)tmp[i].dwData;
            else if (tmp[i].dwOfs == DIMOFS_Y) sy += (LONG)tmp[i].dwData;
            else if (outn + 2 < cap) out[outn++] = tmp[i];
        }
        if (n < 256) break;
    }
    if (sx && outn < cap) {
        out[outn].dwOfs = DIMOFS_X; out[outn].dwData = (DWORD)sx;
        out[outn].dwTimeStamp = ts; out[outn].dwSequence = seq; out[outn].uAppData = 0;
        outn++;
    }
    if (sy && outn < cap) {
        out[outn].dwOfs = DIMOFS_Y; out[outn].dwData = (DWORD)sy;
        out[outn].dwTimeStamp = ts; out[outn].dwSequence = seq; out[outn].uAppData = 0;
        outn++;
    }
    *pn = outn;
    return DI_OK;
}

/* ---------------------------------------------------- DirectInput device hooks */

/* Only installed in RTR_DINPUT_FIX mode. */
static HRESULT WINAPI hook_GetDeviceData(void *This, DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD pn, DWORD flags) {
    if (This != g_mouse || !d || !pn || cb != sizeof(DIDEVICEOBJECTDATA) || (flags & DIGDD_PEEK))
        return real_GetDeviceData(This, cb, d, pn, flags);
    return merge_mouse(This, cb, d, pn, flags);
}

/* Wine's device queue drops new events when full, so merging alone cannot recover
 * them; in RTR_DINPUT_FIX mode the buffer the game asks for (64) is enlarged. */
static HRESULT WINAPI hook_SetProperty(void *This, REFGUID prop, LPCDIPROPHEADER hdr) {
    if (g_fix && This == g_mouse && (ULONG_PTR)prop == (ULONG_PTR)DIPROP_BUFFERSIZE && hdr &&
        hdr->dwSize == sizeof(DIPROPDWORD) && hdr->dwHow == DIPH_DEVICE) {
        DIPROPDWORD c = *(const DIPROPDWORD *)hdr;
        logf_line("SetProperty BUFFERSIZE %lu -> %d\r\n", c.dwData, FIX_BUFFER_EVENTS);
        if (c.dwData < FIX_BUFFER_EVENTS) c.dwData = FIX_BUFFER_EVENTS;
        return real_SetProperty(This, prop, &c.diph);
    }
    return real_SetProperty(This, prop, hdr);
}

static HRESULT WINAPI hook_CreateDevice(void *This, REFGUID rguid, void **dev, void *outer) {
    HRESULT hr = real_CreateDevice(This, rguid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev && same_guid(rguid, &k_SysMouse)) {
        g_mouse = *dev;
        void **vt = *(void ***)g_mouse;
        if (vt[10] != (void *)hook_GetDeviceData) {
            DWORD old;
            /* IDirectInputDevice8 vtable: [6] SetProperty ... [10] GetDeviceData */
            if (VirtualProtect(&vt[6], 5 * sizeof(void *), PAGE_READWRITE, &old)) {
                real_SetProperty = (PFN_SetProperty)vt[6];
                real_GetDeviceData = (PFN_GetDeviceData)vt[10];
                vt[6] = (void *)hook_SetProperty;
                vt[10] = (void *)hook_GetDeviceData;
                VirtualProtect(&vt[6], 5 * sizeof(void *), old, &old);
                logf_line("mouse device %p hooked (event merging)\r\n", g_mouse);
            } else {
                logf_line("VirtualProtect on the device vtable failed\r\n");
            }
        }
    }
    return hr;
}

/* ------------------------------------------------------------ configuration */

static void ini_poll(int force) {
    DWORD now = GetTickCount();
    if (!force && now - g_ini_tick < 1000) return;
    g_ini_tick = now;
    if (!g_ini_path[0]) return;

    HANDLE h = CreateFileA(g_ini_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME mt;
    char buf[1024];
    DWORD n = 0;
    if (GetFileTime(h, NULL, NULL, &mt) &&
        (mt.dwLowDateTime != g_ini_mtime.dwLowDateTime || mt.dwHighDateTime != g_ini_mtime.dwHighDateTime) &&
        ReadFile(h, buf, sizeof buf - 1, &n, NULL)) {
        buf[n] = 0;
        g_ini_mtime = mt;
        rtr_ini_parse(buf, &g_cfg);
        logf_line("ini loaded: gain_x=%d/100 gain_y=%d/100 menu_equalize=%d menu_gain_x=%d/100 menu_gain_y=%d/100 fov_horizontal=%d/10\r\n",
                  (int)(g_cfg.gain_x * 100.0f + 0.5f), (int)(g_cfg.gain_y * 100.0f + 0.5f), g_cfg.menu_equalize > 0.0f,
                  (int)(g_cfg.menu_gain_x * 100.0f + 0.5f), (int)(g_cfg.menu_gain_y * 100.0f + 0.5f),
                  (int)(g_cfg.fov_horizontal * 10.0f + 0.5f));
    }
    CloseHandle(h);
}

/* Creates the ini with defaults if it does not exist yet, then loads it. */
static void ini_init(HINSTANCE inst) {
    DWORD n = GetModuleFileNameA(inst, g_ini_path, MAX_PATH);
    while (n && g_ini_path[n - 1] != '\\' && g_ini_path[n - 1] != '/') n--;
    g_ini_path[n] = 0;
    lstrcatA(g_ini_path, "dinput8-proxy.ini");
    HANDLE h = CreateFileA(g_ini_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, k_ini_default, sizeof k_ini_default - 1, &w, NULL);
        CloseHandle(h);
    }
    ini_poll(1);
}

/* ------------------------------------------------------------ look-curve fix */

/*
 * Replacement for the call to the stick response curve.
 *
 * Windows x64 arguments of the original: rcx = &x (horizontal), rdx = &y (vertical),
 * xmm2 = factor for x, xmm3 = factor for y, then stack arguments 5..9 (two mode
 * bytes and a pointer to the "hold-to-turn-faster" timer). The original is always
 * called first (it updates that timer). If the mouse is the source, its outputs are
 * then overwritten with a linear mapping.
 */
static void WINAPI hook_look(float *a1, float *a2, float s1, float s2,
                             ULONGLONG x5, ULONGLONG x6, ULONGLONG x7, ULONGLONG x8, float *acc) {
    float in1 = *a1, in2 = *a2;
    ini_poll(0);
    float lx = *(volatile float *)(g_base + RVA_LOOK_X);
    float ly = *(volatile float *)(g_base + RVA_LOOK_Y);

    real_look(a1, a2, s1, s2, x5, x6, x7, x8, acc);

    if (lx != 0.0f || ly != 0.0f) { /* the mouse is the source (a gamepad leaves both at 0) */
        float v1 = in1, v2 = in2;
        if (absf(v1) > LOOK_SPIKE || absf(v2) > LOOK_SPIKE) v1 = v2 = 0.0f; /* drop cursor warps */
        if (*(volatile unsigned char *)(g_base + RVA_INVERT_FLAG)) v1 = -v1; /* same as the original */
        *a1 = v1 * s1 * g_cfg.gain_x;
        *a2 = v2 * s2 * g_cfg.gain_y;
    }
}

/*
 * Replacement for the menu mouse update (Asura_MenuMouseInput). rcx points into the object; the
 * pointer position, normalised to 0..1, lives at rcx-0x20 (x) and rcx-0x1c (y). The game adds the
 * same amount per mouse count to both, so the pointer travels "aspect ratio" times more pixels
 * horizontally than vertically. The original runs unchanged; afterwards the movement it caused is
 * rescaled per axis (and clamped to 0..1 like the game does).
 */
static ULONGLONG WINAPI hook_menu_mouse(void *self) {
    float *px = (float *)((unsigned char *)self - 0x20);
    float *py = (float *)((unsigned char *)self - 0x1c);
    float x0 = *px, y0 = *py;
    ini_poll(0);
    ULONGLONG r = real_menu(self);
    float aspect = *(volatile float *)(g_base + RVA_ASPECT);
    *px = rtr_menu_scale(x0, *px, g_cfg.menu_gain_x);
    *py = rtr_menu_scale(y0, *py, rtr_menu_gain_y(&g_cfg, aspect));
    return r;
}

/*
 * Replacement for the FOV function (argument: camera mode). It returns the horizontal FOV in
 * radians as a float in the low 32 bits of xmm0; the upper bits are passed through untouched.
 * Calling the original with mode 0 gives the normal camera's stock FOV for the current aspect
 * ratio (mode 0 only does arithmetic on globals, no side effects).
 */
static double WINAPI hook_fov(DWORD mode) {
    union { double d; struct { float f; DWORD hi; } p; } r, n;
    ini_poll(0);
    r.d = real_fov(mode);
    if (g_cfg.fov_horizontal > 0.0f) {
        n.d = mode == 0 ? r.d : real_fov(0);
        r.p.f = (float)rtr_apply_fov(r.p.f, n.p.f, g_cfg.fov_horizontal);
    }
    return r.d;
}

/* Finds the next run of >= 16 bytes of 0xCC padding in .text after *scan; returns its start or NULL. */
static unsigned char *find_cave(SIZE_T *scan) {
    unsigned char *text = (unsigned char *)(g_base + RVA_TEXT_START);
    SIZE_T i = *scan;
    while (i + 16 <= RVA_TEXT_SIZE) {
        if (text[i] != 0xCC) { i++; continue; }
        SIZE_T j = i;
        while (j < RVA_TEXT_SIZE && text[j] == 0xCC) j++;
        if (j - i >= 16) { *scan = j; return text + i; }
        i = j;
    }
    return NULL;
}

/*
 * Redirects the "call rel32" at RVA site_rva (which must call RVA target_rva) to hook.
 * "call rel32" only reaches +-2 GB, but this DLL is loaded far away from the EXE, so the call
 * goes to a 14-byte "jmp [rip+0]; dq hook" stub placed in 0xCC alignment padding inside .text.
 * Nothing is patched unless the call site matches exactly what was analysed.
 * Returns 1 on success and stores the original function in *orig_out.
 */
static int patch_call(const char *what, ULONG_PTR site_rva, ULONG_PTR target_rva, void *hook, void **orig_out, SIZE_T *scan) {
    unsigned char *site = (unsigned char *)(g_base + site_rva);
    unsigned char *orig = (unsigned char *)(g_base + target_rva);
    DWORD old;

    if (site[0] != 0xE8 || (unsigned char *)(site + 5 + *(LONG *)(site + 1)) != orig) {
        logf_line("%s patch: call site does not match (different game version?), nothing patched\r\n", what);
        return 0;
    }
    unsigned char *cave = find_cave(scan);
    if (!cave) { logf_line("%s patch: no code cave found\r\n", what); return 0; }

    *orig_out = orig;
    if (!VirtualProtect(cave, 14, PAGE_EXECUTE_READWRITE, &old)) { logf_line("%s patch: VirtualProtect(cave) failed\r\n", what); return 0; }
    cave[0] = 0xFF; cave[1] = 0x25; cave[2] = cave[3] = cave[4] = cave[5] = 0; /* jmp [rip+0] */
    *(void **)(cave + 6) = hook;
    VirtualProtect(cave, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), cave, 14);

    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) { logf_line("%s patch: VirtualProtect(call site) failed\r\n", what); return 0; }
    *(LONG *)(site + 1) = (LONG)(cave - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    logf_line("%s patch active: call site %p -> cave %p -> hook %p\r\n", what, (void *)site, (void *)cave, hook);
    return 1;
}

static void apply_patches(void) {
    ini_init(g_dll);

    /* Never touch memory outside the image: this DLL may be loaded by another program or
     * by a different (smaller) build of the game. */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { logf_line("patches: host module has no valid header, nothing patched\r\n"); return; }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + (ULONG_PTR)dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage < RVA_TEXT_START + RVA_TEXT_SIZE ||
        nt->OptionalHeader.SizeOfImage < RVA_LOOK_CALL + 5 || nt->OptionalHeader.SizeOfImage < RVA_MENU_CALL + 5 ||
        nt->OptionalHeader.SizeOfImage < RVA_FOV_CALL + 5) {
        logf_line("patches: call site does not match (not Rogue Trooper Redux / different version?), nothing patched\r\n");
        return;
    }

    SIZE_T scan = 0;
    patch_call("look", RVA_LOOK_CALL, RVA_LOOK_CURVE, (void *)hook_look, (void **)&real_look, &scan);
    patch_call("menu", RVA_MENU_CALL, RVA_MENU_MOUSE, (void *)hook_menu_mouse, (void **)&real_menu, &scan);
    patch_call("fov", RVA_FOV_CALL, RVA_FOV_FN, (void *)hook_fov, (void **)&real_fov, &scan);
}

/* ---------------------------------------------------------------- DLL entry */

HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer) {
    if (!real_Create) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\dinput8.dll");
        HMODULE m = LoadLibraryA(path);
        if (!m) return E_FAIL;
        real_Create = (PFN_Create)GetProcAddress(m, "DirectInput8Create");
        if (!real_Create) return E_FAIL;
    }
    HRESULT hr = real_Create(h, ver, riid, out, outer);

    /* By now the game's code is decrypted (SteamStub runs before main), so the call site can be patched. */
    if (g_linear && !g_patch_tried) {
        g_patch_tried = 1;
        apply_patches();
    }

    if (g_fix && SUCCEEDED(hr) && out && *out && !real_CreateDevice) {
        void **vt = *(void ***)*out; /* IDirectInput8 vtable: [3] CreateDevice */
        DWORD old;
        if (VirtualProtect(&vt[3], sizeof(void *), PAGE_READWRITE, &old)) {
            real_CreateDevice = (PFN_CreateDevice)vt[3];
            vt[3] = (void *)hook_CreateDevice;
            VirtualProtect(&vt[3], sizeof(void *), old, &old);
            logf_line("IDirectInput8 %p: CreateDevice hooked\r\n", *out);
        }
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        char val[16];
        DisableThreadLibraryCalls(inst);
        g_dll = inst;
        g_base = (ULONG_PTR)GetModuleHandleA(NULL);
        g_log = open_beside_dll(inst, "dinput8-proxy.log", OPEN_ALWAYS, FILE_APPEND_DATA);

        g_fix = GetEnvironmentVariableA("RTR_DINPUT_FIX", val, sizeof val) && val[0] == '1';
        if (GetEnvironmentVariableA("RTR_MOUSE_LINEAR", val, sizeof val)) g_linear = val[0] != '0';
        if (GetEnvironmentVariableA("RTR_MOUSE_GAIN", val, sizeof val)) {
            float g;
            if (rtr_parse_float(val, &g)) g_cfg.gain_x = g_cfg.gain_y = rtr_clampf(g, RTR_GAIN_MIN, RTR_GAIN_MAX);
        }

        logf_line("--- rtr-mousefix proxy loaded: fixes=%s, event merging=%s ---\r\n", g_linear ? "on" : "off", g_fix ? "on" : "off");
    }
    return TRUE;
}
