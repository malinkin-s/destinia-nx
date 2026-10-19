/* audio.h -- sound effects and music through SDL2_mixer, loaded from the player's APK. MIT. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stddef.h>
#include <stdint.h>
typedef uint8_t *(*AudioLoader)(const char *path, size_t *len);   /* malloc'd bytes from the APK */
int  audio_init(AudioLoader loader);        /* 0 = ok; failure leaves the game silent */
void audio_play(int id);                    /* <100 effect res/raw/effNN.ogg, >=100 looped music res/raw/bgNNN.mp3 */
void audio_volume(int type, int cur, int max);   /* type 0 = music, 1 = effects */
void audio_pause(int on);
void audio_shutdown(void);
#endif
