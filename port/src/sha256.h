#ifndef SHA256_H
#define SHA256_H
#include <stddef.h>
#include <stdint.h>
void sha256_hex(const uint8_t *data, size_t n, char out[65]);
#endif
