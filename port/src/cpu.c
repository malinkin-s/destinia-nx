/* cpu.c -- ARMv5T interpreter: full Thumb-1, small ARM subset (PLT stubs),
 * paged guest memory and HLE trap dispatch. MIT.
 *
 * Only what libdestinia_jni.so needs: its code is 100% Thumb-1, and the only ARM
 * code it ever runs is the .plt (add/ldr pc). Unaligned word/halfword accesses
 * behave like ARMv6/v7 (SCTLR.U=1), which is what the game ran on. */
#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __GNUC__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

uint8_t *g_pages[1u << 20];
hle_fn g_stub_fn[STUB_SIZE / 8];
const char *g_stub_name[STUB_SIZE / 8];
uint32_t *g_prof; uint32_t g_prof_base, g_prof_span;
static int g_trace;
static uint64_t g_trace_left;
void cpu_set_trace(int n) { g_trace = n != 0; g_trace_left = (uint64_t)n; }

/* ------------------------------------------------------------ memory ---- */
void mem_map(uint32_t guest, uint32_t size) {
    uint32_t pages = (size + 0xfff) >> 12;
    uint8_t *host = (uint8_t *)calloc(pages, 4096);
    if (!host) guest_fatal(2, "out of memory mapping %u bytes", size);
    for (uint32_t i = 0; i < pages; i++) g_pages[(guest >> 12) + i] = host + (size_t)i * 4096;
}
static void fatal_mem(uint32_t a, const char *what) { guest_fatal(3, "guest memory fault: %s at %08x", what, a); }
uint8_t *mem_ptr(uint32_t a) {
    uint8_t *p = g_pages[a >> 12];
    if (!p) fatal_mem(a, "ptr");
    return p + (a & 0xfff);
}
void mem_write(uint32_t guest, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint32_t chunk = 4096 - (guest & 0xfff); if (chunk > n) chunk = n;
        memcpy(mem_ptr(guest), s, chunk); guest += chunk; s += chunk; n -= chunk;
    }
}
void mem_read(uint32_t guest, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        uint32_t chunk = 4096 - (guest & 0xfff); if (chunk > n) chunk = n;
        memcpy(d, mem_ptr(guest), chunk); guest += chunk; d += chunk; n -= chunk;
    }
}
uint8_t mem_r8(uint32_t a) { return *mem_ptr(a); }
uint32_t mem_r32(uint32_t a) {
    uint32_t v; mem_read(a, &v, 4); return v;     /* host is little-endian */
}
void mem_w32(uint32_t a, uint32_t v) { mem_write(a, &v, 4); }
int mem_cstr(uint32_t a, char *dst, int max) {
    int i = 0;
    for (; i < max - 1; i++) { char ch = (char)mem_r8(a + i); dst[i] = ch; if (!ch) return i; }
    dst[i] = 0; return i;
}

/* fast accessors used by the interpreter */
static inline uint32_t ld8(uint32_t a)  { uint8_t *p = g_pages[a >> 12]; if (UNLIKELY(!p)) fatal_mem(a, "read8"); return p[a & 0xfff]; }
static inline uint32_t ld16(uint32_t a) {
    uint8_t *p = g_pages[a >> 12];
    if (UNLIKELY(!p)) fatal_mem(a, "read16");
    if (LIKELY((a & 0xfff) <= 0xffe)) { uint16_t v; memcpy(&v, p + (a & 0xfff), 2); return v; }
    return ld8(a) | (ld8(a + 1) << 8);
}
static inline uint32_t ld32(uint32_t a) {
    uint8_t *p = g_pages[a >> 12];
    if (UNLIKELY(!p)) fatal_mem(a, "read32");
    if (LIKELY((a & 0xfff) <= 0xffc)) { uint32_t v; memcpy(&v, p + (a & 0xfff), 4); return v; }
    return ld8(a) | (ld8(a + 1) << 8) | (ld8(a + 2) << 16) | (ld8(a + 3) << 24);
}
static inline void st8(uint32_t a, uint32_t v)  { uint8_t *p = g_pages[a >> 12]; if (UNLIKELY(!p)) fatal_mem(a, "write8"); p[a & 0xfff] = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) {
    uint8_t *p = g_pages[a >> 12];
    if (UNLIKELY(!p)) fatal_mem(a, "write16");
    if (LIKELY((a & 0xfff) <= 0xffe)) { uint16_t x = (uint16_t)v; memcpy(p + (a & 0xfff), &x, 2); return; }
    st8(a, v); st8(a + 1, v >> 8);
}
static inline void st32(uint32_t a, uint32_t v) {
    uint8_t *p = g_pages[a >> 12];
    if (UNLIKELY(!p)) fatal_mem(a, "write32");
    if (LIKELY((a & 0xfff) <= 0xffc)) { memcpy(p + (a & 0xfff), &v, 4); return; }
    st8(a, v); st8(a + 1, v >> 8); st8(a + 2, v >> 16); st8(a + 3, v >> 24);
}

