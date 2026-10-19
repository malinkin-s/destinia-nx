/* main.c -- Destinia frontend. One source for PC (SDL2) and Switch (devkitA64 + SDL2).
 *
 *   destinia [--apk FILE] [--save DIR]                     play
 *   destinia --headless --frames N [--script F] [--manifest F] [--dump DIR K]
 *                                                       deterministic test mode
 * MIT. */
#include "apk.h"
#include "audio.h"
#include "game.h"
#include "sha256.h"
#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#ifdef __SWITCH__
#define DEFAULT_DIR   "sdmc:/switch/destinia"
#define DEFAULT_APK   DEFAULT_DIR "/DESTINIA_v1_06.apk"
#define DEFAULT_SAVE  DEFAULT_DIR "/save"
#else
#define DEFAULT_APK   "DESTINIA_v1_06.apk"
#define DEFAULT_SAVE  "save"
#endif

#include <stdarg.h>
static Apk *g_apk;
extern int g_patch_hide_pad;   /* native.c: patches */

#include <setjmp.h>
static jmp_buf g_tour_jb; static int g_tour_catch;   /* tour mode: survive a guest fault, report it and go on to the next map */
/* Fatal errors: log them, and on Switch show them on the text console until a button is pressed. */
void guest_fatal(int code, const char *fmt, ...) {
    char msg[512]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    fprintf(stderr, "FATAL: %s\n", msg); if (g_ring_on) cpu_dump_ring(); fflush(stderr);
    if (g_tour_catch) { g_tour_catch = 0; longjmp(g_tour_jb, 1); }
#ifdef __SWITCH__
    consoleInit(NULL);
    printf("Destinia: fatal error\n\n%s\n\nSee sdmc:/switch/destinia/log.txt\nPress + to exit.\n", msg);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break; consoleUpdate(NULL); }
    consoleExit(NULL);
#endif
    exit(code);
}

/* ---------------------------------------------------------------- host ---- */
static int host_asset_size(const char *name) {
    char p[256]; snprintf(p, sizeof p, "assets/%s", name); return g_apk ? apk_size(g_apk, p) : -1;
}
static uint8_t *host_asset_read(const char *name, size_t *len) {
    char p[256]; snprintf(p, sizeof p, "assets/%s", name); return g_apk ? apk_read(g_apk, p, len) : NULL;
}
static int g_audio_on;      /* GUI mode only: headless runs stay silent and just log */
static uint8_t *audio_loader(const char *path, size_t *len) { return g_apk ? apk_read(g_apk, path, len) : NULL; }
static void host_play_sound(int id) { if (g_audio_on) audio_play(id); }
static void host_set_volume(int t, int cur, int max) { if (g_audio_on) audio_volume(t, cur, max); }
static SDL_Joystick *g_joys[8];

#ifdef __SWITCH__
static HidVibrationDeviceHandle g_vib_handles[6];
static int g_vib_count = 0;
static Uint64 g_vib_stop_t = 0;
static int g_vib_active = 0;

static void switch_vib_init(void) {
    g_vib_count = 0;
    if (R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib_handles[g_vib_count], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
        g_vib_count += 2;
    }
    if (R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib_handles[g_vib_count], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
        g_vib_count += 2;
    }
    if (R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib_handles[g_vib_count], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey))) {
        g_vib_count += 2;
    }
}

static void switch_vib_send(float amp_low, float freq_low, float amp_high, float freq_high) {
    if (g_vib_count <= 0) return;
    HidVibrationValue values[6];
    for (int i = 0; i < g_vib_count; i++) {
        values[i].amp_low = amp_low;
        values[i].freq_low = freq_low;
        values[i].amp_high = amp_high;
        values[i].freq_high = freq_high;
    }
    hidSendVibrationValues(g_vib_handles, values, g_vib_count);
}

static void switch_vib_stop(void) {
    if (g_vib_active) {
        switch_vib_send(0.0f, 160.0f, 0.0f, 320.0f);
        g_vib_active = 0;
    }
}

static ClkrstSession g_clk_cpu;
static int g_clk_active = 0;

static void switch_clk_boost(void) {
    if (R_SUCCEEDED(clkrstInitialize())) {
        if (R_SUCCEEDED(clkrstOpenSession(&g_clk_cpu, PcvModuleId_CpuBus, 3))) {
            clkrstSetClockRate(&g_clk_cpu, 1224000000);
            g_clk_active = 1;
        }
    }
}

static void switch_clk_restore(void) {
    if (g_clk_active) {
        clkrstCloseSession(&g_clk_cpu);
        clkrstExit();
        g_clk_active = 0;
    }
}
#endif

static void host_vibrate(int ms) {
    if (ms <= 0) return;
    if (ms > 500) ms = 500;
#ifdef __SWITCH__
    float amp = 0.6f;
    switch_vib_send(amp, 160.0f, amp * 0.8f, 320.0f);
    g_vib_stop_t = SDL_GetTicks64() + (Uint64)ms;
    g_vib_active = 1;
#else
    for (int i = 0; i < 8; i++) {
        if (g_joys[i] && SDL_JoystickGetAttached(g_joys[i])) {
            SDL_JoystickRumble(g_joys[i], 0x8000, 0x8000, (Uint32)ms);
        }
    }
#endif
}

static void host_vibrate_light(int ms) {
    if (ms <= 0) return;
#ifdef __SWITCH__
    float amp = 0.20f;
    switch_vib_send(amp, 200.0f, amp * 0.6f, 320.0f);
    g_vib_stop_t = SDL_GetTicks64() + (Uint64)ms;
    g_vib_active = 1;
#else
    for (int i = 0; i < 8; i++) {
        if (g_joys[i] && SDL_JoystickGetAttached(g_joys[i])) {
            SDL_JoystickRumble(g_joys[i], 0x3000, 0x3000, (Uint32)ms);
        }
    }
#endif
}
static uint32_t host_clock_us(void) { return (uint32_t)(SDL_GetTicks64() * 1000u + 1000000u); }

static int mkdirs(const char *path) {
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
    return mkdir(tmp, 0777);
}

static int g_hle_mode = 0, g_skip_check = 0;
/* The only supported game build: Destinia v1.06 (Gamevil, Android). Its native library is what runs, so that is what we pin;
 * this stays valid if the APK is merely re-signed. */
#define LIB_SHA256 "f12f1d20ab42646ef81df27516851acccb9fffe7be07a054db83890d992df7e9"

