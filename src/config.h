/*
 * config.h - tiny, dependency-free parsing of dinput8-proxy.ini.
 *
 * Kept free of any Windows or libc dependency so the proxy (built without a C
 * runtime) and the host-side unit test (tests/test_config.c) share the exact
 * same code.
 *
 * Format: "key = value" lines. ';' or '#' at the start of a line starts a
 * comment, ';' after a value too. Unknown keys and malformed values are
 * ignored (the previous/default value stays).
 */
#ifndef RTR_CONFIG_H
#define RTR_CONFIG_H

#include <stddef.h>

#define RTR_GAIN_MIN 0.05f
#define RTR_GAIN_MAX 20.0f
#define RTR_GAIN_DEFAULT 0.5f /* shipped default for both axes */

typedef struct {
    float gain_x;      /* horizontal look strength        */
    float gain_y;      /* vertical look strength          */
    float menu_gain_x; /* horizontal menu cursor strength */
    float menu_gain_y; /* vertical menu cursor strength   */
    float menu_equalize; /* 1 = equal pixel speed on both axes, 0 = the game's own behaviour */
    float fov_horizontal; /* wanted horizontal FOV in degrees, 0 = leave the game's FOV alone */
} rtr_config;

#define RTR_FOV_H_MIN 30.0f
#define RTR_FOV_H_MAX 150.0f

#define RTR_MENU_GAIN_DEFAULT 1.0f /* 1.0 = unchanged */
#define RTR_MENU_EQUALIZE_DEFAULT 1.0f

static float rtr_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/*
 * Parses "123", "1.5", "1,5", ".5" into *out. Returns 1 on success, 0 (and leaves *out alone) for
 * empty or non-numeric input, and for 0 unless allow_zero is set. Negative numbers are rejected.
 */
static int rtr_parse_number(const char *s, float *out, int allow_zero) {
    float v = 0.0f, frac = 0.1f;
    int seen = 0, dot = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            seen = 1;
            if (!dot) {
                v = v * 10.0f + (float)(*s - '0');
            } else {
                v += (float)(*s - '0') * frac;
                frac *= 0.1f;
            }
        } else if ((*s == '.' || *s == ',') && !dot) { /* decimal point or comma */
            dot = 1;
        } else {
            break;
        }
    }
    if (!seen || v < 0.0f || (v == 0.0f && !allow_zero)) return 0;
    *out = v;
    return 1;
}

static int rtr_parse_float(const char *s, float *out) { return rtr_parse_number(s, out, 0); }

static int rtr_key_is(const char *key, size_t kl, const char *name) {
    size_t i = 0;
    for (; name[i]; i++)
        if (i >= kl || key[i] != name[i]) return 0;
    return i == kl;
}

/* Applies every recognised key (gain_x, gain_y, menu_gain_x, menu_gain_y, menu_equalize, fov_horizontal) of text to cfg. */
static void rtr_ini_parse(const char *t, rtr_config *cfg) {
    while (*t) {
        const char *line = t;
        while (*t && *t != '\n') t++;
        const char *end = t;
        if (*t) t++;
        if (line < end && (*line == ';' || *line == '#')) continue;

        const char *eq = line;
        while (eq < end && *eq != '=') eq++;
        if (eq >= end) continue;

        const char *key = line;
        size_t kl = (size_t)(eq - line);
        while (kl && (key[kl - 1] == ' ' || key[kl - 1] == '\t')) kl--;

        char val[32];
        size_t vl = 0;
        const char *c = eq + 1;
        while (c < end && (*c == ' ' || *c == '\t')) c++;
        for (; c < end && vl < sizeof val - 1 && *c != '\r' && *c != ' ' && *c != '\t' && *c != ';'; c++) val[vl++] = *c;
        val[vl] = 0;

        float v;
        if (rtr_key_is(key, kl, "menu_equalize")) {
            if (rtr_parse_number(val, &v, 1)) cfg->menu_equalize = v > 0.0f ? 1.0f : 0.0f;
        } else if (rtr_key_is(key, kl, "fov_horizontal")) {
            if (rtr_parse_number(val, &v, 1)) /* 0 switches the FOV fix off */
                cfg->fov_horizontal = v == 0.0f ? 0.0f : rtr_clampf(v, RTR_FOV_H_MIN, RTR_FOV_H_MAX);
        } else if (rtr_parse_float(val, &v)) { /* an invalid value keeps the current setting */
            v = rtr_clampf(v, RTR_GAIN_MIN, RTR_GAIN_MAX);
            if (rtr_key_is(key, kl, "gain_x")) cfg->gain_x = v;
            else if (rtr_key_is(key, kl, "gain_y")) cfg->gain_y = v;
            else if (rtr_key_is(key, kl, "menu_gain_x")) cfg->menu_gain_x = v;
            else if (rtr_key_is(key, kl, "menu_gain_y")) cfg->menu_gain_y = v;
        }
    }
}

/*
 * ---- Menu cursor ----
 *
 * The game keeps the menu pointer as a position in [0,1] x [0,1] and adds the same amount per mouse
 * count on both axes. In pixels that is "aspect ratio" times faster horizontally than vertically
 * (2.39 on 3440x1440, 1.78 on 16:9). The hook measures what the game added in one update and scales
 * it per axis.
 */
static float rtr_clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

/* New position after the game moved the pointer from old_pos to game_pos, with the movement scaled by gain. */
static float rtr_menu_scale(float old_pos, float game_pos, float gain) {
    return rtr_clamp01(old_pos + (game_pos - old_pos) * gain);
}

/* Vertical gain: the user's gain, times the aspect ratio if equalising (aspect is width/height). */
static float rtr_menu_gain_y(const rtr_config *c, float aspect) {
    return (c->menu_equalize > 0.0f && aspect > 0.0f) ? c->menu_gain_y * aspect : c->menu_gain_y;
}

/*
 * ---- Field of view ----
 *
 * The game computes a HORIZONTAL FOV in radians from the aspect ratio (about 62.5 degrees at 4:3,
 * 75 at 16:9, 84 at 21:9) and derives the vertical FOV from it. The user chooses the horizontal FOV
 * directly, in degrees, for the normal camera, the way most games do.
 */
#define RTR_DEG2RAD 0.0174532925199433
#define RTR_FOV_MAX_RAD (150.0 * RTR_DEG2RAD) /* tan(fov/2) explodes towards 180 degrees */

/*
 * stock_mode:   what the game computed for the current camera mode (radians)
 * stock_normal: what the game computes for the normal camera (mode 0) at the same aspect ratio
 * target_deg:   the horizontal FOV the user wants for the normal camera
 * The result is stock_mode scaled by target/stock_normal, so the normal camera gets exactly
 * target_deg and modes that are narrower in the stock game (e.g. while aiming) stay proportionally
 * narrower. Returns stock_mode unchanged if target_deg <= 0 or an input is unusable.
 */
static double rtr_apply_fov(double stock_mode, double stock_normal, double target_deg) {
    if (!(target_deg > 0.0) || !(stock_normal > 0.0) || !(stock_mode > 0.0)) return stock_mode;
    double r = stock_mode * (target_deg * RTR_DEG2RAD / stock_normal);
    return r > RTR_FOV_MAX_RAD ? RTR_FOV_MAX_RAD : r;
}

#endif /* RTR_CONFIG_H */
