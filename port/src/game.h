/* game.h -- host-independent game runner: loads libdestinia_jni.so into the
 * guest, implements its libc + JNI imports and exposes the JNI entry points. MIT. */
#ifndef GAME_API_H
#define GAME_API_H
#include <stdint.h>
#include <stddef.h>

#define GAME_W 400
#define GAME_H 240
#define GAME_FB_BYTES (GAME_W * GAME_H * 2)   /* RGB565 little endian */

typedef struct GameHost {
    /* assets: names are relative to the APK's assets/ dir; read returns malloc'd data */
    int      (*asset_size)(const char *name);
    uint8_t *(*asset_read)(const char *name, size_t *len);
    void     (*play_sound)(int id);               /* <100 effect, >=100 background */
    void     (*set_volume)(int type, int cur, int max);
    void     (*vibrate)(int ms);
    uint32_t (*clock_us)(void);                   /* NULL = deterministic virtual clock */
    const char *save_dir;                         /* where msave.dat / saveN.dat live */
} GameHost;

int  game_load(const uint8_t *so, size_t len, const GameHost *host); /* 0 = ok */
int  game_init(void);                             /* GameThread.initGame; returns native result */
int  game_run(void);                              /* one jniRun tick; returns delay in ms (0 = quit) */
void game_key(int code, int down);                /* MMain_keyPressed / MMain_keyReleased */
void game_touch(int x, int y, int type);          /* jniTouch (0 down,1 up,2 move,3 menu) */
const uint8_t *game_frame(void);                  /* GAME_FB_BYTES of RGB565 */
void game_set_trace(int count_or_zero, int log_libc);
uint64_t game_icount(void);
void game_set_natives(int mode);                   /* before game_load: -1 off, 0 on, N verify every Nth call */
void natives_report(void);
uint32_t game_call(const char *sym, int nargs, const uint32_t *args);   /* test tools: call an exported guest function between frames */
void game_profile_start(void);
void game_profile_dump(int top);
void game_trigger_quick_slot(int slot_idx);
void game_set_hotbar_page(int page);
int  game_get_hotbar_page(void);

#endif