static int load_game(const char *apk_path, const char *save_dir, int virtual_time) {
    game_set_natives(g_hle_mode);
    g_apk = apk_open(apk_path);
    if (!g_apk) guest_fatal(10, "Cannot open the game APK:\n  %s\n\nCopy DESTINIA_v1_06.apk there.", apk_path);
    size_t len = 0; uint8_t *so = apk_read(g_apk, "lib/armeabi/libdestinia_jni.so", &len);
    if (!so) guest_fatal(11, "lib/armeabi/libdestinia_jni.so not found inside the APK.\nIs it the Destinia v1.06 APK?");
    if (!g_skip_check) {
        char sum[65]; sha256_hex(so, len, sum);
        if (strcmp(sum, LIB_SHA256))
            guest_fatal(13, "This is not Destinia v1.06 (the game library does not match).\n\n  found:    %s\n  expected: %s\n\nUse the original v1.06 APK (Gamevil, 2011).", sum, LIB_SHA256);
        if (apk_size(g_apk, "assets/0_en.ddt") < 0) guest_fatal(14, "The APK is missing the game data (assets/). Is it a stripped or damaged copy?");
    }
    mkdirs(save_dir);
    {
        const char *slots[] = {"msave.dat", "save0.dat", "save1.dat", "save2.dat", NULL};
        for (int i = 0; slots[i]; i++) {
            char p_tmp[512], p_dat[512];
            snprintf(p_tmp, sizeof(p_tmp), "%s/%s.tmp", save_dir, slots[i]);
            snprintf(p_dat, sizeof(p_dat), "%s/%s", save_dir, slots[i]);
            FILE *ft = fopen(p_tmp, "rb");
            if (ft) {
                fclose(ft);
                remove(p_dat);
                rename(p_tmp, p_dat);
                fprintf(stderr, "[save] recovered orphaned %s -> %s\n", p_tmp, p_dat);
            }
        }
    }
    static GameHost host;
    host.asset_size = host_asset_size; host.asset_read = host_asset_read;
    host.play_sound = host_play_sound; host.set_volume = host_set_volume; host.vibrate = host_vibrate;
    host.clock_us = virtual_time ? NULL : host_clock_us; host.save_dir = save_dir;
    if (game_load(so, len, &host)) return -1;
    free(so);
    return 0;
}

/* Coverage output (env COVERAGE=file): executed Thumb encoding classes, appended so several runs can be merged. */
static void coverage_start(void) { if (getenv("RING")) g_ring_on = 1; if (getenv("COVERAGE")) g_opcov_on = 1; }
static void coverage_dump(void) {
    const char *p = getenv("COVERAGE"); if (!p) return;
    FILE *f = fopen(p, "a"); if (!f) return;
    for (int i = 0; i < 0x111; i++) if (g_opcov[i]) fprintf(f, "%03x\n", i);
    fclose(f);
}

/* ----------------------------------------------------------------- md5 ---- */
typedef struct { uint32_t a, b, c, d; uint64_t len; uint8_t buf[64]; } Md5;
static const uint32_t K[64] = {
 0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
 0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
 0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
 0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
static const uint8_t S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
 4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
static void md5_block(Md5 *m, const uint8_t *p) {
    uint32_t M[16], a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 16; i++) M[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; } else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) & 15; } else { f = c ^ (b | ~d); g = (7*i) & 15; }
        f = f + a + K[i] + M[g]; a = d; d = c; c = b; b = b + ((f << S[i]) | (f >> (32 - S[i])));
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}
static void md5_hex(const uint8_t *data, size_t n, char out[33]) {
    Md5 m = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0, {0}}; size_t i = 0;
    for (; i + 64 <= n; i += 64) md5_block(&m, data + i);
    uint8_t tail[128] = {0}; size_t rem = n - i; memcpy(tail, data + i, rem); tail[rem] = 0x80;
    size_t tl = rem < 56 ? 64 : 128; uint64_t bits = (uint64_t)n * 8; memcpy(tail + tl - 8, &bits, 8);
    md5_block(&m, tail); if (tl == 128) md5_block(&m, tail + 64);
    uint32_t w[4] = {m.a, m.b, m.c, m.d};
    for (int k = 0; k < 16; k++) sprintf(out + k * 2, "%02x", (w[k / 4] >> (8 * (k % 4))) & 255);
}

