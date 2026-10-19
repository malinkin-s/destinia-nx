/* audio.c -- replaces the Java MediaPlayer code of GameThread (playSound/setSoundVolume). MIT.
 * The original divides cur/max with integer arithmetic (3/5 -> 0), so only the maximum setting was
 * audible on Android; here the ratio is real. */
#include "audio.h"
#include <SDL.h>
#include <SDL_mixer.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_EFF 64
static AudioLoader g_load;
static Mix_Chunk *g_eff[MAX_EFF];
static Mix_Music *g_music; static uint8_t *g_music_data; static int g_music_id = -1;
static float g_vol[2] = {1.0f, 1.0f};
static int g_ok;

int audio_init(AudioLoader loader) {
    g_load = loader;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 || Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
        fprintf(stderr, "audio disabled: %s\n", SDL_GetError()); return -1;
    }
    Mix_Init(MIX_INIT_OGG | MIX_INIT_MP3);
    Mix_AllocateChannels(8);
    g_ok = 1; return 0;
}

static void apply_music_volume(void) { if (g_ok) Mix_VolumeMusic((int)(g_vol[0] * MIX_MAX_VOLUME)); }

void audio_volume(int type, int cur, int max) {
    if (type < 0 || type > 1 || max <= 0) return;
    g_vol[type] = (float)cur / (float)max;
    if (type == 0) apply_music_volume();
}

void audio_play(int id) {
    if (!g_ok || id < 0) return;
    char path[64];
    if (id < 100) {
        if (id >= MAX_EFF) return;
        if (!g_eff[id]) {
            snprintf(path, sizeof path, "res/raw/eff%02d.ogg", id);
            size_t len; uint8_t *d = g_load(path, &len); if (!d) return;
            g_eff[id] = Mix_LoadWAV_RW(SDL_RWFromConstMem(d, (int)len), 1);   /* decoded copy; source freed below */
            free(d);
            if (!g_eff[id]) { fprintf(stderr, "cannot decode %s: %s\n", path, Mix_GetError()); return; }
        }
        Mix_VolumeChunk(g_eff[id], (int)(g_vol[1] * MIX_MAX_VOLUME));
        Mix_PlayChannel(-1, g_eff[id], 0);
    } else {
        if (id == g_music_id && Mix_PlayingMusic()) return;
        snprintf(path, sizeof path, "res/raw/bg%03d.mp3", id);
        size_t len; uint8_t *d = g_load(path, &len); if (!d) return;
        Mix_HaltMusic(); if (g_music) Mix_FreeMusic(g_music); free(g_music_data);
        g_music_data = d;
        g_music = Mix_LoadMUS_RW(SDL_RWFromConstMem(d, (int)len), 0);
        if (!g_music) { fprintf(stderr, "cannot decode %s: %s\n", path, Mix_GetError()); return; }
        g_music_id = id; apply_music_volume(); Mix_PlayMusic(g_music, -1);
    }
}

void audio_pause(int on) { if (!g_ok) return; if (on) { Mix_PauseMusic(); Mix_Pause(-1); } else { Mix_ResumeMusic(); Mix_Resume(-1); } }
void audio_shutdown(void) { if (!g_ok) return; Mix_HaltMusic(); Mix_CloseAudio(); Mix_Quit(); g_ok = 0; }
