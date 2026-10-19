/* isatest: differential single-instruction test of the interpreter against a reference (Unicorn) -- see tools/isa_diff.py.
 * Reads cases (instruction halfwords + register/flag state), runs each once on the fixed test memory image and writes
 * the resulting state. Build: make isatest. */
#include "guest.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define CODE_PAGE LIB_BASE
#define DATA_PAGE HEAP_BASE
#define DATA_SIZE 0x2000
void cpu_step_one(Cpu *c, uint32_t pc, int thumb);

static jmp_buf g_jb; static int g_in_test;
void guest_fatal(int code, const char *fmt, ...) {
    (void)code; (void)fmt;
    if (g_in_test) longjmp(g_jb, 1);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(2);
}
static void pattern(uint8_t *p, size_t n, uint32_t salt) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 167u + (i >> 8) * 31u + salt); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: isatest cases.bin results.bin\n"); return 1; }
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb"); if (!fi || !fo) return 1;
    uint32_t n; if (fread(&n, 4, 1, fi) != 1) return 1;
    mem_map(CODE_PAGE, 0x1000); mem_map(DATA_PAGE, DATA_SIZE);
    uint8_t code[0x200], data[DATA_SIZE]; pattern(code, sizeof code, 13); pattern(data, sizeof data, 77);
    for (uint32_t k = 0; k < n; k++) {
        uint16_t ops[2]; uint32_t regs[16]; uint8_t fl;
        if (fread(ops, 2, 2, fi) != 2 || fread(regs, 4, 16, fi) != 16 || fread(&fl, 1, 1, fi) != 1) return 1;
        mem_write(CODE_PAGE, code, sizeof code); mem_write(CODE_PAGE + 0x100, ops, 4); mem_write(DATA_PAGE, data, DATA_SIZE);
        Cpu c; memset(&c, 0, sizeof c); memcpy(c.r, regs, sizeof regs);
        c.n = fl >> 3 & 1; c.z = fl >> 2 & 1; c.c = fl >> 1 & 1; c.v = fl & 1;
        uint8_t fault = 0; g_in_test = 1;
        if (setjmp(g_jb) == 0) {
            cpu_step_one(&c, CODE_PAGE + 0x100, 1);
            if ((ops[0] >> 11) == 0x1e) cpu_step_one(&c, c.r[15], 1);       /* BL is a prefix/suffix pair here */
        } else fault = 1;
        g_in_test = 0;
        uint8_t buf[DATA_SIZE]; mem_read(DATA_PAGE, buf, DATA_SIZE);
        uint32_t crc = (uint32_t)crc32(0, buf, DATA_SIZE);
        uint8_t oflags = (uint8_t)(c.n << 3 | c.z << 2 | c.c << 1 | c.v), thumb = (uint8_t)(c.thumb != 0);
        fwrite(&fault, 1, 1, fo); fwrite(c.r, 4, 16, fo); fwrite(&oflags, 1, 1, fo); fwrite(&thumb, 1, 1, fo); fwrite(&crc, 4, 1, fo);
    }
    fclose(fo); return 0;
}