/* ------------------------------------------------------- headless mode ---- */
static uint32_t rd16(uint32_t a) { uint16_t v; mem_read(a, &v, 2); return v; }
static int hero_state(void) { return (int)(rd16(mem_r32(LIB_BASE + 0x44278) + 0x4854)); }
static uint32_t screen_id(void) { return mem_r32(mem_r32(LIB_BASE + 0x44240)); }   /* game's current screen object */
typedef struct Ev { int frame, kind, a, b, c; } Ev;   /* kind 0 key(code,down) 1 touch(x,y,t) */
static void rgb565_to_ppm(const uint8_t *fb, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GAME_W, GAME_H);
    for (int i = 0; i < GAME_W * GAME_H; i++) {
        unsigned v = fb[2*i] | (fb[2*i+1] << 8), r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        unsigned char px[3] = {(unsigned char)((r << 3) | (r >> 2)), (unsigned char)((g << 2) | (g >> 4)), (unsigned char)((b << 3) | (b >> 2))};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
static void do_warp(int area, int sub);
static void pad_button(int b, int down);
static int run_headless(const char *apk, const char *save, int frames, const char *script, const char *manifest,
                        const char *dumpdir, int dump_every, int trace, int profile) {
    if (load_game(apk, save, 1)) return 1;
    coverage_start();
    game_set_trace(trace, 0);
    int pf_from = getenv("PROFILE_FROM") ? atoi(getenv("PROFILE_FROM")) : 0, pf_to = getenv("PROFILE_TO") ? atoi(getenv("PROFILE_TO")) : frames;
    Ev *evs = NULL; int nev = 0;
    if (script) {
        FILE *f = fopen(script, "r"); char ln[128];
        while (f && fgets(ln, sizeof ln, f)) {
            int fr, a, b, c; char w[16];
            if (sscanf(ln, "%d %15s %i %i %i", &fr, w, &a, &b, &c) >= 3) {
                evs = (Ev *)realloc(evs, (size_t)(nev + 1) * sizeof *evs);
                if (!strcmp(w, "key")) evs[nev++] = (Ev){fr, 0, a, b, 0};
                else if (!strcmp(w, "touch")) evs[nev++] = (Ev){fr, 1, a, b, c};
                else if (!strcmp(w, "warp")) evs[nev++] = (Ev){fr, 2, a, b, 0};
                else if (!strcmp(w, "page")) evs[nev++] = (Ev){fr, 3, a, 0, 0};
                else if (!strcmp(w, "button")) evs[nev++] = (Ev){fr, 4, a, b, 0};
            }
        }
        if (f) fclose(f);
    }
    int r = game_init(); fprintf(stderr, "initGame -> %d\n", r);
    FILE *mf = manifest ? fopen(manifest, "w") : NULL;
    Uint64 t0 = SDL_GetPerformanceCounter(); uint64_t prev_ic = 0; static uint64_t per_frame[1 << 16];
    for (int fr = 0; fr < frames; fr++) {
        if (profile && fr == pf_from) game_profile_start();
        if (profile && fr == pf_to) { game_profile_dump(profile); profile = 0; }
        for (int i = 0; i < nev; i++) if (evs[i].frame == fr) {
            if (evs[i].kind == 0) game_key(evs[i].a, evs[i].b);
            else if (evs[i].kind == 2) do_warp(evs[i].a, evs[i].b);
            else if (evs[i].kind == 3) game_set_hotbar_page(evs[i].a);
            else if (evs[i].kind == 4) pad_button(evs[i].a, evs[i].b);
            else game_touch(evs[i].a, evs[i].b, evs[i].c);
        }
        int d = game_run();
        if (fr < (1 << 16)) { per_frame[fr] = game_icount() - prev_ic; prev_ic = game_icount(); }
        char h[33]; md5_hex(game_frame(), GAME_FB_BYTES, h);
        if (mf) fprintf(mf, "%d %s\n", fr, h);
        { static uint32_t last_sc; uint32_t sc = screen_id(); if (sc != last_sc) { fprintf(stderr, "[screen] f%d: 0x%x -> 0x%x\n", fr, last_sc, sc); last_sc = sc; } }
        if (dumpdir && dump_every && fr % dump_every == 0) {
            if (hero_state() == 0) native_draw_hotbar_overlay(game_frame_ptr());
            char p[512]; snprintf(p, sizeof p, "%s/frame_%04d.ppm", dumpdir, fr); rgb565_to_ppm(game_frame(), p);
        }
        if (d == 0) { fprintf(stderr, "game requested exit at frame %d\n", fr); break; }
    }
    double secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    fprintf(stderr, "%d frames, %.2fs, %.1f M guest instr, %.1f MIPS\n", frames, secs, (double)game_icount() / 1e6, (double)game_icount() / 1e6 / secs);
    if (mf) fclose(mf);
    if (profile >= 0 && frames > 0 && getenv("FRAME_STATS")) {
        for (int k = 0; k < 8; k++) { int bi = 0; for (int i = 1; i < frames && i < (1 << 16); i++) if (per_frame[i] > per_frame[bi]) bi = i;
            fprintf(stderr, "worst frame %d: %.2f M instr\n", bi, (double)per_frame[bi] / 1e6); per_frame[bi] = 0; }
    }
    if (profile) game_profile_dump(profile);
    natives_report(); coverage_dump();
    return 0;
}

/* Warp the hero into map area_sub with the game's own sequence (checkWarpData) -- test tool. */
static void gcall(const char *n, int na, uint32_t a0, uint32_t a1, uint32_t a2) { uint32_t a[3] = {a0, a1, a2}; game_call(n, na, a); }
static void do_warp(int area, int sub) {
    gcall("setHeroPos", 3, (uint32_t)((10 * 0x100000 + 0x80000) >> 16), (uint32_t)((10 * 0x100000 + 0x80000) >> 16), 1);
    { uint8_t *p = mem_ptr(mem_r32(LIB_BASE + 0x4439c) + 0xc2); p[0] = (uint8_t)area; p[1] = 0; p[2] = (uint8_t)sub; p[3] = 0; }
    gcall("Map_freeData", 0, 0, 0, 0); gcall("Map_freeTile", 0, 0, 0, 0);
    gcall("Map_loadData", 3, (uint32_t)area, (uint32_t)sub, 1); gcall("Map_loadTile", 1, 0, 0, 0);
    gcall("setHeroAction", 2, 2, (uint32_t)-1, 0);
    { uint8_t *p = mem_ptr(mem_r32(LIB_BASE + 0x44278) + 0x489a); p[0] = 0xff; p[1] = 0xff; }
    gcall("setMapSound", 0, 0, 0, 0);
}

/* ------------------------------------------------------------- tour mode ---
 * Test tool: play the script to the first field screen, then warp the hero into every map of the game with the game's
 * own warp sequence (see checkWarpData), run frames with random input and look for crashes. */
static FILE *g_rec;   /* --tour-record: the same session as a script the Unicorn harness can replay (tools: harness/run_frames.py) */
static int run_tour(const char *apk, const char *save, const char *script, const char *outdir, int only_area, int only_sub, int frames_per_map) {
    if (getenv("TOUR_RECORD")) g_rec = fopen(getenv("TOUR_RECORD"), "w");
    if (load_game(apk, save, 1)) return 1;
    coverage_start();
    Ev *evs = NULL; int nev = 0; FILE *f = fopen(script, "r"); char ln[128];
    while (f && fgets(ln, sizeof ln, f)) { int fr, a, b, c; char w[16];
        if (sscanf(ln, "%d %15s %i %i %i", &fr, w, &a, &b, &c) >= 3) { evs = (Ev *)realloc(evs, (size_t)(nev + 1) * sizeof *evs);
            if (!strcmp(w, "key")) evs[nev++] = (Ev){fr, 0, a, b, 0}; else if (!strcmp(w, "touch")) evs[nev++] = (Ev){fr, 1, a, b, c}; } }
    if (f) fclose(f);
    game_init(); int fr = 0, lastev = 0; for (int i = 0; i < nev; i++) if (evs[i].frame > lastev) lastev = evs[i].frame;
    for (; fr < lastev + 60; fr++) { for (int i = 0; i < nev; i++) if (evs[i].frame == fr) { if (evs[i].kind == 0) game_key(evs[i].a, evs[i].b); else game_touch(evs[i].a, evs[i].b, evs[i].c); } game_run(); }
    fprintf(stderr, "[tour] in the field at frame %d, screen=0x%x\n", fr, mem_r32(mem_r32(LIB_BASE + 0x44240)));
    unsigned rng = 12345; static const int keys[] = {0xd, 0xe, 0xf, 0x10, 0x11, 0x11, 0x11, 2, 8, 4, 0xa};
    int ok = 0, nfault = 0;
    for (int area = 0; area < 3; area++) for (int sub = 0; sub < 70; sub++) {
        char nm[64]; snprintf(nm, sizeof nm, "m%d_%d.mpd", area, sub);
        if (host_asset_size(nm) < 0) continue;
        if (only_area >= 0 && (area != only_area || (only_sub >= 0 && sub != only_sub))) continue;
        /* a warp in the middle of a script/cutscene leaves the script running against the wrong map (that is what crashed
         * the first tour), so first let the game finish whatever it is doing: confirm through dialogs until it is back in play */
        for (int w = 0; w < 1500 && (int)(mem_r32(mem_r32(LIB_BASE + 0x44278) + 0x4854) & 0xffff) != 0; w++, fr++) {
            if (w % 8 == 0) { if (g_rec) fprintf(g_rec, "%d key 0x11 1\n%d key 0x11 0\n", fr, fr + 1); game_key(0x11, 1); game_run(); fr++; game_key(0x11, 0); }
            game_run();
        }
        fprintf(stderr, "[tour] warp to map %d_%d\n", area, sub); if (g_rec) { fprintf(g_rec, "%d warp %d %d\n", fr, area, sub); fflush(g_rec); }
        do_warp(area, sub);
        g_tour_catch = 1; int faulted = setjmp(g_tour_jb);
        if (faulted) { nfault++; fprintf(stderr, "[tour] FAULT in map %d_%d (frame %d)\n", area, sub, fr); continue; }
        for (int k = 0; k < frames_per_map; k++, fr++) {
            rng = rng * 1664525u + 1013904223u;
            if ((rng >> 24) % 6 == 0) { int key = keys[(rng >> 8) % (sizeof keys / sizeof *keys)]; if (g_rec) fprintf(g_rec, "%d key 0x%x 1\n%d key 0x%x 0\n", fr, key, fr + 1, key); game_key(key, 1); game_run(); fr++; game_key(key, 0); }
            { static int last_st = -1; int st = (int)mem_r32(mem_r32(LIB_BASE + 0x44278) + 0x4854) & 0xffff; if (st != last_st) { fprintf(stderr, "[tour] f%d state %d -> %d (map %d_%d)\n", fr, last_st, st, area, sub); last_st = st; } }
            if (game_run() == 0) { fprintf(stderr, "[tour] game quit at map %d_%d\n", area, sub); return 2; }
        }
        g_tour_catch = 0; char p[512]; snprintf(p, sizeof p, "%s/map_%d_%02d.ppm", outdir, area, sub); rgb565_to_ppm(game_frame(), p); ok++;
        if (getenv("TOUR_KILL")) { { static const int offs[] = {0x48f4, 0x48f8}; for (int q = 0; q < 2; q++) { mem_ptr(mem_r32(LIB_BASE + 0x44278) + (uint32_t)offs[q])[0] = 0; mem_ptr(mem_r32(LIB_BASE + 0x44278) + (uint32_t)offs[q])[1] = 0; } } for (int k = 0; k < 60; k++) game_run(); snprintf(p, sizeof p, "%s/map_%d_%02d_dead.ppm", outdir, area, sub); rgb565_to_ppm(game_frame(), p); }
        if (getenv("TOUR_SCAN")) { uint32_t base = mem_r32(LIB_BASE + 0x44278); int v = atoi(getenv("TOUR_SCAN"));
            for (uint32_t o = 0; o < 0x9000; o += 2) { uint16_t x; mem_read(base + o, &x, 2); if (x == v) fprintf(stderr, "[scan] gs+0x%x = %d\n", o, v); } }
        if (getenv("TOUR_CONFIRM")) {      /* answer whatever dialog is up (e.g. the death/purchase prompt) and look again */
            for (int n = 0; n < 8; n++) { game_key(0x11, 1); game_run(); game_key(0x11, 0); for (int k = 0; k < 90; k++) game_run();
                snprintf(p, sizeof p, "%s/map_%d_%02d_after%d.ppm", outdir, area, sub, n); rgb565_to_ppm(game_frame(), p); }
        }
    }
    fprintf(stderr, "[tour] %d maps visited, %d faulted\n", ok, nfault); coverage_dump(); return nfault != 0;
}


/* ---------------------------------------------------------- portal test ---
 * For every map: read its warp table (the game's own data), put the hero on each trigger tile and check that the warp works. */
static int cur_map(int *area, int *sub) { uint32_t st = mem_r32(LIB_BASE + 0x4439c); *area = (int)rd16(st + 0xc2); *sub = (int)rd16(st + 0xc4); return 0; }
static void settle(int *fr, int max) { for (int w = 0; w < max && hero_state() != 0; w++, (*fr)++) { if (w % 8 == 0) { game_key(0x11, 1); game_run(); (*fr)++; game_key(0x11, 0); } game_run(); } }
static int run_portals(const char *apk, const char *save, const char *script, const char *outdir) {
    (void)outdir;
    if (load_game(apk, save, 1)) return 1;
    coverage_start();
    Ev *evs = NULL; int nev = 0; FILE *f = fopen(script, "r"); char ln[128];
    while (f && fgets(ln, sizeof ln, f)) { int fr, a, b, c; char w[16];
        if (sscanf(ln, "%d %15s %i %i %i", &fr, w, &a, &b, &c) >= 3) { evs = (Ev *)realloc(evs, (size_t)(nev + 1) * sizeof *evs);
            if (!strcmp(w, "key")) evs[nev++] = (Ev){fr, 0, a, b, 0}; else if (!strcmp(w, "touch")) evs[nev++] = (Ev){fr, 1, a, b, c}; } }
    if (f) fclose(f);
    game_init(); int fr = 0, lastev = 0; for (int i = 0; i < nev; i++) if (evs[i].frame > lastev) lastev = evs[i].frame;
    for (; fr < lastev + 60; fr++) { for (int i = 0; i < nev; i++) if (evs[i].frame == fr) { if (evs[i].kind == 0) game_key(evs[i].a, evs[i].b); else game_touch(evs[i].a, evs[i].b, evs[i].c); } game_run(); }
    int total = 0, good = 0, wrong = 0, faults = 0;
    for (int area = 0; area < 3; area++) for (int sub = 0; sub < 70; sub++) {
        char nm[64]; snprintf(nm, sizeof nm, "m%d_%d.mpd", area, sub); if (host_asset_size(nm) < 0) continue;
        settle(&fr, 1500); do_warp(area, sub); for (int k = 0; k < 30; k++, fr++) game_run();
        uint32_t map = mem_r32(LIB_BASE + 0x44344); int n = (int)rd16(map + 0x2cc); uint32_t tab = mem_r32(map + 0x2d0);
        if (n > 64) n = 64;
        uint8_t snap[64][8]; for (int i = 0; i < n; i++) mem_read(tab + (uint32_t)i * 8, snap[i], 8);   /* the table is freed with the map */
        for (volatile int i = 0; i < n; i++) {
            uint8_t e[8]; memcpy(e, snap[i], 8); int da = e[2], ds = (int)(e[4] | e[5] << 8);
            if (getenv("PORTAL_DEBUG")) fprintf(stderr, "[portal] %d_%d entry %d: %02x %02x %02x %02x %02x %02x %02x %02x\n", area, sub, i, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
            g_tour_catch = 1; if (setjmp(g_tour_jb)) { faults++; total++; fprintf(stderr, "[portal] FAULT %d_%d #%d -> %d_%d\n", area, sub, i, da, ds); settle(&fr, 0); continue; }
            settle(&fr, 1500); do_warp(area, sub); for (int k = 0; k < 20; k++, fr++) game_run(); settle(&fr, 1500);
            gcall("setHeroPos", 3, (uint32_t)((e[0] * 0x100000 + 0x80000) >> 16), (uint32_t)((e[1] * 0x100000 + 0x80000) >> 16), 1);
            for (int k = 0; k < 600; k++, fr++) { if (k == 2) game_key(0xe, 1); if (k == 3) game_key(0xe, 0); if (k == 5) game_key(0xd, 1); if (k == 6) game_key(0xd, 0); game_run(); int a, s2; cur_map(&a, &s2); if ((a != area || s2 != sub) && hero_state() == 0) break; }
            settle(&fr, 1500); for (int k = 0; k < 30; k++, fr++) game_run();
            int a, s2; cur_map(&a, &s2); total++; g_tour_catch = 0;
            if (a == da && s2 == ds) good++; else { wrong++; fprintf(stderr, "[portal] %d_%d #%d expected %d_%d got %d_%d\n", area, sub, i, da, ds, a, s2); }
        }
        g_tour_catch = 0; fprintf(stderr, "[portal] map %d_%d: %d portals (total %d ok %d wrong %d fault %d)\n", area, sub, n, total, good, wrong, faults);
    }
    fprintf(stderr, "[portal] %d transitions: %d correct, %d to another map, %d faults\n", total, good, wrong, faults); coverage_dump();
    return faults != 0;
}

/* ---------------------------------------------------------- script test ---
 * Stage 5 test tool: trigger and execute all in-game bytecode scripts (1..58).
 * Checks that every script runs to completion, advances dialogs, handles state transitions,
 * frees its allocated script memory, and returns to normal field state without faulting. */
static int run_scripts(const char *apk, const char *save, const char *script, int start_id, int end_id) {
    if (load_game(apk, save, 1)) return 1;
    coverage_start();
    Ev *evs = NULL; int nev = 0; FILE *f = fopen(script, "r"); char ln[128];
    if (!f) { fprintf(stderr, "Cannot open script: %s\n", script); return 1; }
    while (fgets(ln, sizeof ln, f)) { int fr, a, b, c; char w[16];
        if (sscanf(ln, "%d %15s %i %i %i", &fr, w, &a, &b, &c) >= 3) { evs = (Ev *)realloc(evs, (size_t)(nev + 1) * sizeof *evs);
            if (!strcmp(w, "key")) evs[nev++] = (Ev){fr, 0, a, b, 0}; else if (!strcmp(w, "touch")) evs[nev++] = (Ev){fr, 1, a, b, c}; } }
    fclose(f);
    game_init(); int fr = 0, lastev = 0; for (int i = 0; i < nev; i++) if (evs[i].frame > lastev) lastev = evs[i].frame;
    for (; fr < lastev + 60; fr++) { for (int i = 0; i < nev; i++) if (evs[i].frame == fr) { if (evs[i].kind == 0) game_key(evs[i].a, evs[i].b); else game_touch(evs[i].a, evs[i].b, evs[i].c); } game_run(); }
    fprintf(stderr, "[scripts] hero in field at frame %d, screen=0x%x\n", fr, mem_r32(mem_r32(LIB_BASE + 0x44240)));
    settle(&fr, 1500);

    volatile int total = 0, passed = 0, faults = 0, timeouts = 0;
    if (start_id < 1) start_id = 1;
    if (end_id > 58) end_id = 58;
    if (start_id > end_id) { fprintf(stderr, "Invalid script range: %d..%d\n", start_id, end_id); return 1; }

    for (int sid = start_id; sid <= end_id; sid++) {
        uint32_t canvas = mem_r32(LIB_BASE + 0x44278);
        total++;
        settle(&fr, 1500);
        do_warp(0, 0);
        settle(&fr, 1500);

        fprintf(stderr, "[scripts] trigger script #%02d... ", sid);
        fflush(stderr);

        uint16_t s16 = (uint16_t)sid, st30 = 30, zero16 = 0;
        mem_write(canvas + 0x145e, &s16, 2);
        mem_write(canvas + 0x1464, &zero16, 2);
        mem_write(canvas + 0x4854, &st30, 2);

        g_tour_catch = 1;
        if (setjmp(g_tour_jb)) {
            faults++;
            fprintf(stderr, "FAULT (guest crash)\n");
            g_tour_catch = 0;
            gcall("freeScriptData", 0, 0, 0, 0);
            gcall("freeDialogData", 0, 0, 0, 0);
            uint16_t st0 = 0;
            mem_write(canvas + 0x4854, &st0, 2);
            continue;
        }

        int done = 0;
        int script_frames = 0;
        for (int k = 0; k < 3000; k++, fr++, script_frames++) {
            if (k % 4 == 0) {
                game_key(0x11, 1);
                game_run();
                fr++;
                game_key(0x11, 0);
            }
            int delay = game_run();
            if (delay == 0) {
                fprintf(stderr, "GAME QUIT at script #%02d\n", sid);
                coverage_dump();
                return 2;
            }
            if (k > 15 && hero_state() == 0) {
                done = 1;
                break;
            }
        }
        g_tour_catch = 0;
        if (done) {
            passed++;
            fprintf(stderr, "OK (%d frames)\n", script_frames);
        } else {
            timeouts++;
            fprintf(stderr, "TIMEOUT (hero_state=%d, screen=0x%x)\n", hero_state(), mem_r32(mem_r32(LIB_BASE + 0x44240)));
            gcall("freeScriptData", 0, 0, 0, 0);
            gcall("freeDialogData", 0, 0, 0, 0);
            uint16_t st0 = 0;
            mem_write(canvas + 0x4854, &st0, 2);
        }
    }
    fprintf(stderr, "[scripts] complete: %d total, %d passed, %d faults, %d timeouts\n", total, passed, faults, timeouts);
    coverage_dump();
    return (faults != 0 || timeouts != 0);
}


/* ---------------------------------------------------- key codes ---- */
#define KEY_OK     0x11   /* confirm / attack / talk */
#define KEY_BACK   0x1b   /* cancel; opens the character menu in the field */
#define KEY_SLOT0  0x02   /* quick slot 1 */
#define KEY_SLOT1  0x08   /* quick slot 2 */
#define KEY_SLOT2  0x0b   /* quick slot 3 */
#define KEY_SLOT3  0x04   /* quick slot 4 */
#define KEY_SLOT4  0x0a   /* quick slot 5 */
#define KEY_MAP    0x01   /* mini map */

/* ---------------------------------------------------- map profiler ---- */
static int run_profile_map(const char *apk, const char *save, int area, int sub, int frames, int open_map) {
    if (load_game(apk, save, 1)) return 1;
    coverage_start();
    game_init();
    Ev *evs = NULL; int nev = 0; FILE *f = fopen("harness/scripts/newgame3.txt", "r"); char ln[128];
    while (f && fgets(ln, sizeof ln, f)) {
        int fr, a, b, c; char w[16];
        if (sscanf(ln, "%d %15s %i %i %i", &fr, w, &a, &b, &c) >= 3) {
            evs = (Ev *)realloc(evs, (size_t)(nev + 1) * sizeof *evs);
            if (!strcmp(w, "key")) evs[nev++] = (Ev){fr, 0, a, b, 0};
            else if (!strcmp(w, "touch")) evs[nev++] = (Ev){fr, 1, a, b, c};
        }
    }
    if (f) fclose(f);
    int fr = 0, lastev = 0;
    for (int i = 0; i < nev; i++) if (evs[i].frame > lastev) lastev = evs[i].frame;
    for (; fr < lastev + 60; fr++) {
        for (int i = 0; i < nev; i++) if (evs[i].frame == fr) {
            if (evs[i].kind == 0) game_key(evs[i].a, evs[i].b);
            else game_touch(evs[i].a, evs[i].b, evs[i].c);
        }
        game_run();
    }
    free(evs);
    settle(&fr, 1500);
    fprintf(stderr, "[profile_map] warping to area %d sub %d...\n", area, sub);
    do_warp(area, sub);
    for (int k = 0; k < 30; k++, fr++) game_run();
    settle(&fr, 1500);

    if (open_map) {
        fprintf(stderr, "[profile_map] opening map (KEY_MAP = 0x01)...\n");
        game_key(KEY_MAP, 1); game_run(); fr++;
        game_key(KEY_MAP, 0); game_run(); fr++;
        for (int k = 0; k < 10; k++, fr++) game_run();
    }

    fprintf(stderr, "[profile_map] starting profile for %d frames (state=%d, screen=0x%x)...\n",
            frames, hero_state(), screen_id());
    game_profile_start();
    Uint64 t0 = SDL_GetTicks64();
    Uint64 ic0 = game_icount();
    Uint64 worst_frame_ic = 0;

    for (int k = 0; k < frames; k++, fr++) {
        Uint64 frame_ic0 = game_icount();
        int delay = game_run();
        Uint64 frame_ic = game_icount() - frame_ic0;
        if (frame_ic > worst_frame_ic) worst_frame_ic = frame_ic;
        if (k % 30 == 0) {
            fprintf(stderr, "  frame %d: delay=%d, ic=%.2f M\n", k, delay, (double)frame_ic / 1e6);
        }
    }
    Uint64 total_ms = SDL_GetTicks64() - t0;
    Uint64 total_ic = game_icount() - ic0;
    fprintf(stderr, "[profile_map] %d frames finished: total %.2f M instr (%.2f M/frame avg, worst %.2f M), time %llu ms (%.1f ms/frame)\n",
            frames, (double)total_ic / 1e6, (double)total_ic / 1e6 / frames, (double)worst_frame_ic / 1e6,
            (unsigned long long)total_ms, (double)total_ms / frames);
    game_profile_dump(25);
    natives_report();
    return 0;
}

/* ----------------------------------------------------------- GUI mode ---- */
/* ------------------------------------------------------------ controls ---
 * The game is touch-first: the hero walks with an on-screen joystick (touch drag on the left half),
 * menus are tapped. So: left stick / D-pad drive that virtual joystick, the right stick moves a
 * pointer and ZR taps it. Buttons use the game's own key codes (found by probing, see docs). */
#define KEY_OK     0x11   /* confirm / attack / talk */
#define KEY_BACK   0x1b   /* cancel; opens the character menu in the field */
#define KEY_SLOT0  0x02   /* quick slot 1 */
#define KEY_SLOT1  0x08   /* quick slot 2 */
#define KEY_SLOT2  0x0b   /* quick slot 3 */
#define KEY_SLOT3  0x04   /* quick slot 4 */
#define KEY_SLOT4  0x0a   /* quick slot 5 */
#define KEY_MAP    0x01   /* mini map */

static unsigned g_frame_no;
static void in_key(int code, int down) { game_key(code, down); }
static void in_touch(int x, int y, int type) { game_touch(x, y, type); }
static int g_shot_no;
static void screenshot(void) {   /* RS click / F12: dump the current frame next to the log */
    if (hero_state() == 0) native_draw_hotbar_overlay(game_frame_ptr());
#ifdef __SWITCH__
    char p[128]; snprintf(p, sizeof p, DEFAULT_DIR "/shots/shot_%04d.ppm", g_shot_no++); mkdirs(DEFAULT_DIR "/shots");
#else
    char p[128]; snprintf(p, sizeof p, "shot_%04d.ppm", g_shot_no++);
#endif
    rgb565_to_ppm(game_frame(), p); fprintf(stderr, "[shot] f%u screen=0x%x saved %s\n", g_frame_no, screen_id(), p); fflush(stderr);
}
typedef struct Pad { float lx, ly, rx, ry; int dl, du, dr, dd; } Pad;
static Pad g_pad;
static int g_ptr_x = 200, g_ptr_y = 120, g_ptr_visible;
static Uint64 g_ptr_t;

static void tap_press(int x, int y) { in_touch(x, y, 0); }
static void tap_release(int x, int y) { if (x < GAME_W / 2) in_touch(x, y, 1); }   /* the Java layer drops right-half releases */

/* Direction keys of the game (probed): 0xd up, 0xe down, 0xf left, 0x10 right. They move the hero in the
 * field and move the selection in menus that support it; the stick/D-pad are digitised to them. */
/* The game drops movement if a direction key is released while another one is still down, so a change of
 * direction is always sent as "release everything held, then press everything wanted". Diagonals are two keys.
 * The analogue stick is cut into 8 sectors (22.5 deg each way around the axes -> diagonals get a 45 deg sector). */
static void update_joystick(float ax, float ay, int digital) {
    static const int codes[4] = {0xd, 0xe, 0xf, 0x10};
    static int held[4];
    int want[4] = {0, 0, 0, 0};
    if (digital) { want[0] = ay < -0.5f; want[1] = ay > 0.5f; want[2] = ax < -0.5f; want[3] = ax > 0.5f; }
    else if (ax * ax + ay * ay > 0.35f * 0.35f) {
        float lim = 0.38f * sqrtf(ax * ax + ay * ay);          /* sin(22.5 deg): a component below this does not count */
        want[0] = ay < -lim; want[1] = ay > lim; want[2] = ax < -lim; want[3] = ax > lim;
        for (int i = 0; i < 4; i++) if (held[i] && !want[i]) {   /* hysteresis: keep a held axis a bit longer */
            float v = i < 2 ? (i == 0 ? -ay : ay) : (i == 2 ? -ax : ax);
            if (v > lim * 0.7f) want[i] = 1;
        }
    }
    if (!memcmp(want, held, sizeof held)) return;
    /* The game runs when the same direction key is pressed twice within 300 ms (dash). A change of direction without
     * going through neutral must never look like that, so before a repeat press a different key is tapped. */
    static int last_key = -1; static Uint64 last_t;
    int was_neutral = !(held[0] | held[1] | held[2] | held[3]), now_neutral = !(want[0] | want[1] | want[2] | want[3]);
    int adding = 1; for (int i = 0; i < 4; i++) if (held[i] && !want[i]) adding = 0;   /* only added keys: nothing to release */
    if (!adding) for (int i = 0; i < 4; i++) if (held[i]) in_key(codes[i], 0);
    for (int i = 0; i < 4; i++) if (want[i] && (!adding || !held[i])) {
        Uint64 now = SDL_GetTicks64();
        if (!was_neutral && !now_neutral && last_key == codes[i] && now - last_t < 350) {
            int d = codes[(i + 1) & 3]; in_key(d, 1); in_key(d, 0);
        }
        in_key(codes[i], 1); last_key = codes[i]; last_t = now;
    }
    memcpy(held, want, sizeof held);
}

static void pad_button(int b, int down) {
    int page = game_get_hotbar_page();
    switch (b) {                                  /* libnx SDL joystick: 0 A 1 B 2 X 3 Y 4 LS 5 RS 6 L 7 R 8 ZL 9 ZR 10 + 11 - 12-15 dpad L U R D */
    case 0: in_key(KEY_OK, down); break;          /* A: confirm / attack / talk */
    case 1: in_key(KEY_BACK, down); break;        /* B: cancel / dash / menu */
    case 6:
        if (down != page) {
            game_set_hotbar_page(down ? 1 : 0);
            if (down) host_vibrate_light(35);
        }
        break; /* L shoulder: Page Shift Modifier */
    case 3: if (down) game_trigger_quick_slot(page ? 5 : 0); break; /* Y: Combat Slot 1 */
    case 2: if (down) game_trigger_quick_slot(page ? 6 : 1); break; /* X: Combat Slot 2 */
    case 7: if (down) game_trigger_quick_slot(page ? 7 : 2); break; /* R: Combat Slot 3 */
    case 9:                                                        /* ZR: Combat Slot 4 (or pointer click in menu) */
        if (hero_state() != 0 && g_ptr_visible && SDL_GetTicks64() - g_ptr_t < 3000) {
            if (down) tap_press(g_ptr_x, g_ptr_y); else tap_release(g_ptr_x, g_ptr_y);
        } else {
            if (down) game_trigger_quick_slot(page ? 8 : 3);
        }
        break;
    case 8:                                                        /* ZL: Utility Slot / Potion (or pointer click in menu) */
        if (hero_state() != 0 && g_ptr_visible && SDL_GetTicks64() - g_ptr_t < 3000) {
            if (down) tap_press(g_ptr_x, g_ptr_y); else tap_release(g_ptr_x, g_ptr_y);
        } else {
            if (down) game_trigger_quick_slot(page ? 9 : 4);
        }
        break;
    case 4: if (down) game_trigger_quick_slot(page ? 9 : 4); break; /* LS (L3 click): quick slot 5/10 */
    case 5: if (down) screenshot(); break;                          /* RS (R3 click): screenshot */
    case 10: if (down) in_touch(0, 0, 3); break;                    /* Menu (+): save / settings */
    case 11: in_key(KEY_MAP, down); break;                          /* Minus (-): Mini Map */
    case 12: g_pad.dl = down; break; case 13: g_pad.du = down; break;
    case 14: g_pad.dr = down; break; case 15: g_pad.dd = down; break;
    }
}

static int run_gui(const char *apk, const char *save) {
    g_audio_on = 1;
    if (load_game(apk, save, 0)) return 1;
    audio_init(audio_loader); coverage_start();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow("Destinia", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!ren && win) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) guest_fatal(12, "SDL window/renderer failed: %s", SDL_GetError());
    SDL_RenderSetLogicalSize(ren, GAME_W, GAME_H);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, GAME_W, GAME_H);
#ifdef __SWITCH__
    switch_clk_boost();
    switch_vib_init();
#endif
    for (int i = 0; i < 8; i++) g_joys[i] = NULL;
    for (int i = 0; i < SDL_NumJoysticks() && i < 8; i++) g_joys[i] = SDL_JoystickOpen(i);
    int r = game_init(); fprintf(stderr, "initGame -> %d\n", r);
    int quit = 0, paused = 0; unsigned nframes = 0; Uint64 run_ms = 0, worst_ms = 0, last_ic = 0, win_t0 = SDL_GetTicks64();
    while (!quit) {
        Uint64 t0 = SDL_GetTicks64();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = 1;
            else if (e.type == SDL_APP_WILLENTERBACKGROUND || (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_MINIMIZED
#ifdef __SWITCH__
                     || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST
#endif
                     ))) {                                          /* HOME menu / minimised: freeze the game and the sound */
                if (!paused) {
                    paused = 1;
                    update_joystick(0, 0, 1);
                    audio_pause(1);
#ifdef __SWITCH__
                    switch_vib_stop();
#endif
                    fprintf(stderr, "[app] paused f%u\n", g_frame_no); fflush(stderr);
                }
            } else if (e.type == SDL_APP_DIDENTERFOREGROUND || (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED))) {
                if (paused) {
                    paused = 0;
                    audio_pause(0);
#ifdef __SWITCH__
                    switch_clk_boost();
#endif
                    fprintf(stderr, "[app] resumed f%u\n", g_frame_no); fflush(stderr);
                }
            }
            else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                int down = e.type == SDL_KEYDOWN;
                switch (e.key.keysym.sym) {                       /* PC keyboard, mirrors the pad */
                case SDLK_RETURN: case SDLK_SPACE: case SDLK_z: pad_button(0, down); break;
                case SDLK_x: pad_button(1, down); break;
                case SDLK_LSHIFT: case SDLK_TAB: pad_button(6, down); break;  /* Shift page */
                case SDLK_1: pad_button(8, down); break;                     /* ZL (potion) */
                case SDLK_2: pad_button(3, down); break;                     /* Y (skill 1) */
                case SDLK_3: pad_button(2, down); break;                     /* X (skill 2) */
                case SDLK_4: pad_button(7, down); break;                     /* R (skill 3) */
                case SDLK_5: pad_button(9, down); break;                     /* ZR (skill 4) */
                case SDLK_a: pad_button(2, down); break; case SDLK_s: pad_button(3, down); break;
                case SDLK_d: pad_button(6, down); break; case SDLK_f: pad_button(7, down); break;
                case SDLK_c: pad_button(4, down); break;
                case SDLK_m: pad_button(11, down); break;
                case SDLK_ESCAPE: pad_button(10, down); break;
                case SDLK_LEFT: g_pad.dl = down; break; case SDLK_UP: g_pad.du = down; break;
                case SDLK_RIGHT: g_pad.dr = down; break; case SDLK_DOWN: g_pad.dd = down; break;
                case SDLK_F12: if (down) screenshot(); break;
                case SDLK_q: quit = 1; break;
                }
            } else if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYBUTTONUP) {
                pad_button(e.jbutton.button, e.type == SDL_JOYBUTTONDOWN);
            } else if (e.type == SDL_JOYAXISMOTION) {
#ifndef __SWITCH__
                if (e.jaxis.axis >= 4) { static int zr; int on = e.jaxis.value > 16000; if (on != zr) { zr = on; pad_button(9, on); } }
#endif
            } else if (e.type == SDL_JOYHATMOTION) {
                g_pad.dl = !!(e.jhat.value & SDL_HAT_LEFT); g_pad.dr = !!(e.jhat.value & SDL_HAT_RIGHT);
                g_pad.du = !!(e.jhat.value & SDL_HAT_UP); g_pad.dd = !!(e.jhat.value & SDL_HAT_DOWN);
            } else if (e.type == SDL_JOYDEVICEADDED) {
                for (int i = 0; i < 8; i++) {
                    if (!g_joys[i]) {
                        g_joys[i] = SDL_JoystickOpen(e.jdevice.which);
                        break;
                    }
                }
            } else if (e.type == SDL_JOYDEVICEREMOVED) {
                for (int i = 0; i < 8; i++) {
                    if (g_joys[i] && SDL_JoystickInstanceID(g_joys[i]) == e.jdevice.which) {
                        SDL_JoystickClose(g_joys[i]);
                        g_joys[i] = NULL;
                        break;
                    }
                }
            } else if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERUP || e.type == SDL_FINGERMOTION) {
                int win_w = 1200, win_h = 720;
                SDL_GetWindowSize(win, &win_w, &win_h);
                float lx, ly;
                SDL_RenderWindowToLogical(ren, (int)(e.tfinger.x * (float)win_w), (int)(e.tfinger.y * (float)win_h), &lx, &ly);
                int gx = (int)lx, gy = (int)ly;
                if (e.type == SDL_FINGERDOWN) {
                    if (gx >= 0 && gx < GAME_W && gy >= 0 && gy < GAME_H) {
                        g_ptr_visible = 0;
                        tap_press(gx, gy);
                    }
                } else if (e.type == SDL_FINGERUP) {
                    int cx = gx < 0 ? 0 : (gx >= GAME_W ? GAME_W - 1 : gx);
                    int cy = gy < 0 ? 0 : (gy >= GAME_H ? GAME_H - 1 : gy);
                    tap_release(cx, cy);
                } else if (e.type == SDL_FINGERMOTION) {
                    if (gx >= 0 && gx < GAME_W / 2 && gy >= 0 && gy < GAME_H) {
                        in_touch(gx, gy, 2);
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.which != SDL_TOUCH_MOUSEID) {
                    float lx, ly; SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
                    int gx = (int)lx, gy = (int)ly;
                    if (e.type == SDL_MOUSEBUTTONDOWN) {
                        if (gx >= 0 && gx < GAME_W && gy >= 0 && gy < GAME_H) {
                            g_ptr_visible = 0;
                            tap_press(gx, gy);
                        }
                    } else {
                        int cx = gx < 0 ? 0 : (gx >= GAME_W ? GAME_W - 1 : gx);
                        int cy = gy < 0 ? 0 : (gy >= GAME_H ? GAME_H - 1 : gy);
                        tap_release(cx, cy);
                    }
                }
            } else if (e.type == SDL_MOUSEMOTION) {
                if (e.motion.which != SDL_TOUCH_MOUSEID && (e.motion.state & SDL_BUTTON_LMASK)) {
                    float lx, ly; SDL_RenderWindowToLogical(ren, e.motion.x, e.motion.y, &lx, &ly);
                    int gx = (int)lx, gy = (int)ly;
                    if (gx >= 0 && gx < GAME_W / 2 && gy >= 0 && gy < GAME_H) in_touch(gx, gy, 2);
                }
            }
        }
        /* analogue sticks: first pad that reports anything */
        g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0;
        for (int i = 0; i < 8; i++) if (g_joys[i] && SDL_JoystickGetAttached(g_joys[i])) {
            float v[4]; for (int a = 0; a < 4 && a < SDL_JoystickNumAxes(g_joys[i]); a++) v[a] = (float)SDL_JoystickGetAxis(g_joys[i], a) / 32767.0f;
            if (SDL_JoystickNumAxes(g_joys[i]) >= 4) { if (v[0] * v[0] + v[1] * v[1] > g_pad.lx * g_pad.lx + g_pad.ly * g_pad.ly) { g_pad.lx = v[0]; g_pad.ly = v[1]; }
                if (v[2] * v[2] + v[3] * v[3] > g_pad.rx * g_pad.rx + g_pad.ry * g_pad.ry) { g_pad.rx = v[2]; g_pad.ry = v[3]; } }
        }
        int dpad = g_pad.dr || g_pad.dl || g_pad.du || g_pad.dd;
        if (dpad) update_joystick((float)(g_pad.dr - g_pad.dl), (float)(g_pad.dd - g_pad.du), 1);   /* D-pad wins over the stick */
        else update_joystick(g_pad.lx, g_pad.ly, 0);
        if (g_pad.rx * g_pad.rx + g_pad.ry * g_pad.ry > 0.2f * 0.2f) {          /* pointer */
            g_ptr_x += (int)(g_pad.rx * 6);
            g_ptr_y += (int)(g_pad.ry * 6);
            if (g_ptr_x < 0) g_ptr_x = 0;
            if (g_ptr_x > GAME_W - 1) g_ptr_x = GAME_W - 1;
            if (g_ptr_y < 0) g_ptr_y = 0;
            if (g_ptr_y > GAME_H - 1) g_ptr_y = GAME_H - 1;
            g_ptr_visible = 1;
            g_ptr_t = SDL_GetTicks64();
        }
        if (paused) { SDL_Delay(50); continue; }
