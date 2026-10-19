/* apk.h -- read-only access to files inside the player's APK. MIT. */
#ifndef APK_H
#define APK_H
#include <stddef.h>
#include <stdint.h>
typedef struct Apk Apk;
Apk *apk_open(const char *path);
void apk_close(Apk *a);
int apk_size(Apk *a, const char *name);                       /* -1 if missing */
uint8_t *apk_read(Apk *a, const char *name, size_t *out_len); /* malloc'd or NULL */
#endif
