/* audio_stub.c -- silent fallback used by the PC build when SDL2_mixer is not installed. MIT. */
#include "audio.h"
int audio_init(AudioLoader loader) { (void)loader; return -1; }
void audio_play(int id) { (void)id; }
void audio_volume(int type, int cur, int max) { (void)type; (void)cur; (void)max; }
void audio_pause(int on) { (void)on; }
void audio_shutdown(void) {}