#ifdef __SWITCH__
        if (g_vib_active && SDL_GetTicks64() >= g_vib_stop_t) {
            switch_vib_stop();
        }
#endif
        Uint64 g0 = SDL_GetTicks64();
        int delay = game_run(); g_frame_no++;
        { static uint32_t last_scr = 1; uint32_t sc = screen_id(); if (sc != last_scr) { fprintf(stderr, "[screen] f%u 0x%x -> 0x%x delay=%d\n", g_frame_no, last_scr, sc, delay); last_scr = sc; fflush(stderr); } }
        Uint64 gms = SDL_GetTicks64() - g0; run_ms += gms; if (gms > worst_ms) worst_ms = gms;
        if (getenv("PERF") && ++nframes % 120 == 0) {          /* performance line for the log */
            Uint64 ic = game_icount(), wall = SDL_GetTicks64() - win_t0;
            fprintf(stderr, "[perf] ptr=%d,%d screen=0x%x delay=%d\n", g_ptr_x, g_ptr_y, screen_id(), delay);
            fprintf(stderr, "[perf] %u frames: game %.1f ms/frame avg, %llu ms worst, wall %.1f ms/frame, %.1f guest MIPS\n",
                    nframes, (double)run_ms / 120.0, (unsigned long long)worst_ms, (double)wall / 120.0,
                    (double)(ic - last_ic) / 1e3 / (double)(run_ms ? run_ms : 1));
            fflush(stderr); run_ms = worst_ms = 0; last_ic = ic; win_t0 = SDL_GetTicks64();
        }
        if (delay == 0) break;
        if (hero_state() == 0) native_draw_hotbar_overlay(game_frame_ptr());
        SDL_UpdateTexture(tex, NULL, game_frame(), GAME_W * 2);
        SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, NULL, NULL);
        if (g_ptr_visible && SDL_GetTicks64() - g_ptr_t < 3000) {               /* pointer overlay */
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderDrawLine(ren, g_ptr_x - 5, g_ptr_y, g_ptr_x + 5, g_ptr_y); SDL_RenderDrawLine(ren, g_ptr_x, g_ptr_y - 5, g_ptr_x, g_ptr_y + 5);
            SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
            SDL_RenderDrawLine(ren, g_ptr_x - 4, g_ptr_y, g_ptr_x + 4, g_ptr_y); SDL_RenderDrawLine(ren, g_ptr_x, g_ptr_y - 4, g_ptr_x, g_ptr_y + 4);
        }
        SDL_RenderPresent(ren);
        Uint64 used = SDL_GetTicks64() - t0;
        if (delay == 1000) delay = 100;
        if (delay < 16) delay = 16;
        if ((int)used < delay) {
            SDL_Delay((Uint32)(delay - (int)used));
        } else {
            SDL_Delay(1);
        }
    }
