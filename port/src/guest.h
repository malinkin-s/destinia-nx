/* guest.h -- ARMv5/Thumb-1 guest CPU, flat paged guest memory, HLE hooks.
 *
 * The game's libdestinia_jni.so is 100% Thumb-1 (plus an ARM PLT). We run it in
 * an interpreter and implement its 28 libc imports and the JNIEnv in C (hle.c).
 * The same code builds for PC (SDL2) and Switch (devkitA64 + SDL2). MIT. */
#ifndef GUEST_H
#define GUEST_H
#include <stdint.h>
#include <stddef.h>

/* ---- guest address map (same as the Unicorn reference harness so that
 * pointer values, and with them frame hashes, are comparable) ---- */
#define LIB_BASE    0x10000000u
#define HEAP_BASE   0x20000000u
#define HEAP_SIZE   0x04000000u   /* 64 MiB, as in the reference harness (peak use is measured below) */
#define STUB_BASE   0x30000000u   /* HLE trap addresses (never mapped)   */
#define STUB_SIZE   0x00010000u
#define JNI_BASE    0x31000000u   /* JNIEnv + function tables            */
#define JNI_SIZE    0x00010000u
#define RET_MAGIC   0x3ffff000u   /* returning here ends cpu_call()      */
#define STACK_TOP   0x7f100000u
#define STACK_SIZE  0x00100000u

/* Fatal-error hook implemented by the frontend (shows the message, then exits). */
void guest_fatal(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

typedef struct Cpu {
    uint32_t r[16];               /* r[15] is only valid between cpu_call()s */
    uint32_t n, z, c, v;          /* flags as 0/1 */
    int thumb;                    /* current instruction set state */
    int fault;                    /* set by a fatal condition */
    uint64_t icount;              /* retired guest instructions */
} Cpu;

/* ---- paged guest memory ---- */
extern uint8_t *g_pages[1u << 20];            /* 4 KiB pages -> host pointer */
void  mem_map(uint32_t guest, uint32_t size); /* zero-filled, page aligned   */
void  mem_write(uint32_t guest, const void *src, uint32_t n);
void  mem_read(uint32_t guest, void *dst, uint32_t n);
uint32_t mem_r32(uint32_t a);
void  mem_w32(uint32_t a, uint32_t v);
uint8_t mem_r8(uint32_t a);
int   mem_cstr(uint32_t a, char *dst, int max); /* returns length */
uint8_t *mem_ptr(uint32_t a);                 /* host pointer, a must be mapped */

/* ---- symbols / heap (hle.c) ---- */
uint32_t guest_sym(const char *name);        /* address of an exported guest symbol (bit0 = Thumb) */
uint32_t guest_heap_end(void);
uint32_t guest_lib_span(void);
void natives_install(int verify_every);     /* native.c: register overrides (0 = off, -1 = none) */
void native_draw_hotbar_overlay(uint16_t *fb);
void native_draw_title_overlay(uint16_t *fb);
void native_draw_perf_overlay(uint16_t *fb, const char *text);
uint16_t *game_frame_ptr(void);
uint8_t *economy_patch_asset(const char *name, uint8_t *data, size_t *len);
void economy_install(void);
void economy_step(void);

/* ---- CPU ---- */
typedef uint32_t (*hle_fn)(Cpu *c);           /* returns r0 (r1 for 64-bit via c->r[1]) */
extern hle_fn g_stub_fn[STUB_SIZE / 8];       /* indexed by (pc-STUB_BASE)/8 */
extern const char *g_stub_name[STUB_SIZE / 8];
/* Call guest code at `addr` (bit0 = Thumb) with up to 4 reg args + stack args.
 * Returns r0. Aborts the process (after logging) on an unimplemented opcode. */
typedef int (*native_fn)(Cpu *c);   /* 0 = not handled, 1 = handled (return to lr), 2 = handled (continue at c->r[15]) */
extern int g_ring_on; void cpu_dump_ring(void);   /* crash reports: last executed pcs */
extern uint8_t g_opcov[0x120]; extern int g_opcov_on;   /* instruction-class coverage */
extern uint8_t *g_hookmap; extern uint32_t g_hook_span; extern native_fn g_hook_fn[256]; extern int g_hooks_on, g_hook_cur;
void cpu_run_original(Cpu *c, uint32_t entry, uint32_t stop_pc);
uint32_t cpu_call(Cpu *c, uint32_t addr, int nargs, const uint32_t *args);
void  cpu_set_trace(int on);
extern uint32_t *g_prof;                      /* per-halfword instruction counters (NULL = off) */
extern uint32_t g_prof_base, g_prof_span;
uint32_t cpu_arg(Cpu *c, int i);              /* i-th argument inside an HLE call */

#endif
