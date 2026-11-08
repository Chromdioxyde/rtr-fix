/*
 * Windows test program (run under Wine/Proton, see run_wine_test.sh).
 *
 * Loads the proxy DLL, creates the system-mouse device, injects mouse movement with
 * SendInput and calls GetDeviceData with a 64-entry buffer, like the game does.
 *
 * Round 0 injects 10 moves (20 events: fits the buffer).
 * Rounds 1 and 2 inject 100 moves (200 events: overflows the 64-entry queue).
 *
 * Expected output without RTR_DINPUT_FIX: hr=00000001 (DI_BUFFEROVERFLOW), n=63, partial sums.
 * Expected output with    RTR_DINPUT_FIX=1: hr=00000000, n=2, sums close to the injected 100/100
 * (a few counts of noise are possible when a real mouse moves during the test).
 *
 * Freestanding like the proxy: kernel32/user32 only.
 */
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

typedef HRESULT (WINAPI *PFN)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);

static void out(const char *fmt, ...) {
    char b[400];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = wvsprintfA(b, fmt, ap);
    __builtin_va_end(ap);
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b, n, &w, NULL);
}

static int run(void) {
    char dllbuf[MAX_PATH];
    const char *dll = GetEnvironmentVariableA("PROXY_DLL", dllbuf, sizeof dllbuf) ? dllbuf : "dinput8.dll";
    HMODULE m = LoadLibraryA(dll);
    if (!m) { out("LoadLibrary(%s) failed %lu\n", dll, GetLastError()); return 1; }
    PFN create = (PFN)GetProcAddress(m, "DirectInput8Create");
    IDirectInput8A *di = NULL;
    HRESULT hr = create(GetModuleHandleA(NULL), 0x0800, &IID_IDirectInput8A, (void **)&di, NULL);
    out("DirectInput8Create hr=%08lx\n", hr);
    if (FAILED(hr)) return 1;

    IDirectInputDevice8A *dev = NULL;
    hr = IDirectInput8_CreateDevice(di, &GUID_SysMouse, &dev, NULL);
    out("CreateDevice hr=%08lx\n", hr);
    if (FAILED(hr)) return 1;

    HWND hwnd = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 200, NULL, NULL, NULL, NULL);
    IDirectInputDevice8_SetDataFormat(dev, &c_dfDIMouse2);
    IDirectInputDevice8_SetCooperativeLevel(dev, hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIPROPDWORD p = {{sizeof p, sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 64};
    IDirectInputDevice8_SetProperty(dev, DIPROP_BUFFERSIZE, &p.diph);
    IDirectInputDevice8_Acquire(dev);

    for (int round = 0; round < 3; round++) {
        int moves = round == 0 ? 10 : 100;
        for (int i = 0; i < moves; i++) {
            INPUT in;
            for (unsigned k = 0; k < sizeof in; k++) ((unsigned char *)&in)[k] = 0;
            in.type = INPUT_MOUSE;
            in.mi.dx = 1; in.mi.dy = 1;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof in);
        }
        Sleep(200);
        DIDEVICEOBJECTDATA d[64];
        DWORD n = 64;
        hr = IDirectInputDevice8_GetDeviceData(dev, sizeof d[0], d, &n, 0);
        long sx = 0, sy = 0;
        for (DWORD i = 0; i < n && SUCCEEDED(hr); i++) {
            if (d[i].dwOfs == DIMOFS_X) sx += (LONG)d[i].dwData;
            if (d[i].dwOfs == DIMOFS_Y) sy += (LONG)d[i].dwData;
        }
        out("round %d: moves=%d hr=%08lx n=%lu sum_x=%ld sum_y=%ld\n", round, moves, hr, n, sx, sy);
        n = 64; /* drain leftovers so the next round starts clean */
        IDirectInputDevice8_GetDeviceData(dev, sizeof d[0], d, &n, 0);
    }
    return 0;
}

void mainstart(void) { ExitProcess(run()); }