/* ------------------------------------------------------------- ALU ------ */
static inline uint32_t ror32(uint32_t x, unsigned n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }

typedef struct Flags { uint32_t n, z, c, v; } Flags;

#define SETNZ(res) do { F.n = (uint32_t)(res) >> 31; F.z = ((res) == 0); } while (0)

static inline uint32_t add_f(Flags *f, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t s = (uint64_t)a + b + cin; uint32_t r = (uint32_t)s;
    f->c = (uint32_t)(s >> 32); f->v = ((a ^ r) & (b ^ r)) >> 31;
    f->n = r >> 31; f->z = r == 0; return r;
}
static inline uint32_t sub_f(Flags *f, uint32_t a, uint32_t b, uint32_t cin) { /* a - b - !cin */
    return add_f(f, a, ~b, cin);
}

/* register-specified shifts (amount in Rs low byte); carry unchanged if amount==0 */
static inline uint32_t lsl_r(Flags *f, uint32_t v, uint32_t amt) {
    amt &= 255; if (!amt) return v;
    if (amt < 32) { f->c = (v >> (32 - amt)) & 1; return v << amt; }
    f->c = (amt == 32) ? (v & 1) : 0; return 0;
}
static inline uint32_t lsr_r(Flags *f, uint32_t v, uint32_t amt) {
    amt &= 255; if (!amt) return v;
    if (amt < 32) { f->c = (v >> (amt - 1)) & 1; return v >> amt; }
    f->c = (amt == 32) ? (v >> 31) : 0; return 0;
}
static inline uint32_t asr_r(Flags *f, uint32_t v, uint32_t amt) {
    amt &= 255; if (!amt) return v;
    if (amt < 32) { f->c = ((int32_t)v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
    f->c = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t ror_r(Flags *f, uint32_t v, uint32_t amt) {
    amt &= 255; if (!amt) return v;
    amt &= 31;
    if (!amt) { f->c = v >> 31; return v; }
    f->c = (v >> (amt - 1)) & 1; return ror32(v, amt);
}

static inline int cond_pass(const Flags *f, uint32_t cc) {
    switch (cc) {
    case 0: return f->z;              case 1: return !f->z;
    case 2: return f->c;              case 3: return !f->c;
    case 4: return f->n;              case 5: return !f->n;
    case 6: return f->v;              case 7: return !f->v;
    case 8: return f->c && !f->z;     case 9: return !f->c || f->z;
    case 10: return f->n == f->v;     case 11: return f->n != f->v;
    case 12: return !f->z && f->n == f->v;
    case 13: return f->z || f->n != f->v;
    default: return 1;
    }
}

/* ---------------------------------------------------------- core --------- */
uint32_t cpu_arg(Cpu *c, int i) { return i < 4 ? c->r[i] : mem_r32(c->r[13] + 4 * (uint32_t)(i - 4)); }

static void trace_line(const Cpu *c, uint32_t pc, const Flags *f) {
    fprintf(stderr, "%08x", pc);
    for (int i = 0; i < 13; i++) fprintf(stderr, " %08x", c->r[i]);
    fprintf(stderr, " %08x %08x %x%x%x%x\n", c->r[13], c->r[14], f->n, f->z, f->c, f->v);
}

static void bad_op(const char *what, uint32_t pc, uint32_t op) {
    guest_fatal(4, "unimplemented %s opcode %08x at guest pc %08x", what, op, pc);
}

/* shifter operand for ARM data processing (register form, immediate or reg amount) */
static uint32_t arm_shift(Flags *f, uint32_t v, uint32_t type, uint32_t amt, int by_reg, int set_c) {
    Flags tmp = *f; uint32_t res;
    if (by_reg) {
        switch (type) {
        case 0: res = lsl_r(&tmp, v, amt); break; case 1: res = lsr_r(&tmp, v, amt); break;
        case 2: res = asr_r(&tmp, v, amt); break; default: res = ror_r(&tmp, v, amt); break;
        }
    } else {
        switch (type) {
        case 0: if (!amt) { res = v; } else { tmp.c = (v >> (32 - amt)) & 1; res = v << amt; } break;
        case 1: if (!amt) { tmp.c = v >> 31; res = 0; } else { tmp.c = (v >> (amt - 1)) & 1; res = v >> amt; } break;
        case 2: if (!amt) { tmp.c = v >> 31; res = (uint32_t)((int32_t)v >> 31); }
                else { tmp.c = ((int32_t)v >> (amt - 1)) & 1; res = (uint32_t)((int32_t)v >> amt); } break;
        default: if (!amt) { tmp.c = v & 1; res = (v >> 1) | (f->c << 31); }               /* RRX */
                 else { tmp.c = (v >> (amt - 1)) & 1; res = ror32(v, amt); } break;
        }
    }
    if (set_c) f->c = tmp.c;
    return res;
}

/* native (HLE) overrides of hot guest functions, looked up at function entry */
uint8_t *g_hookmap;                 /* one byte per halfword of the library: 0 = none, else index into g_hook_fn */
uint32_t g_hook_span;
native_fn g_hook_fn[256];
int g_hooks_on = 1;
uint8_t g_opcov[0x120]; int g_opcov_on;     /* Thumb encoding classes executed (see tools/audit_isa.py for the key) */
uint32_t g_stop_pc;
int g_ring_on; static uint32_t g_ring[256], g_ring_i, g_ring_lr[256];   /* last executed pcs, for crash reports */
void cpu_dump_ring(void) { for (int k = 0; k < 64; k++) { uint32_t i = (g_ring_i + 256 - 64 + (uint32_t)k) & 255; fprintf(stderr, "  pc=%08x (lib+%x) lr=%08x\n", g_ring[i], g_ring[i] - LIB_BASE, g_ring_lr[i]); } }                 /* nonzero: exec_loop returns when pc reaches it */
int g_hook_cur;                     /* index of the hook being called (for the verifier) */

#ifdef ISA_TEST
static int g_single, g_single_done; static uint32_t g_final_pc; static int g_final_thumb;
#endif
static void exec_loop(Cpu *cpu, uint32_t pc, int thumb) {
    uint32_t *r = cpu->r;
    Flags F = { cpu->n, cpu->z, cpu->c, cpu->v };
    uint64_t ic = 0;

    for (;;) {
#ifdef ISA_TEST                                              /* single-step mode for the differential instruction test */
        if (g_single) { if (g_single_done) { g_final_pc = pc; g_final_thumb = thumb; break; } g_single_done = 1; }
#endif
        if (UNLIKELY((pc >> 28) == 3)) {                       /* HLE trap or return */
            if (pc == RET_MAGIC) break;
            uint32_t idx = (pc - STUB_BASE) >> 3;
            if (pc < STUB_BASE || idx >= STUB_SIZE / 8 || !g_stub_fn[idx]) {
                guest_fatal(5, "jump to unmapped special address %08x (lr=%08x)", pc, r[14]);
            }
            if (UNLIKELY(g_trace && g_trace_left)) { r[15] = pc; trace_line(cpu, pc, &F); g_trace_left--; }
            cpu->n = F.n; cpu->z = F.z; cpu->c = F.c; cpu->v = F.v; cpu->thumb = thumb; r[15] = pc;
            r[0] = g_stub_fn[idx](cpu);
            pc = r[14] & ~1u; thumb = r[14] & 1;                /* bx lr */
            continue;
        }
        if (UNLIKELY(pc - LIB_BASE < g_hook_span)) {
            if (pc == g_stop_pc) break;                          /* verifier: original code reached the join point */
            uint8_t hi = g_hookmap[(pc - LIB_BASE) >> 1];
            if (hi && thumb && g_hooks_on) {
                cpu->n = F.n; cpu->z = F.z; cpu->c = F.c; cpu->v = F.v; r[15] = pc;
                g_hook_cur = hi;
                int rc = g_hook_fn[hi](cpu);
                if (rc) {
                    F.n = cpu->n; F.z = cpu->z; F.c = cpu->c; F.v = cpu->v;
                    if (rc == 1) { pc = r[14] & ~1u; thumb = r[14] & 1; }    /* as if the guest function returned */
                    else pc = r[15] & ~1u;                                    /* rc == 2: continue at r[15] (Thumb) */
                    continue;
                }
            }
        }
        if (UNLIKELY(g_trace && g_trace_left)) { r[15] = pc; trace_line(cpu, pc, &F); g_trace_left--; }
        if (UNLIKELY(g_ring_on)) { g_ring[g_ring_i & 255] = pc; g_ring_lr[g_ring_i & 255] = r[14]; g_ring_i++; }
        ic++;
        if (UNLIKELY(g_prof != NULL) && pc - g_prof_base < g_prof_span) g_prof[(pc - g_prof_base) >> 1]++;

        if (thumb) {
            uint32_t op = ld16(pc);
            if (UNLIKELY(g_opcov_on)) g_opcov[(op >> 11) == 0x1e ? 0x110 : (op >> 10) == 0x10 ? (op >> 6) & 0x1ff : op >> 8] = 1;
            uint32_t ipc = pc;           /* address of this instruction */
            pc += 2;                     /* pc = next instruction */
            switch (op >> 11) {
            /* ---- format 1: shift by immediate ---- */
            case 0: { uint32_t imm = (op >> 6) & 31, rs = r[(op >> 3) & 7], res;
                if (imm) { F.c = (rs >> (32 - imm)) & 1; res = rs << imm; } else res = rs;
                r[op & 7] = res; SETNZ(res); break; }
            case 1: { uint32_t imm = (op >> 6) & 31, rs = r[(op >> 3) & 7], res;
                if (imm) { F.c = (rs >> (imm - 1)) & 1; res = rs >> imm; } else { F.c = rs >> 31; res = 0; }
                r[op & 7] = res; SETNZ(res); break; }
            case 2: { uint32_t imm = (op >> 6) & 31, rs = r[(op >> 3) & 7], res;
                if (imm) { F.c = ((int32_t)rs >> (imm - 1)) & 1; res = (uint32_t)((int32_t)rs >> imm); }
                else { F.c = rs >> 31; res = (uint32_t)((int32_t)rs >> 31); }
                r[op & 7] = res; SETNZ(res); break; }
            /* ---- format 2: add/subtract ---- */
            case 3: { uint32_t rn = r[(op >> 3) & 7];
                uint32_t val = (op & 0x400) ? ((op >> 6) & 7) : r[(op >> 6) & 7];
                r[op & 7] = (op & 0x200) ? sub_f(&F, rn, val, 1) : add_f(&F, rn, val, 0); break; }
            /* ---- format 3: mov/cmp/add/sub immediate ---- */
            case 4: { uint32_t v = op & 255; r[(op >> 8) & 7] = v; SETNZ(v); break; }
            case 5: sub_f(&F, r[(op >> 8) & 7], op & 255, 1); break;
            case 6: { uint32_t rd = (op >> 8) & 7; r[rd] = add_f(&F, r[rd], op & 255, 0); break; }
            case 7: { uint32_t rd = (op >> 8) & 7; r[rd] = sub_f(&F, r[rd], op & 255, 1); break; }
            case 8: case 9:                                   /* 01000 / 01001 */
                if (op & 0x0800) {
                    /* ---- format 6: PC-relative load ---- */
                    r[(op >> 8) & 7] = ld32(((ipc + 4) & ~3u) + (op & 255) * 4);
                } else if (op & 0x0400) {
                    /* ---- format 5: hi register operations / BX ---- */
                    uint32_t rd = (op & 7) | ((op >> 4) & 8), rs = (op >> 3) & 15;
                    uint32_t sv = rs == 15 ? ipc + 4 : r[rs];
                    switch ((op >> 8) & 3) {
                    case 0: { uint32_t dv = rd == 15 ? ipc + 4 : r[rd]; uint32_t res = dv + sv;
                        if (rd == 15) pc = res & ~1u; else r[rd] = res; break; }
                    case 1: sub_f(&F, rd == 15 ? ipc + 4 : r[rd], sv, 1); break;
                    case 2: if (rd == 15) pc = sv & ~1u; else r[rd] = sv; break;
                    case 3:
                        if (op & 0x80) r[14] = pc | 1;       /* BLX reg */
                        thumb = sv & 1; pc = sv & ~1u; break;
                    }
                } else {
                    /* ---- format 4: ALU operations ---- */
                    uint32_t rd = op & 7, rs = r[(op >> 3) & 7], a = r[rd], res;
                    switch ((op >> 6) & 15) {
                    case 0: res = a & rs; r[rd] = res; SETNZ(res); break;
                    case 1: res = a ^ rs; r[rd] = res; SETNZ(res); break;
                    case 2: res = lsl_r(&F, a, rs); r[rd] = res; SETNZ(res); break;
                    case 3: res = lsr_r(&F, a, rs); r[rd] = res; SETNZ(res); break;
                    case 4: res = asr_r(&F, a, rs); r[rd] = res; SETNZ(res); break;
                    case 5: r[rd] = add_f(&F, a, rs, F.c); break;
                    case 6: r[rd] = sub_f(&F, a, rs, F.c); break;
                    case 7: res = ror_r(&F, a, rs); r[rd] = res; SETNZ(res); break;
                    case 8: res = a & rs; SETNZ(res); break;
                    case 9: r[rd] = sub_f(&F, 0, rs, 1); break;
                    case 10: sub_f(&F, a, rs, 1); break;
                    case 11: add_f(&F, a, rs, 0); break;
                    case 12: res = a | rs; r[rd] = res; SETNZ(res); break;
                    case 13: res = a * rs; r[rd] = res; SETNZ(res); break;
                    case 14: res = a & ~rs; r[rd] = res; SETNZ(res); break;
                    default: res = ~rs; r[rd] = res; SETNZ(res); break;
                    }
                }
                break;
            /* ---- format 7/8: load/store register offset, sign-extended ---- */
            case 10: case 11: {
                uint32_t rd = op & 7, addr2 = r[(op >> 3) & 7] + r[(op >> 6) & 7];
                if (!(op & 0x0200)) {                         /* format 7 */
                    switch ((op >> 10) & 3) {
                    case 0: st32(addr2, r[rd]); break;
                    case 1: st8(addr2, r[rd]); break;
                    case 2: r[rd] = ld32(addr2); break;
                    default: r[rd] = ld8(addr2); break;
                    }
                } else {                                      /* format 8 */
                    switch ((op >> 10) & 3) {
                    case 0: st16(addr2, r[rd]); break;
                    case 1: r[rd] = (uint32_t)(int32_t)(int8_t)ld8(addr2); break;
                    case 2: r[rd] = ld16(addr2); break;
                    default: r[rd] = (uint32_t)(int32_t)(int16_t)ld16(addr2); break;
                    }
                }
                break; }
            /* ---- format 9: load/store word/byte immediate offset ---- */
            case 12: st32(r[(op >> 3) & 7] + ((op >> 6) & 31) * 4, r[op & 7]); break;
            case 13: r[op & 7] = ld32(r[(op >> 3) & 7] + ((op >> 6) & 31) * 4); break;
            case 14: st8(r[(op >> 3) & 7] + ((op >> 6) & 31), r[op & 7]); break;
            case 15: r[op & 7] = ld8(r[(op >> 3) & 7] + ((op >> 6) & 31)); break;
            /* ---- format 10: halfword immediate offset ---- */
            case 16: st16(r[(op >> 3) & 7] + ((op >> 6) & 31) * 2, r[op & 7]); break;
            case 17: r[op & 7] = ld16(r[(op >> 3) & 7] + ((op >> 6) & 31) * 2); break;
            /* ---- format 11: SP-relative load/store ---- */
            case 18: st32(r[13] + (op & 255) * 4, r[(op >> 8) & 7]); break;
            case 19: r[(op >> 8) & 7] = ld32(r[13] + (op & 255) * 4); break;
            /* ---- format 12: load address ---- */
            case 20: r[(op >> 8) & 7] = ((ipc + 4) & ~3u) + (op & 255) * 4; break;
            case 21: r[(op >> 8) & 7] = r[13] + (op & 255) * 4; break;
            /* ---- format 13/14: SP adjust, push/pop ---- */
            case 22: case 23: {
                if ((op & 0xff00) == 0xb000) {
                    uint32_t off = (op & 0x7f) * 4; r[13] = (op & 0x80) ? r[13] - off : r[13] + off;
                } else if ((op & 0x0600) == 0x0400) {                         /* push/pop */
                    uint32_t list = op & 255;
                    if (!(op & 0x0800)) {                                     /* PUSH */
                        uint32_t n = (uint32_t)__builtin_popcount(list) + ((op >> 8) & 1);
                        uint32_t a = r[13] - 4 * n; r[13] = a;
                        for (int i = 0; i < 8; i++) if (list & (1u << i)) { st32(a, r[i]); a += 4; }
                        if (op & 0x100) st32(a, r[14]);
                    } else {                                                  /* POP */
                        uint32_t a = r[13];
                        for (int i = 0; i < 8; i++) if (list & (1u << i)) { r[i] = ld32(a); a += 4; }
                        if (op & 0x100) { uint32_t v = ld32(a); a += 4; thumb = v & 1; pc = v & ~1u; }
                        r[13] = a;
                    }
                } else if ((op & 0xff00) == 0xbe00) {
                    bad_op("thumb bkpt", ipc, op);
                } else {
                    bad_op("thumb misc", ipc, op);
                }
                break; }
            /* ---- format 15: multiple load/store ---- */
            case 24: case 25: {
                uint32_t rb = (op >> 8) & 7, list = op & 255, a = r[rb];
                if (!(op & 0x0800)) {
                    int first = 1;
                    for (int i = 0; i < 8; i++) if (list & (1u << i)) {
                        st32(a, ((uint32_t)i == rb && !first) ? r[rb] + 4u * (uint32_t)__builtin_popcount(list) : r[i]);
                        a += 4; first = 0;
                    }
                    r[rb] = a;
                } else {
                    for (int i = 0; i < 8; i++) if (list & (1u << i)) { r[i] = ld32(a); a += 4; }
                    if (!(list & (1u << rb))) r[rb] = a;
                }
                break; }
            /* ---- format 16/17: conditional branch, SWI ---- */
            case 26: case 27: {
                uint32_t cc = (op >> 8) & 15;
                if (cc == 15) bad_op("thumb svc", ipc, op);
                if (cc == 14) bad_op("thumb udf", ipc, op);
                if (cond_pass(&F, cc)) pc = ipc + 4 + (uint32_t)((int32_t)(int8_t)(op & 255) * 2);
                break; }
            /* ---- format 18: unconditional branch ---- */
            case 28: { int32_t off = (int32_t)((op & 0x7ff) << 21) >> 20; pc = ipc + 4 + (uint32_t)off; break; }
            case 29: {                                                /* BLX suffix (ARMv5) */
                uint32_t t = (r[14] + ((op & 0x7ff) << 1)) & ~3u;
                r[14] = pc | 1; pc = t; thumb = 0; break; }
            /* ---- format 19: long branch with link ---- */
            case 30: r[14] = ipc + 4 + (uint32_t)(((int32_t)((op & 0x7ff) << 21) >> 21) * 4096); break;
            case 31: { uint32_t t = r[14] + ((op & 0x7ff) << 1); r[14] = pc | 1; pc = t; break; }
            default: bad_op("thumb", ipc, op);
            }
        } else {
            /* ---------------- minimal ARM state ---------------- */
            uint32_t ipc = pc, op = ld32(pc);
            pc += 4;
            uint32_t cc = op >> 28;
            if (cc == 15) {
                if ((op & 0xfe000000) == 0xfa000000) {            /* BLX imm: to Thumb */
                    int32_t off = (int32_t)(op << 8) >> 6; off += (int32_t)((op >> 23) & 2);
                    r[14] = pc; pc = ipc + 8 + (uint32_t)off; thumb = 1; continue;
                }
                bad_op("arm unconditional", ipc, op);
            }
            if (!cond_pass(&F, cc)) continue;
            uint32_t cls = (op >> 25) & 7;
            if ((op & 0x0ffffff0) == 0x012fff10) {                /* BX */
                uint32_t t = r[op & 15]; thumb = t & 1; pc = t & ~1u; continue;
            }
            if ((op & 0x0ffffff0) == 0x012fff30) {                /* BLX reg */
                uint32_t t = r[op & 15]; r[14] = pc; thumb = t & 1; pc = t & ~1u; continue;
            }
            if (cls == 5) {                                        /* B / BL */
                int32_t off = (int32_t)(op << 8) >> 6;
                if (op & 0x01000000) r[14] = pc;
                pc = ipc + 8 + (uint32_t)off; continue;
            }
            if (cls <= 1 && !((op & 0x0fc000f0) == 0x00000090) && !((op & 0x90) == 0x90 && cls == 0)) {   /* data processing */
                uint32_t opc = (op >> 21) & 15, s = (op >> 20) & 1, rn = (op >> 16) & 15, rd = (op >> 12) & 15;
                uint32_t a = rn == 15 ? ipc + 8 : r[rn], b;
                int logical = (opc == 0 || opc == 1 || opc == 8 || opc == 9 || opc == 12 || opc == 13 || opc == 14 || opc == 15);
                if (cls == 1) {
                    uint32_t rot = ((op >> 8) & 15) * 2; b = ror32(op & 255, rot);
                    if (s && logical && rot) F.c = b >> 31;
                } else {
                    uint32_t rm = op & 15, rv = rm == 15 ? ipc + 8 : r[rm];
                    if (op & 0x10) { uint32_t rs = r[(op >> 8) & 15]; if (rm == 15) rv = ipc + 12;
                        b = arm_shift(&F, rv, (op >> 5) & 3, rs, 1, s && logical); }
                    else b = arm_shift(&F, rv, (op >> 5) & 3, (op >> 7) & 31, 0, s && logical);
                }
                uint32_t res; Flags nf = F;
                switch (opc) {
                case 0: res = a & b; break;           case 1: res = a ^ b; break;
                case 2: res = sub_f(&nf, a, b, 1); break;   case 3: res = sub_f(&nf, b, a, 1); break;
                case 4: res = add_f(&nf, a, b, 0); break;   case 5: res = add_f(&nf, a, b, F.c); break;
                case 6: res = sub_f(&nf, a, b, F.c); break; case 7: res = sub_f(&nf, b, a, F.c); break;
                case 8: res = a & b; break;           case 9: res = a ^ b; break;
                case 10: res = sub_f(&nf, a, b, 1); break;  case 11: res = add_f(&nf, a, b, 0); break;
                case 12: res = a | b; break;          case 13: res = b; break;
                case 14: res = a & ~b; break;         default: res = ~b; break;
                }
                if (s) {
                    if (logical) { F.n = res >> 31; F.z = res == 0; } else F = nf;
                }
                if (opc < 8 || opc > 11) {
                    if (rd == 15) { pc = res & ~1u; if (s) bad_op("arm movs pc", ipc, op); } else r[rd] = res;
                }
                continue;
            }
            if (cls == 2 || (cls == 3 && !(op & 0x10))) {          /* LDR/STR word/byte */
                uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, p = (op >> 24) & 1, u = (op >> 23) & 1,
                         bflag = (op >> 22) & 1, w = (op >> 21) & 1, l = (op >> 20) & 1;
                uint32_t base = rn == 15 ? ipc + 8 : r[rn], off;
                if (cls == 2) off = op & 0xfff;
                else { uint32_t rm = r[op & 15]; off = arm_shift(&F, rm, (op >> 5) & 3, (op >> 7) & 31, 0, 0); }
                uint32_t eff = u ? base + off : base - off, a = p ? eff : base;
                if (l) {
                    uint32_t v = bflag ? ld8(a) : ld32(a);
                    if (!p || w) r[rn] = eff;
                    if (rd == 15) { thumb = v & 1; pc = v & ~1u; } else r[rd] = v;
                } else {
                    uint32_t v = rd == 15 ? ipc + 12 : r[rd];
                    if (bflag) st8(a, v); else st32(a, v);
                    if (!p || w) r[rn] = eff;
                }
                continue;
            }
            bad_op("arm", ipc, op);
        }
    }
    cpu->icount += ic; cpu->n = F.n; cpu->z = F.z; cpu->c = F.c; cpu->v = F.v;
}

uint32_t cpu_call(Cpu *cpu, uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t *r = cpu->r;
    uint32_t sp = (STACK_TOP - 0x100);
    int nstack = nargs > 4 ? nargs - 4 : 0;
    sp -= 4u * (uint32_t)nstack; sp &= ~7u;
    for (int i = 0; i < nstack; i++) mem_w32(sp + 4u * (uint32_t)i, args[4 + i]);
    for (int i = 0; i < 4; i++) r[i] = i < nargs ? args[i] : 0;
    r[13] = sp; r[14] = RET_MAGIC;
    exec_loop(cpu, addr & ~1u, addr & 1);
    return r[0];
}

/* Run the ORIGINAL guest function whose entry state is *c (same registers and stack) to completion,
 * with overrides disabled. Used to verify natives against the real code. */
void cpu_run_original(Cpu *c, uint32_t entry, uint32_t stop_pc) {
    int saved = g_hooks_on; g_hooks_on = 0;
    if (stop_pc) g_stop_pc = stop_pc & ~1u; else c->r[14] = RET_MAGIC;      /* entry hooks end at the return; mid-function hooks at stop_pc */
    exec_loop(c, entry & ~1u, 1);
    g_stop_pc = 0; g_hooks_on = saved;
}

#ifdef ISA_TEST
/* Execute exactly one instruction (a Thumb BL pair counts as one); leaves the next pc in r[15], its state in ->thumb. */
void cpu_step_one(Cpu *c, uint32_t pc, int thumb) {
    g_single = 1; g_single_done = 0; exec_loop(c, pc, thumb); g_single = 0;
    c->r[15] = g_final_pc; c->thumb = g_final_thumb;
}
#endif