#ifdef __SWITCH__
    switch_vib_stop();
    switch_clk_restore();
#endif
    for (int i = 0; i < 8; i++) {
        if (g_joys[i]) { SDL_JoystickClose(g_joys[i]); g_joys[i] = NULL; }
    }
    audio_shutdown(); coverage_dump();
    return 0;
}

int main(int argc, char **argv) {
    const char *apk = DEFAULT_APK, *save = DEFAULT_SAVE, *script = NULL, *manifest = NULL, *dumpdir = NULL;
    int do_portals = 0, do_scripts = 0, do_prof_map = 0, prof_map_open = 0, prof_map_area = 0, prof_map_sub = 0, prof_map_frames = 120;
    int script_start = 1, script_end = 58; const char *tour_dir = NULL; int tour_area = -1, tour_sub = -1, tour_frames = 240; int show_pad = 0, headless = 0, frames = 100, dump_every = 0, trace = 0, profile = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--apk") && i + 1 < argc) apk = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) { dumpdir = argv[++i]; dump_every = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip-check")) g_skip_check = 1;
        else if (!strcmp(argv[i], "--tour") && i + 1 < argc) { tour_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--portals")) do_portals = 1;
        else if (!strcmp(argv[i], "--scripts")) do_scripts = 1;
        else if (!strcmp(argv[i], "--script-range") && i + 2 < argc) { do_scripts = 1; script_start = atoi(argv[++i]); script_end = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--tour-map") && i + 2 < argc) { tour_area = atoi(argv[++i]); tour_sub = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--tour-frames") && i + 1 < argc) tour_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile-map") && i + 4 < argc) {
            do_prof_map = 1; prof_map_area = atoi(argv[++i]); prof_map_sub = atoi(argv[++i]);
            prof_map_frames = atoi(argv[++i]); prof_map_open = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--hide-pad")) g_patch_hide_pad = 1;
        else if (!strcmp(argv[i], "--show-pad")) show_pad = 1;
        else if (!strcmp(argv[i], "--hle") && i + 1 < argc) g_hle_mode = atoi(argv[++i]);
    }
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777); mkdirs(DEFAULT_DIR);
    freopen(DEFAULT_DIR "/log.txt", "w", stderr);
#endif
    if (do_prof_map) { SDL_Init(0); return run_profile_map(apk, save, prof_map_area, prof_map_sub, prof_map_frames, prof_map_open); }
    if (do_scripts) { SDL_Init(0); return run_scripts(apk, save, script ? script : "harness/scripts/newgame3.txt", script_start, script_end); }
    if (do_portals) { SDL_Init(0); return run_portals(apk, save, script, ""); }
    if (tour_dir) { SDL_Init(0); return run_tour(apk, save, script, tour_dir, tour_area, tour_sub, tour_frames); }
    if (headless) { SDL_Init(0); return run_headless(apk, save, frames, script, manifest, dumpdir, dump_every, trace, profile); }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (!show_pad) g_patch_hide_pad = 1;
    int rc = run_gui(apk, save);
    SDL_Quit();
    return rc;
}
