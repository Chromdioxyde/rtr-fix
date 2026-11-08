/* Host-side test for src/config.h (the ini parser shared with the proxy). */
#include <math.h>
#include <stdio.h>

#include "../src/config.h"

static int failures;

/* Starts from (ix, iy), parses ini, expects (ex, ey). */
static void expect(const char *name, const char *ini, float ix, float iy, float ex, float ey) {
    rtr_config c = {ix, iy, RTR_MENU_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, RTR_MENU_EQUALIZE_DEFAULT, 0.0f};
    rtr_ini_parse(ini, &c);
    int ok = fabsf(c.gain_x - ex) < 1e-4f && fabsf(c.gain_y - ey) < 1e-4f;
    printf("%s %-34s x=%.3f y=%.3f (expected %.3f %.3f)\n", ok ? "ok  " : "FAIL", name, c.gain_x, c.gain_y, ex, ey);
    if (!ok) failures++;
}

int main(void) {
    const float D = RTR_GAIN_DEFAULT;
    expect("plain values, CRLF", "gain_x=0.5\r\ngain_y=1.75\r\n", D, D, 0.5f, 1.75f);
    expect("spaces and tabs around '='", "gain_x = 0.75\ngain_y\t=\t0.3\n", D, D, 0.75f, 0.3f);
    expect("trailing comment", "gain_x=2 ; faster\ngain_y=0.5;slow\n", D, D, 2.0f, 0.5f);
    expect("comment lines ignored", "; gain_x=9\n# gain_y=9\ngain_x=1.5\n", D, D, 1.5f, D);
    expect("integer value", "gain_x=3\n", D, D, 3.0f, D);
    expect("no trailing newline", "gain_y=1.25", D, D, D, 1.25f);
    expect("leading dot", "gain_x=.5\n", 2.0f, 2.0f, 0.5f, 2.0f);
    expect("garbage keeps current value", "gain_x=abc\ngain_y=\n", 0.7f, 0.9f, 0.7f, 0.9f);
    expect("zero and negative keep current", "gain_x=0\ngain_y=-2\n", 0.7f, 0.9f, 0.7f, 0.9f);
    expect("decimal comma accepted", "gain_x=0,6\ngain_y=1,25\n", D, D, 0.6f, 1.25f);
    expect("bad line after a good one", "gain_x=0.4\ngain_x=0..6x\n", D, D, 0.4f, D);
    expect("clamped to max 20", "gain_x=99\n", D, D, 20.0f, D);
    expect("clamped to min 0.05", "gain_y=0.001\n", D, D, D, 0.05f);
    expect("unknown keys ignored", "foo=3\ngain_z=5\ngain=7\ngain_xx=4\n", 0.7f, 0.9f, 0.7f, 0.9f);
    expect("shipped default file", "; rtr-mousefix\r\n;\r\ngain_x=0.5\r\ngain_y=0.5\r\n", 2.0f, 2.0f, 0.5f, 0.5f);
    expect("later line wins", "gain_x=2\ngain_x=3\n", D, D, 3.0f, D);
    {
        rtr_config c = {D, D, RTR_MENU_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, RTR_MENU_EQUALIZE_DEFAULT, 0.0f};
        rtr_ini_parse("menu_gain_x = 0.75\r\nmenu_gain_y=1,2\r\ngain_x=0.3\r\n", &c);
        int ok = fabsf(c.menu_gain_x - 0.75f) < 1e-4f && fabsf(c.menu_gain_y - 1.2f) < 1e-4f && fabsf(c.gain_x - 0.3f) < 1e-4f && fabsf(c.gain_y - D) < 1e-4f;
        printf("%s %-34s menu x=%.3f y=%.3f, look x=%.3f y=%.3f\n", ok ? "ok  " : "FAIL", "menu gains are separate keys", c.menu_gain_x, c.menu_gain_y, c.gain_x, c.gain_y);
        if (!ok) failures++;
        rtr_config d = {D, D, 0.8f, 0.9f, RTR_MENU_EQUALIZE_DEFAULT, 0.0f};
        rtr_ini_parse("menu_gain_x=zzz\nmenu_gain_y=0\nmenu_gain_xx=3\n", &d);
        ok = fabsf(d.menu_gain_x - 0.8f) < 1e-4f && fabsf(d.menu_gain_y - 0.9f) < 1e-4f;
        printf("%s %-34s menu x=%.3f y=%.3f\n", ok ? "ok  " : "FAIL", "invalid menu values keep current", d.menu_gain_x, d.menu_gain_y);
        if (!ok) failures++;
    }
    {
        struct { const char *name, *ini; float init, expect; } t[] = {
            {"fov_horizontal plain", "fov_horizontal=100\n", 0.0f, 100.0f},
            {"fov_horizontal 0 switches off", "fov_horizontal=0\n", 90.0f, 0.0f},
            {"fov_horizontal decimal comma", "fov_horizontal = 97,5\n", 0.0f, 97.5f},
            {"fov_horizontal clamped to max", "fov_horizontal=500\n", 0.0f, RTR_FOV_H_MAX},
            {"fov_horizontal clamped to min", "fov_horizontal=3\n", 0.0f, RTR_FOV_H_MIN},
            {"fov_horizontal negative rejected", "fov_horizontal=-5\n", 90.0f, 90.0f},
            {"fov_horizontal garbage keeps current", "fov_horizontal=abc\n", 90.0f, 90.0f},
            {"old fov_vertical key is ignored", "fov_vertical=60\n", 90.0f, 90.0f},
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            rtr_config c = {D, D, RTR_MENU_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, RTR_MENU_EQUALIZE_DEFAULT, t[i].init};
            rtr_ini_parse(t[i].ini, &c);
            int ok = fabsf(c.fov_horizontal - t[i].expect) < 1e-4f;
            printf("%s %-38s fov_horizontal=%.2f (expected %.2f)\n", ok ? "ok  " : "FAIL", t[i].name, c.fov_horizontal, t[i].expect);
            if (!ok) failures++;
        }

        /* The game's stock horizontal FOV: aspect/0.4444*12.5+25 degrees, above 75 compressed. */
        struct { const char *name; double aspect; } ar[] = {
            {"4:3", 4.0 / 3.0}, {"5:4", 5.0 / 4.0}, {"16:10", 1.6}, {"16:9", 16.0 / 9.0},
            {"21:9 (3440x1440)", 3440.0 / 1440.0}, {"32:9", 32.0 / 9.0},
        };
        for (unsigned i = 0; i < sizeof ar / sizeof ar[0]; i++) {
            double asp = ar[i].aspect;
            double b = asp / 0.444444448 * 12.5 + 25.0, stock_deg = b;
            if (b > 75.0) stock_deg = 75.0 + (b - 75.0 > 100.0 ? 100.0 : b - 75.0) / 100.0 * 0.7 * 75.0;
            double stock = stock_deg * RTR_DEG2RAD;
            /* The normal camera gets exactly the wanted horizontal FOV, whatever the stock value was. */
            double got = rtr_apply_fov(stock, stock, 100.0) / RTR_DEG2RAD;
            int ok = fabs(got - 100.0) < 1e-6;
            printf("%s %-38s stock h=%.1f -> h=%.2f (wanted 100)\n", ok ? "ok  " : "FAIL", ar[i].name, stock_deg, got);
            if (!ok) failures++;
        }

        /* Per-mode behaviour is kept: a mode that is narrower in stock stays proportionally narrower. */
        double normal = 75.0 * RTR_DEG2RAD, aim = 50.0 * RTR_DEG2RAD;
        double new_normal = rtr_apply_fov(normal, normal, 100.0), new_aim = rtr_apply_fov(aim, normal, 100.0);
        int okp = fabs(new_aim / new_normal - aim / normal) < 1e-9 && fabs(new_normal / RTR_DEG2RAD - 100.0) < 1e-9;
        printf("%s %-38s aim/normal %.4f -> %.4f\n", okp ? "ok  " : "FAIL", "per-mode ratio preserved", aim / normal, new_aim / new_normal);
        if (!okp) failures++;

        int off = rtr_apply_fov(1.2, 1.3, 0.0) == 1.2 && rtr_apply_fov(1.2, 0.0, 100.0) == 1.2 && rtr_apply_fov(0.0, 1.3, 100.0) == 0.0;
        printf("%s %-38s\n", off ? "ok  " : "FAIL", "off / unusable inputs unchanged");
        if (!off) failures++;

        /* A mode that is wider than the normal camera in stock is capped at 150 degrees. */
        double capped = rtr_apply_fov(1.8, 1.0, 150.0) / RTR_DEG2RAD;
        int okc = fabs(capped - 150.0) < 1e-6;
        printf("%s %-38s %.2f deg\n", okc ? "ok  " : "FAIL", "results capped at 150 degrees", capped);
        if (!okc) failures++;
    }
    {
        struct { const char *name, *ini; float init, expect; } t[] = {
            {"menu_equalize 0 turns it off", "menu_equalize=0\n", 1.0f, 0.0f},
            {"menu_equalize 1 turns it on", "menu_equalize = 1\n", 0.0f, 1.0f},
            {"menu_equalize any positive is on", "menu_equalize=2\n", 0.0f, 1.0f},
            {"menu_equalize garbage keeps current", "menu_equalize=maybe\n", 0.0f, 0.0f},
            {"menu_equalize negative keeps current", "menu_equalize=-1\n", 1.0f, 1.0f},
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            rtr_config c = {D, D, RTR_MENU_GAIN_DEFAULT, RTR_MENU_GAIN_DEFAULT, t[i].init, 0.0f};
            rtr_ini_parse(t[i].ini, &c);
            int ok = fabsf(c.menu_equalize - t[i].expect) < 1e-6f;
            printf("%s %-38s menu_equalize=%.0f (expected %.0f)\n", ok ? "ok  " : "FAIL", t[i].name, c.menu_equalize, t[i].expect);
            if (!ok) failures++;
        }

        /* Pointer movement scaling */
        struct { const char *name; float old_pos, game_pos, gain, expect; } m[] = {
            {"gain 1 keeps the game's movement", 0.50f, 0.52f, 1.0f, 0.52f},
            {"gain 2 doubles the movement", 0.50f, 0.52f, 2.0f, 0.54f},
            {"gain 0.5 halves it (leftwards)", 0.50f, 0.48f, 0.5f, 0.49f},
            {"scaled movement is clamped to 1", 0.99f, 1.00f, 5.0f, 1.0f},
            {"scaled movement is clamped to 0", 0.01f, 0.00f, 5.0f, 0.0f},
            {"no game movement, no movement", 0.30f, 0.30f, 3.0f, 0.30f},
        };
        for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
            float got = rtr_menu_scale(m[i].old_pos, m[i].game_pos, m[i].gain);
            int ok = fabsf(got - m[i].expect) < 1e-5f;
            printf("%s %-38s %.4f (expected %.4f)\n", ok ? "ok  " : "FAIL", m[i].name, got, m[i].expect);
            if (!ok) failures++;
        }

        /* With equalising, one mouse count moves the pointer the same number of pixels on both axes. */
        double dims[][2] = {{3440, 1440}, {2560, 1440}, {1920, 1080}, {1280, 1024}, {5120, 1440}};
        for (unsigned i = 0; i < sizeof dims / sizeof dims[0]; i++) {
            double w = dims[i][0], h = dims[i][1];
            rtr_config c = {D, D, 1.0f, 1.0f, 1.0f, 0.0f};
            /* the game adds the same normalised amount n per count to both axes; pixels = n * size */
            double px = 1.0 * c.menu_gain_x * w;
            double py = 1.0 * rtr_menu_gain_y(&c, (float)(w / h)) * h;
            int ok = fabs(px - py) < 1e-3 * w;
            printf("%s %-38s pixels per count x=%.1f y=%.1f\n", ok ? "ok  " : "FAIL", "equalised pointer, screen size", px, py);
            if (!ok) failures++;
        }
        rtr_config off = {D, D, 1.0f, 1.0f, 0.0f, 0.0f};
        int okoff = rtr_menu_gain_y(&off, 2.39f) == 1.0f;
        printf("%s %-38s\n", okoff ? "ok  " : "FAIL", "menu_equalize=0 leaves y gain alone");
        if (!okoff) failures++;
    }
    printf("%s\n", failures ? "FAILED" : "all config tests passed");
    return failures ? 1 : 0;
}
