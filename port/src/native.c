/* native.c -- native (host) implementations of the game's hottest guest functions.
 *
 * ~95% of the game's instructions are software blitters writing a 400x240 RGB565
 * framebuffer. Each function here reproduces one guest function exactly (including
 * the scratch globals it writes) and is installed as an override at the function's
 * entry. Verification mode runs the real guest code from the same entry state and
 * compares all guest memory and the return value, so a wrong reimplementation is
 * caught on real game data rather than by eyeballing frames. MIT. */
#include "guest.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint16_t __attribute__((aligned(1))) u16;
typedef uint32_t __attribute__((aligned(1))) u32;

static inline uint8_t *HP(uint32_t a) { return g_pages[a >> 12] + (a & 0xfff); }  /* regions are contiguous host memory */
static inline uint32_t G32(uint32_t a) { return *(u32 *)HP(a); }
static inline void S32(uint32_t a, uint32_t v) { *(u32 *)HP(a) = v; }

/* guest globals (addresses of variables in the library's .bss) */
static struct {
    uint32_t WIPIMEM, zstate, Grp, maxR, maxG, maxB, maskR, maskG, maskB, shiftR, shiftG, lenR, lenG, lenB, gProcVarS, gProcVarB, gProcVarR;
    uint32_t frameBuffer, mainScreenWidth, mainScreenHeight;
} V;
#define GRP_KEY(g)   G32((g) + 0x50)     /* transparent colour */
#define GRP_BPP(g)   G32((g) + 0x48)
#define GRP_MODE(g)  G32((g) + 0x64)
#define GRP_ALPHA(g) G32((g) + 0x68)

/* ------------------------------------------------- handle table / zlib bits
 * The game's memory manager hands out small integer handles; MC_GETDPTR maps a handle
 * (1..5000) to its data pointer through the 5000-entry table WIPIMEM. */
static inline uint32_t getdptr(uint32_t h) { return (h - 1u) <= 4999u ? G32(V.WIPIMEM + (h - 1u) * 4u) : 0; }
static int n_getdptr(Cpu *c) { c->r[0] = getdptr(c->r[0]); return 1; }

/* Zlib_skipBits(n): bit position += n. State struct: [0] src descriptor, [1] dst descriptor, [2] bit position, [3] out index. */
static int n_zskip(Cpu *c) { S32(V.zstate + 8, G32(V.zstate + 8) + c->r[0]); return 1; }

/* Zlib_readBits(n): LSB-first bit reader over the source buffer, updates the bit position after every byte step. */
static uint32_t zread(int32_t n) {
    const uint8_t *src = HP(getdptr(G32(G32(V.zstate))));
    uint32_t res = 0;
    if (n > 0) {
        uint32_t pos = G32(V.zstate + 8), done = 0;
        do {
            uint32_t bit = pos & 7, take = (8 - bit) & 0xffff;
            if ((int32_t)take > (int32_t)(n - (int32_t)done)) take = (uint32_t)(n - (int32_t)done);
            take &= 0xffff;
            uint32_t mask = *HP(LIB_BASE + 0x3fedc + take), byte = src[pos >> 3];
            pos += take;
            res |= ((byte >> bit) & mask) << (done & 0xff);
            S32(V.zstate + 8, pos);
            done = (done + take) & 0xffff;
        } while ((int32_t)done < n);
    }
    return res;
}
static int n_zread(Cpu *c) { c->r[0] = zread((int32_t)c->r[0]); return 1; }
static inline void zskip(int32_t n) { S32(V.zstate + 8, G32(V.zstate + 8) + (uint32_t)n); }

/* Zlib_inflProc(litTable, litBits, distTable, distBits): the Huffman-coded body of one deflate block.
 * Tables are arrays of {int16 code length, uint16 symbol}; rodata tables hold length/distance bases and extra bits. */
static int n_zinfl(Cpu *c) {
    uint32_t st = V.zstate, tlit = c->r[0], tdist = c->r[2]; int32_t lbits = (int32_t)c->r[1], dbits = (int32_t)c->r[3];
    uint32_t dstdesc = G32(st + 4);
    uint8_t *out = HP(getdptr(G32(dstdesc)));
    const uint8_t *lit = HP(getdptr(G32(tlit))), *dst_t = HP(getdptr(G32(tdist)));
    uint32_t limit = G32(dstdesc + 4);
    for (;;) {
        if (G32(st + 12) >= limit) return 1;
        uint32_t sym;
        for (;;) {
            uint32_t idx = zread(lbits);
            zskip((int32_t)(int16_t)*(u16 *)(lit + idx * 4) - lbits);
            sym = *(u16 *)(lit + idx * 4 + 2);
            if (sym == 0x100) return 1;
            if (sym > 0xff) break;
            uint32_t o = G32(st + 12);
            out[o] = (uint8_t)sym; S32(st + 12, o + 1);
            if (G32(st + 12) >= limit) return 1;
        }
        uint32_t base = *(u16 *)HP(LIB_BASE + 0x3fee8 + (sym - 0x101) * 2);
        int32_t len = (int32_t)(base + zread(*HP(LIB_BASE + 0x3fe27 + sym)));
        uint32_t d = zread(dbits);
        zskip((int32_t)(int16_t)*(u16 *)(dst_t + d * 4) - dbits);
        uint32_t dsym = *(u16 *)(dst_t + d * 4 + 2);
        uint32_t dbase = *(u16 *)HP(LIB_BASE + 0x3ff48 + dsym * 2);
        uint32_t dextra = zread(*HP(LIB_BASE + 0x3ff84 + dsym));
        if (len < 1) continue;
        int32_t dist = (int32_t)(dbase + dextra);
        uint32_t o = G32(st + 12);
        const uint8_t *from = out + o - dist;
        do {
            o = G32(st + 12);
            if (len < dist) { memcpy(out + o, from, (size_t)len); S32(st + 12, o + (uint32_t)len); break; }
            memcpy(out + o, from, (size_t)dist); S32(st + 12, o + (uint32_t)dist); len -= dist;
        } while (len > 0);
    }
}

/* ---------------------------------------------------------------- ALPHA16 */
static uint32_t alpha16(uint32_t a, uint32_t b, uint32_t alpha) {
    uint32_t u1 = (a | (a << 16)) & 0x07e0f81f; S32(V.gProcVarB, u1);
    uint32_t u2 = (b | (b << 16)) & 0x07e0f81f; S32(V.gProcVarS, u2);
    uint32_t t = (uint32_t)((int32_t)((u1 - u2) * alpha) >> 4) + u2;
    uint32_t u3 = t & 0x07e0f81f; S32(V.gProcVarR, u3);
    return (u3 | ((uint32_t)((int32_t)u3 >> 16))) & 0xffff;   /* guest: orrs r1,r0 ; lsls/lsrs 16 */
}
static int n_alpha16(Cpu *c) { c->r[0] = alpha16(c->r[0], c->r[1], c->r[2]); return 1; }

/* ------------------------------------------- 8-bit indexed -> RGB565 blits
 * Shared ABI of _Grp_draw{Dodge,Screen,...}16:
 *   r0 dst (RGB565*), r1 src (palette indices), r2 palette (RGB565[256]), r3 width,
 *   stack: rows, src_skip (bytes after each row), dst_colstep (pixels), dst_rowskip (pixels) */
typedef struct { uint32_t dst, src, pal, w, rows, src_skip, col_step, row_skip; } Blit;
static inline Blit blit_args(Cpu *c) {
    Blit b = { c->r[0], c->r[1], c->r[2], c->r[3], cpu_arg(c, 4), cpu_arg(c, 5), cpu_arg(c, 6), cpu_arg(c, 7) };
    return b;
}

static int n_dodge16(Cpu *c) {
    Blit b = blit_args(c);
    if ((int32_t)b.rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp), mR = G32(V.maxR), mG = G32(V.maxG), mB = G32(V.maxB);
    const uint8_t *src = HP(b.src); const u16 *pal = (const u16 *)HP(b.pal);
    uint8_t *dst = HP(b.dst); int32_t step = (int32_t)b.col_step * 2;
    for (int32_t y = 0; y < (int32_t)b.rows; y++) {
        if ((int32_t)b.w > 0) {
            for (int32_t x = 0; x < (int32_t)b.w; x++, dst += step) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t d = *(u16 *)dst;
                uint32_t r = (d & mR) + (p & mR); if ((int32_t)r > (int32_t)mR) r = mR;
                uint32_t g = (d & mG) + (p & mG); if ((int32_t)g > (int32_t)mG) g = mG;
                uint32_t bl = (d & mB) + (p & mB); if ((int32_t)bl > (int32_t)mB) bl = mB;
                *(u16 *)dst = (uint16_t)(g | r | bl);
            }
            src += b.w;
        }
        if (y + 1 == (int32_t)b.rows) break;
        dst += (int32_t)b.row_skip * 2; src += (int32_t)b.src_skip;
    }
    return 1;
}

static int n_screen16(Cpu *c) {
    Blit b = blit_args(c);
    if ((int32_t)b.rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp);
    uint32_t mkR = G32(V.maskR), mkG = G32(V.maskG), mkB = G32(V.maskB), shR = G32(V.shiftR), shG = G32(V.shiftG),
             lnR = G32(V.lenR), lnG = G32(V.lenG), lnB = G32(V.lenB);
    const uint8_t *src = HP(b.src); const u16 *pal = (const u16 *)HP(b.pal);
    uint8_t *dst = HP(b.dst); int32_t step = (int32_t)b.col_step * 2;
    for (int32_t y = 0; y < (int32_t)b.rows; y++) {
        if ((int32_t)b.w > 0) {
            for (int32_t x = 0; x < (int32_t)b.w; x++, dst += step) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t d = *(u16 *)dst;
                uint32_t a = (((mkR - (((mkR & ~(d >> shR)) * (mkR & ~(p >> shR))) >> lnR)) & mkR) << shR);
                uint32_t g = (((mkG - (((mkG & ~(d >> shG)) * (mkG & ~(p >> shG))) >> lnG)) & mkG) << shG);
                uint32_t bl = ((mkB - (((mkB & ~d) * (mkB & ~p)) >> lnB)) & mkR);      /* guest masks the blue term with maskR */
                *(u16 *)dst = (uint16_t)(a | g | bl);
            }
            src += b.w;
        }
        if (y + 1 == (int32_t)b.rows) break;
        dst += (int32_t)b.row_skip * 2; src += (int32_t)b.src_skip;
    }
    return 1;
}

/* _Grp_drawDodgeCol16: _Grp_drawDodge16 against a constant colour (Grp+0x70) instead of the destination pixel; the result is stored. */
static int n_dodgecol16(Cpu *c) {
    Blit b = blit_args(c);
    if ((int32_t)b.rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp), mR = G32(V.maxR), mG = G32(V.maxG), mB = G32(V.maxB), col = *(u16 *)HP(V.Grp + 0x70);
    const uint8_t *src = HP(b.src); const u16 *pal = (const u16 *)HP(b.pal);
    uint8_t *dst = HP(b.dst); int32_t step = (int32_t)b.col_step * 2;
    for (int32_t y = 0; y < (int32_t)b.rows; y++) {
        if ((int32_t)b.w > 0) {
            for (int32_t x = 0; x < (int32_t)b.w; x++, dst += step) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t r = (p & mR) + (col & mR); if ((int32_t)r > (int32_t)mR) r = mR;
                uint32_t g = (p & mG) + (col & mG); if ((int32_t)g > (int32_t)mG) g = mG;
                uint32_t bl = (p & mB) + (col & mB); if ((int32_t)bl > (int32_t)mB) bl = mB;
                *(u16 *)dst = (uint16_t)(g | r | bl);
            }
            src += b.w;
        }
        if (y + 1 == (int32_t)b.rows) break;
        dst += (int32_t)b.row_skip * 2; src += (int32_t)b.src_skip;
    }
    return 1;
}

/* _Grp_drawAura16: for opaque source pixels strictly inside the sprite (not on its first/last row or column) copy the destination
 * pixel at a neighbour offset over the pixel itself. The offset is derived from Grp+0x5c (the framebuffer stride in pixels)
 * and the signs of the column step / row skip: it selects which neighbour the "aura" smears from. */
static int n_aura16(Cpu *c) {
    Blit b = blit_args(c);
    int32_t rows = (int32_t)b.rows, w = (int32_t)b.w, cs = (int32_t)b.col_step, rs = (int32_t)b.row_skip;
    uint32_t H = *(u16 *)HP(V.Grp + 0x5c), off;
    int32_t pos = cs > 0;
    if (rs > 0)       off = pos ? H - 1 : (cs < 0 ? H + 1 : ~H);
    else if (rs == 0) off = ~H;
    else              off = pos ? 1 - H : ~H;
    int32_t off2 = (int32_t)(int16_t)(uint16_t)off * 2;
    if (rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp);
    const uint8_t *src = HP(b.src); const u16 *pal = (const u16 *)HP(b.pal);
    uint32_t dstg = b.dst;
    for (int32_t y = 0; y < rows; y++) {
        if (w > 0) {
            for (int32_t x = 0; x < w; x++) {
                if (y != 0 && x > 0 && rows - 1 > y && w - 1 > x) {
                    uint32_t p = pal[src[x]];
                    if (p != key) { uint8_t *d = HP(dstg + (uint32_t)(x * cs * 2)); *(u16 *)d = *(u16 *)HP(dstg + (uint32_t)(x * cs * 2) + (uint32_t)off2); }
                }
            }
            dstg += (uint32_t)(w * cs * 2); src += w;
        }
        if (y + 1 == rows) break;
        src += (int32_t)b.src_skip; dstg += (uint32_t)(rs * 2);
    }
    return 1;
}

/* Grp_closeupScreen2(handle, zoom): in-place "zoom to the centre" of a 16-bit framebuffer, done as four mirrored quadrants.
 * Each of the (H+1)/2 rows writes (W+1)/2 pixels into every corner, reading from the (already partly rewritten) framebuffer at
 * 10.6 fixed-point positions that advance by q = 4096/zoom per step. The writes of one iteration are done in the guest's order
 * because the quadrants overlap in the middle row/column of odd sizes. Anything unusual (bpp != 16, indices out of the
 * framebuffer) is declined so the guest code runs instead. */
static uint32_t fb_desc_for(uint32_t h) { return h > 4 ? 0 : G32(V.frameBuffer + h * 4); }
static int n_closeup2(Cpu *c) {
    uint32_t h = c->r[0]; int32_t prm = (int32_t)c->r[1];
    uint32_t desc = fb_desc_for(h);
    if (!desc) return 0;
    int32_t W = (int32_t)(h == 0 ? G32(V.mainScreenWidth) : G32(desc)), H = (int32_t)(h == 0 ? G32(V.mainScreenHeight) : G32(desc + 4)); uint32_t fbg = G32(desc + 8);
    if (G32(V.Grp + 0x48) != 16) return 1;
    if (W <= 0 || H <= 0 || !fbg) return 0;
    int32_t halfRows = H % 2 + H / 2, halfW = W % 2 + W / 2;
    int32_t q = prm == 0 ? 0 : (prm == -1 ? -4096 : 4096 / prm);
    int32_t t = halfW - 1, A = t - ((int32_t)((uint32_t)q * (uint32_t)t) >> 6);
    int32_t r3 = halfRows - 1, B = r3 - ((int32_t)((uint32_t)q * (uint32_t)r3) >> 6);
    int32_t X1 = B * 64, X2 = (H - 1 - B) * 64, Y1 = (W - 1 - A) * 64, Y2 = A * 64;
    int64_t lastR = halfRows - 1, lastK = halfW - 1;                    /* every index is monotone in i / k: check the ends */
    int64_t rt0 = X1 >> 6, rt1 = ((int64_t)X1 + lastR * q) >> 6, rb0 = X2 >> 6, rb1 = ((int64_t)X2 - lastR * q) >> 6;
    int64_t ca0 = Y2 >> 6, ca1 = ((int64_t)Y2 + lastK * q) >> 6, cb0 = Y1 >> 6, cb1 = ((int64_t)Y1 - lastK * q) >> 6;
    if (rt0 < 0 || rt0 >= H || rt1 < 0 || rt1 >= H || rb0 < 0 || rb0 >= H || rb1 < 0 || rb1 >= H) return 0;
    if (ca0 < 0 || ca0 >= W || ca1 < 0 || ca1 >= W || cb0 < 0 || cb0 >= W || cb1 < 0 || cb1 >= W) return 0;
    u16 *fb = (u16 *)HP(fbg);
    for (int32_t i = 0; i < halfRows; i++) {
        int32_t rowTop = W * (X1 >> 6), rowBot = W * (X2 >> 6);
        int32_t ya = Y2, yb = Y1;
        for (int32_t k = 0; k < halfW; k++) {
            int32_t ca = ya >> 6; ya += q;
            fb[i * W + k] = fb[rowTop + ca];
            fb[(H - 1 - i) * W + k] = fb[rowBot + ca];
            int32_t cb = yb >> 6; yb -= q;
            fb[i * W + (W - 1) - k] = fb[rowTop + cb];
            fb[(H - i) * W - 1 - k] = fb[rowBot + cb];
        }
        X1 += q; X2 -= q;
    }
    return 1;
}

static inline uint16_t pxlmultiply(uint16_t c0, uint16_t c1) {
    uint32_t mkR = G32(V.maskR), mkG = G32(V.maskG), mkB = G32(V.maskB);
    uint32_t shR = G32(V.shiftR), shG = G32(V.shiftG);
    uint32_t lnR = G32(V.lenR), lnG = G32(V.lenG), lnB = G32(V.lenB);
    uint32_t r = (((((uint32_t)c0 >> shR) & mkR) * (((uint32_t)c1 >> shR) & mkR)) >> lnR) << shR;
    uint32_t g = (((((uint32_t)c0 >> shG) & mkG) * (((uint32_t)c1 >> shG) & mkG)) >> lnG) << shG;
    uint32_t b = ((((uint32_t)c0 & mkB) * ((uint32_t)c1 & mkB)) >> lnB);
    return (uint16_t)(r | g | b);
}

static int n_pxlmultiply(Cpu *c) {
    c->r[0] = pxlmultiply((uint16_t)c->r[0], (uint16_t)c->r[1]);
    return 1;
}

static int n_drawmultiply16(Cpu *c) {
    Blit b = blit_args(c);
    if ((int32_t)b.rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp);
    uint32_t mkR = G32(V.maskR), mkG = G32(V.maskG), mkB = G32(V.maskB);
    uint32_t shR = G32(V.shiftR), shG = G32(V.shiftG);
    uint32_t lnR = G32(V.lenR), lnG = G32(V.lenG), lnB = G32(V.lenB);
    const uint8_t *src = HP(b.src);
    const u16 *pal = (const u16 *)HP(b.pal);
    uint8_t *dst = HP(b.dst);
    int32_t step = (int32_t)b.col_step * 2;
    for (int32_t y = 0; y < (int32_t)b.rows; y++) {
        if ((int32_t)b.w > 0) {
            for (int32_t x = 0; x < (int32_t)b.w; x++, dst += step) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t d = *(u16 *)dst;
                uint32_t r = (((((uint32_t)d >> shR) & mkR) * (((uint32_t)p >> shR) & mkR)) >> lnR) << shR;
                uint32_t g = (((((uint32_t)d >> shG) & mkG) * (((uint32_t)p >> shG) & mkG)) >> lnG) << shG;
                uint32_t bl = ((((uint32_t)d & mkB) * ((uint32_t)p & mkB)) >> lnB);
                *(u16 *)dst = (uint16_t)(r | g | bl);
            }
            src += b.w;
        }
        if (y + 1 == (int32_t)b.rows) break;
        dst += (int32_t)b.row_skip * 2;
        src += (int32_t)b.src_skip;
    }
    return 1;
}

static int n_drawalpmultiply16(Cpu *c) {
    Blit b = blit_args(c);
    if ((int32_t)b.rows <= 0) return 1;
    uint32_t key = GRP_KEY(V.Grp);
    uint32_t alpha = GRP_ALPHA(V.Grp);
    const uint8_t *src = HP(b.src);
    const u16 *pal = (const u16 *)HP(b.pal);
    uint8_t *dst = HP(b.dst);
    int32_t step = (int32_t)b.col_step * 2;
    for (int32_t y = 0; y < (int32_t)b.rows; y++) {
        if ((int32_t)b.w > 0) {
            for (int32_t x = 0; x < (int32_t)b.w; x++, dst += step) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t d = *(u16 *)dst;
                uint32_t mult = pxlmultiply((uint16_t)d, (uint16_t)p);
                *(u16 *)dst = (uint16_t)alpha16(d, mult, alpha);
            }
            src += b.w;
        }
        if (y + 1 == (int32_t)b.rows) break;
        dst += (int32_t)b.row_skip * 2;
        src += (int32_t)b.src_skip;
    }
    return 1;
}


/* ------------------------------------------------ mid-function overrides ----
 * These replace only the pixel loops of three big functions and rejoin the guest
 * code at its epilogue, so the clipping logic stays in the guest and cannot drift. */
#define ABS(a) (LIB_BASE + (a))
static inline uint32_t clamp_add(uint32_t a, uint32_t b, uint32_t m) { uint32_t r = a + b; return (int32_t)r > (int32_t)m ? m : r; }

/* Grp_drawClipWImageP @0xbd5a: after dst(r5)/src(r6)/palette(r8) are computed, before the mode switch. */
static int n_clipw(Cpu *c) {
    uint32_t grp = c->r[0], mode = G32(grp + 0x64);
    int plain = mode == 0 || mode > 0x16 || (mode >= 0xb && mode <= 0x13);
    if (!(plain || mode == 1 || mode == 2 || mode == 7 || mode == 8)) return 0;
    uint32_t sp = c->r[13];
    int32_t width = (int32_t)G32(sp + 0x78), rows = (int32_t)G32(sp + 0x7c);
    int32_t cstep = (int32_t)G32(sp + 0x34) * 2, rskip = (int32_t)G32(sp + 0x38) * 2, sskip = (int32_t)G32(sp + 0x3c);
    const uint8_t *src = HP(c->r[6]); uint8_t *dst = HP(c->r[5]); const u16 *pal = (const u16 *)HP(c->r[8]);
    uint32_t key = GRP_KEY(grp), alpha = GRP_ALPHA(grp);
    uint32_t mR = G32(V.maxR), mG = G32(V.maxG), mB = G32(V.maxB);
    if (rows > 0 && width > 0) {
        for (int32_t y = 0; y < rows; y++) {
            for (int32_t x = 0; x < width; x++, dst += cstep) {
                uint32_t p = pal[src[x]];
                if (p == key) continue;
                uint32_t d = *(u16 *)dst;
                switch (plain ? 0 : mode) {
                case 0: *(u16 *)dst = (uint16_t)p; break;
                case 1: *(u16 *)dst = (uint16_t)alpha16(d, p, alpha); break;
                case 2: {
                    uint32_t g = (d & mG) < (p & mG) ? (p & mG) : (d & mG);
                    uint32_t r = (d & mR) < (p & mR) ? (p & mR) : (d & mR);
                    uint32_t b = (d & mB) < (p & mB) ? (p & mB) : (d & mB);
                    *(u16 *)dst = (uint16_t)(r | g | b); break; }
                case 7: {
                    uint32_t t = (uint32_t)((int32_t)(((p | (p << 16)) & 0x07e0f81f) * (uint32_t)(0x10 - (int32_t)alpha)) >> 4) & 0x07e0f81f;
                    S32(V.gProcVarR, t); *(u16 *)dst = (uint16_t)(t | (t >> 16)); break; }
                case 8: {
                    uint32_t t = (uint32_t)((int32_t)((p >> 11) + (mB & p) + ((int32_t)(p & mG) >> 5)) >> 2);
                    S32(V.gProcVarR, t); *(u16 *)dst = (uint16_t)((t << 6) | (t << 11) | t); break; }
                }
            }
            src += width;
            if (y + 1 < rows) { dst += rskip; src += sskip; }
        }
    }
    c->r[15] = ABS(0xbc8c); return 2;
}

/* Grp_drawSimpleClipWImageP @0xb5d2: r7 = Grp, r0 dst, r1 src, r2 palette, r4 width; no colour key. */
static int n_simpleclip(Cpu *c) {
    uint32_t grp = c->r[7], mode = G32(grp + 0x64);
    if (mode == 6) return 0;                                  /* guest calls _Grp_drawBright16 */
    uint32_t sp = c->r[13];
    int32_t width = (int32_t)c->r[4], rows = (int32_t)G32(sp + 0x64);
    int32_t sskip = (int32_t)G32(sp + 0x24), dskip = (int32_t)G32(sp + 0x28) * 2;
    const uint8_t *src = HP(c->r[1]); uint8_t *dst = HP(c->r[0]); const u16 *pal = (const u16 *)HP(c->r[2]);
    uint32_t alpha = GRP_ALPHA(grp), col = *(u16 *)HP(grp + 0x70);
    uint32_t mR = G32(V.maxR), mG = G32(V.maxG), mB = G32(V.maxB);
    for (int32_t y = 0; y < rows; y++) {
        for (int32_t x = 0; x < width; x++, dst += 2) {
            uint32_t p = pal[src[x]];
            if (mode == 7) {
                uint32_t t = (uint32_t)((int32_t)(((p | (p << 16)) & 0x07e0f81f) * (uint32_t)(0x10 - (int32_t)alpha)) >> 4) & 0x07e0f81f;
                S32(V.gProcVarR, t); *(u16 *)dst = (uint16_t)(t | (t >> 16));
            } else if (mode == 10) {
                *(u16 *)dst = (uint16_t)(clamp_add(col & mG, p & mG, mG) | clamp_add(col & mR, p & mR, mR) | clamp_add(p & mB, col & mB, mB));
            } else *(u16 *)dst = (uint16_t)p;
        }
        src += width;
        if (y + 1 < rows) { src += sskip; dst += dskip; }
    }
    c->r[15] = ABS(0xb546); return 2;
}

/* Grp_fillRect @0xac38: r4 = Grp, sb = dst, r5 = colour, sl = width, fp = rows, [sp+0x10] = width16, [sp+0x14] = row skip. */
static int n_fillrect(Cpu *c) {
    uint32_t grp = c->r[4], mode = G32(grp + 0x64), sp = c->r[13];
    int32_t sl = (int32_t)c->r[10], rows = (int32_t)c->r[11];
    uint32_t w16 = G32(sp + 0x10) & 0xffff; int32_t skip = (int32_t)(int16_t)G32(sp + 0x14) * 2;
    uint32_t colour = c->r[5];
    uint8_t *dst = HP(c->r[9]);
    if (rows > 0) {
        for (int32_t y = 0; y < rows; y++) {
            if (sl > 0) {
                if (mode == 1) for (uint32_t x = 0; x < w16; x++, dst += 2) *(u16 *)dst = (uint16_t)alpha16(*(u16 *)dst, colour, GRP_ALPHA(grp));
                else for (uint32_t x = 0; x < w16; x++, dst += 2) *(u16 *)dst = (uint16_t)colour;
            }
            if (y + 1 < rows) dst += skip;
        }
    }
    c->r[15] = ABS(0xabc4); return 2;
}

/* Grp_drawWPNImage @0xb0de: pixel loop for particle & flame effects */
static int n_wpnimage(Cpu *c) {
    uint32_t sp = c->r[13];
    int32_t width = (int32_t)c->r[8];
    int32_t rows = (int32_t)G32(sp + 4);
    int32_t cur_row = (int32_t)G32(sp + 8);
    int32_t sskip = (int32_t)G32(sp + 12);
    int32_t mskip = (int32_t)G32(sp + 24);
    int32_t dskip = (int32_t)G32(sp + 20);
    uint32_t param = G32(sp + 16);
    uint32_t aoff = c->r[9] & 0xffff;
    uint8_t *dst = HP(c->r[5]);
    const uint8_t *src = HP(c->r[6]);
    const uint8_t *mask = HP(c->r[7]);

    for (int32_t y = cur_row; y < rows; y++) {
        for (int32_t x = 0; x < width; x++) {
            uint32_t m = mask[x];
            uint32_t a = aoff;
            if (m != 0) {
                a = (aoff + param + m) & 0xffff;
            }
            if (a > 14) continue;
            uint16_t s = *(const u16 *)(src + x * 2);
            if (a == 0) {
                *(u16 *)(dst + x * 2) = s;
            } else {
                uint16_t d = *(u16 *)(dst + x * 2);
                *(u16 *)(dst + x * 2) = (uint16_t)alpha16(d, s, a);
            }
        }
        dst += width * 2 + dskip;
        src += width * 2 + sskip;
        mask += width + mskip;
    }
    c->r[15] = ABS(0xb066);
    return 2;
}

/* Grp_drawWPNImage2 @0xbb16: transformed/oriented particle loop */
static int n_wpnimage2(Cpu *c) {
    uint32_t sp = c->r[13];
    int32_t width = (int32_t)G32(sp + 0x44);
    int32_t rows = (int32_t)G32(sp + 0x40);
    int32_t cur_row = (int32_t)c->r[10]; /* sl */
    int32_t sskip = (int32_t)G32(sp + 28);
    int32_t mskip = (int32_t)G32(sp + 32);
    int32_t dskip = (int32_t)G32(sp + 36);
    int32_t dstep = (int32_t)c->r[9];    /* dst step per pixel in bytes */
    uint32_t param = c->r[11];           /* fp */
    uint32_t aoff = c->r[7] & 0xffff;
    uint8_t *dst = HP(c->r[5]);
    const uint8_t *src = HP(c->r[6]);
    const uint8_t *mask = HP(c->r[8]);

    for (int32_t y = cur_row; y < rows; y++) {
        uint8_t *row_dst = dst;
        for (int32_t x = 0; x < width; x++, row_dst += dstep) {
            uint32_t m = mask[x];
            uint32_t a = aoff;
            if (m != 0) {
                a = (aoff + param + m) & 0xffff;
            }
            if (a > 14) continue;
            uint16_t s = *(const u16 *)(src + x * 2);
            if (a == 0) {
                *(u16 *)row_dst = s;
            } else {
                uint16_t d = *(u16 *)row_dst;
                *(u16 *)row_dst = (uint16_t)alpha16(d, s, a);
            }
        }
        dst = row_dst + dskip;
        src += width * 2 + sskip;
        mask += width + mskip;
    }
    c->r[15] = ABS(0xb9f0);
    return 2;
}

/* Grp_darkScreen(canvas_id, alpha): fast whole-frame darkener (used by minimap, pause menus, death screen) */
static int n_darkscreen(Cpu *c) {
    uint32_t scr_idx = c->r[0];
    int32_t alpha = (int32_t)c->r[1];
    if (alpha <= 0) { c->r[15] = c->r[14] & ~1u; return 1; }

    uint32_t grp = V.Grp;
    if (G32(grp) == scr_idx) { c->r[15] = c->r[14] & ~1u; return 1; }

    if (alpha > 256) alpha = 256;
    uint32_t a = (uint32_t)((alpha * 15 + 105) >> 8);
    if (a == 0) { c->r[15] = c->r[14] & ~1u; return 1; }

    uint32_t bpp = G32(grp + 0x48);
    if (bpp != 16) { c->r[15] = c->r[14] & ~1u; return 1; }

    if (scr_idx > 4) { c->r[15] = c->r[14] & ~1u; return 1; }
    uint32_t fb_desc = G32(V.frameBuffer + scr_idx * 4);
    if (!fb_desc) { c->r[15] = c->r[14] & ~1u; return 1; }

    int32_t w = (int32_t)G32(fb_desc + 0);
    int32_t h = (int32_t)G32(fb_desc + 4);
    uint32_t fb_ptr = G32(fb_desc + 8);
    if (w <= 0 || h <= 0 || !fb_ptr) { c->r[15] = c->r[14] & ~1u; return 1; }

    int total_pixels = w * h;
    uint16_t *dst = (uint16_t *)HP(fb_ptr);

    if (a >= 15) {
        memset(dst, 0, (size_t)total_pixels * 2);
        c->r[15] = c->r[14] & ~1u;
        return 1;
    }

    uint32_t scale = 16 - a;
    uint32_t last_d = 0;
    for (int i = 0; i < total_pixels; i++) {
        uint32_t p = dst[i];
        uint32_t d = (p | (p << 16)) & 0x07e0f81fu;
        d = ((d * scale) >> 4) & 0x07e0f81fu;
        last_d = d;
        dst[i] = (uint16_t)(d | (d >> 16));
    }
    S32(V.gProcVarR, last_d);
    c->r[15] = c->r[14] & ~1u;
    return 1;
}

/* Debug spy (env SPY=1): logs hero action reservations (2 stop, 3 walk, 4 dash) and declines, so the game runs unchanged. */
static int n_spy_reserve(Cpu *c) { if (getenv("SPY")) fprintf(stderr, "[spy] reserveHeroAct(%d,%d)\n", (int)c->r[0], (int)c->r[1]); return 0; }

/* ---------------------------------------------------------------- registry */
typedef struct { const char *name; uint32_t addr; uint32_t join; native_fn fn; int returns; } NativeDef;
/* name: exported symbol (addr == 0) or label for a mid-function hook at library offset addr, which
 * rejoins the guest at library offset `join`. */
static const NativeDef DEFS[] = {
    {"ALPHA16", 0, 0, n_alpha16, 1},
    {"_Grp_pxlMultiply", 0, 0, n_pxlmultiply, 1},
    {"_Grp_drawDodge16", 0, 0, n_dodge16, 0},
    {"_Grp_drawScreen16", 0, 0, n_screen16, 0},
    {"_Grp_drawDodgeCol16", 0, 0, n_dodgecol16, 0},
    {"_Grp_drawAura16", 0, 0, n_aura16, 0},
    {"Grp_closeupScreen2", 0, 0, n_closeup2, 0},
    {"_Grp_drawMultiply16", 0, 0, n_drawmultiply16, 0},
    {"_Grp_drawAlpMultiply16", 0, 0, n_drawalpmultiply16, 0},
    {"Grp_drawClipWImageP:blit", 0xbd5a, 0xbc8c, n_clipw, 0},
    {"Grp_drawSimpleClipWImageP:blit", 0xb5d2, 0xb546, n_simpleclip, 0},
    {"Grp_drawWPNImage:blit", 0xb0de, 0xb066, n_wpnimage, 0},
    {"Grp_drawWPNImage2:blit", 0xbb16, 0xb9f0, n_wpnimage2, 0},
    {"Grp_fillRect:fill", 0xac38, 0xabc4, n_fillrect, 0},
    {"Grp_darkScreen", 0, 0, n_darkscreen, 0},
    {"MC_GETDPTR", 0, 0, n_getdptr, 1},
    {"Zlib_skipBits", 0, 0, n_zskip, 0},
    {"Zlib_readBits", 0, 0, n_zread, 1},
    {"Zlib_inflProc", 0, 0, n_zinfl, 0},
    {"reserveHeroAct", 0, 0, n_spy_reserve, 0},
};
#define NDEFS ((int)(sizeof DEFS / sizeof *DEFS))

/* ----------------------------------------------------------------- patches
 * Deliberate behaviour changes (not performance overrides): never verified against the guest, off unless enabled. */
int g_patch_hide_pad = 0;                   /* skip the game's on-screen touch pad and enable console UI */
static int p_hide_pad(Cpu *c) { if (!g_patch_hide_pad) return 0; c->r[15] = c->r[14] & ~1u; return 1; }

/* Status gauge overlay hook: suppress HP/MP in menus to eliminate visual clipping */
static int p_draw_status(Cpu *c) {
    if (!g_patch_hide_pad) return 0;
    uint32_t canvas = mem_r32(LIB_BASE + 0x44278);
    if (canvas) {
        uint16_t st = 0;
        mem_read(canvas + 0x4854, &st, 2);
        if (st != 0) {
            c->r[15] = c->r[14] & ~1u;
            return 1; /* Suppress status gauge in menus/dialogs */
        }
    }
    return 0;
}

int g_hotbar_page = 0;
static uint16_t g_p1_slots[5];
static int g_p1_saved = 0;
static int g_prev_page = 0;
static int g_quick_slot_drawn = 0;

static int p_draw_quick_slot(Cpu *c) {
    if (!g_patch_hide_pad) return 0;
    g_quick_slot_drawn = 1;
    (void)c;
    uint32_t got = LIB_BASE + 0x440c8;
    uint32_t ui_table = mem_r32(got + 0x178);
    if (ui_table) {
        /* Ergonomic console bottom-right horizontal placement:
         * Slot 4 (Potion): x=256, y=210
         * Slot 0 (Skill 1): x=281, y=210
         * Slot 1 (Skill 2): x=305, y=210
         * Slot 2 (Skill 3): x=329, y=210
         * Slot 3 (Skill 4): x=353, y=210 */
        static const int16_t new_coords[5][2] = {
            {281, 210}, /* Slot 0 (Combat 1 - Y) */
            {305, 210}, /* Slot 1 (Combat 2 - X) */
            {329, 210}, /* Slot 2 (Combat 3 - R) */
            {353, 210}, /* Slot 3 (Combat 4 - ZR) */
            {256, 210}, /* Slot 4 (Potion - ZL) */
        };
        for (int i = 0; i < 5; i++) {
            mem_write(ui_table + 282 + i * 8, &new_coords[i][0], 2);
            mem_write(ui_table + 282 + i * 8 + 2, &new_coords[i][1], 2);
        }
        int16_t offscreen = 999;
        mem_write(ui_table + 322, &offscreen, 2); /* hide on-screen page switch button */
        mem_write(ui_table + 324, &offscreen, 2);
    }
    uint32_t canvas = mem_r32(LIB_BASE + 0x44278);
    if (canvas) {
        if (g_hotbar_page == 0) {
            if (g_prev_page == 1 && g_p1_saved) {
                /* Restore Page 1 slots */
                for (int i = 0; i < 5; i++) mem_write(canvas + 0x4a90 + i * 2, &g_p1_slots[i], 2);
            }
            for (int i = 0; i < 5; i++) mem_read(canvas + 0x4a90 + i * 2, &g_p1_slots[i], 2);
            g_p1_saved = 1;
        } else if (g_hotbar_page == 1) {
            /* Swap in Page 2 slots (slots 5..9) */
            for (int i = 0; i < 5; i++) {
                uint16_t p2_val = 0;
                mem_read(canvas + 0x4a90 + 10 + i * 2, &p2_val, 2);
                mem_write(canvas + 0x4a90 + i * 2, &p2_val, 2);
            }
        }
        g_prev_page = g_hotbar_page;
    }
    return 0; /* run original GameUI_drawQuickSlot with updated coords and slots */
}

/* ---- 3x5 Pixel Font & Controller Badge Overlay ---- */
static const uint8_t GLYPH_3X5[10][5] = {
    { 0x5, 0x5, 0x2, 0x5, 0x5 }, /* 0: 'X' */
    { 0x5, 0x5, 0x2, 0x2, 0x2 }, /* 1: 'Y' */
    { 0x6, 0x5, 0x6, 0x5, 0x5 }, /* 2: 'R' */
    { 0x7, 0x1, 0x2, 0x4, 0x7 }, /* 3: 'Z' */
    { 0x4, 0x4, 0x4, 0x4, 0x7 }, /* 4: 'L' */
    { 0x6, 0x5, 0x6, 0x4, 0x4 }, /* 5: 'P' */
    { 0x2, 0x6, 0x2, 0x2, 0x7 }, /* 6: '1' */
    { 0x6, 0x1, 0x2, 0x4, 0x7 }, /* 7: '2' */
    { 0x0, 0x2, 0x7, 0x2, 0x0 }, /* 8: '+' */
    { 0x0, 0x0, 0x0, 0x0, 0x0 }, /* 9: ' ' */
};

static void draw_char(uint16_t *fb, int x, int y, char c, uint16_t color) {
    int idx = -1;
    switch (c) {
    case 'X': idx = 0; break;
    case 'Y': idx = 1; break;
    case 'R': idx = 2; break;
    case 'Z': idx = 3; break;
    case 'L': idx = 4; break;
    case 'P': idx = 5; break;
    case '1': idx = 6; break;
    case '2': idx = 7; break;
    case '+': idx = 8; break;
    default: return;
    }
    for (int r = 0; r < 5; r++) {
        uint8_t row = GLYPH_3X5[idx][r];
        int py = y + r;
        if (py < 0 || py >= GAME_H) continue;
        for (int col = 0; col < 3; col++) {
            if ((row >> (2 - col)) & 1) {
                int px = x + col;
                if (px >= 0 && px < GAME_W) fb[py * GAME_W + px] = color;
            }
        }
    }
}

static void draw_badge_text(uint16_t *fb, int x, int y, const char *str, uint16_t text_color, uint16_t border_color) {
    int len = 0; while (str[len]) len++;
    int w = len * 4 + 3, h = 7, bx = x - 2, by = y - 1;
    for (int py = by; py < by + h; py++) {
        if (py < 0 || py >= GAME_H) continue;
        for (int px = bx; px < bx + w; px++) {
            if (px < 0 || px >= GAME_W) continue;
            int is_edge = (py == by || py == by + h - 1 || px == bx || px == bx + w - 1);
            fb[py * GAME_W + px] = is_edge ? border_color : (uint16_t)0x18C3;
        }
    }
    for (int i = 0; i < len; i++) draw_char(fb, x + i * 4, y, str[i], text_color);
}

void native_draw_hotbar_overlay(uint16_t *fb) {
    if (!fb || !g_patch_hide_pad || !g_quick_slot_drawn) return;
    g_quick_slot_drawn = 0;
    uint32_t canvas = mem_r32(LIB_BASE + 0x44278);
    if (canvas) {
        uint16_t st = 0;
        mem_read(canvas + 0x4854, &st, 2);
        if (st != 0) return; /* Suppress overlay in menus/dialogs */
    }
    if (g_hotbar_page == 0) {
        /* Page 1 (Neutral): silver badges, [L] P1 */
        draw_badge_text(fb, 236, 218, "L P1", 0xB5F9, 0x4A69);
        draw_badge_text(fb, 258, 202, "ZL", 0xFFFF, 0x4A69);
        draw_badge_text(fb, 287, 202, "Y", 0xFFFF, 0x4A69);
        draw_badge_text(fb, 311, 202, "X", 0xFFFF, 0x4A69);
        draw_badge_text(fb, 335, 202, "R", 0xFFFF, 0x4A69);
        draw_badge_text(fb, 357, 202, "ZR", 0xFFFF, 0x4A69);
    } else {
        /* Page 2 (Holding L): Arcane Cyan glow border (#38E1FF) + L+ badges + P2 */
        const uint16_t CYAN = 0x3F1F;
        const uint16_t GOLD = 0xFEC7;
        /* Draw glowing border */
        for (int px = 233; px <= 378; px++) {
            fb[208 * GAME_W + px] = CYAN;
            fb[234 * GAME_W + px] = CYAN;
        }
        for (int py = 208; py <= 234; py++) {
            fb[py * GAME_W + 233] = CYAN;
            fb[py * GAME_W + 378] = CYAN;
        }
        draw_badge_text(fb, 236, 218, "L P2", GOLD, CYAN);
        draw_badge_text(fb, 250, 202, "L+ZL", CYAN, CYAN);
        draw_badge_text(fb, 277, 202, "L+Y", CYAN, CYAN);
        draw_badge_text(fb, 301, 202, "L+X", CYAN, CYAN);
        draw_badge_text(fb, 325, 202, "L+R", CYAN, CYAN);
        draw_badge_text(fb, 347, 202, "L+ZR", CYAN, CYAN);
    }
}

/* Title menu (MenuCanvas, state 1: START / MULTI MODE / OPTION / HELP / INFO). The game keeps the key-driven selection in
 * menu->+0xcc but draws nothing for it (it is touch-first), so for gamepad play the host marks the selected entry. */
static uint16_t blend565(uint16_t a, uint16_t b, int alpha /* 0..256 of b */) {
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((ar * (256 - alpha) + br * alpha) >> 8) << 11) | (((ag * (256 - alpha) + bg * alpha) >> 8) << 5) | ((ab * (256 - alpha) + bb * alpha) >> 8));
}
void native_draw_title_overlay(uint16_t *fb) {
    if (!fb) return;
    uint32_t scr = mem_r32(LIB_BASE + 0x44240), menu = mem_r32(LIB_BASE + 0x4439c);
    if (!scr || !menu || mem_r32(scr) != LIB_BASE + 0x77788) return;
    int32_t state = (int32_t)mem_r32(menu + 0x9c), idx = (int32_t)mem_r32(menu + 0xcc);
    if (state != 1 || idx < 0 || idx > 4) return;
    static const int cy[5] = {68, 103, 140, 175, 211}, hw[5] = {50, 82, 58, 42, 42};   /* centre row, half width of each label */
    const uint16_t GOLD = 0xFEC7, DARK = 0x0000;
    int x0 = 200 - hw[idx], x1 = 200 + hw[idx], y0 = cy[idx] - 15, y1 = cy[idx] + 15;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        uint16_t *px = &fb[y * GAME_W + x];
        int edge = (y <= y0 + 1 || y >= y1 - 1 || x <= x0 + 1 || x >= x1 - 1);
        *px = edge ? GOLD : blend565(*px, DARK, 90);
    }
    for (int i = 0; i < 9; i++) for (int j = -i; j <= i; j++) {        /* arrows pointing at the entry from both sides */
        int ly = cy[idx] + j, lx = x0 - 14 + (8 - i) , rx = x1 + 14 - (8 - i);
        if (lx >= 0 && lx < GAME_W) fb[ly * GAME_W + lx] = GOLD;
        if (rx >= 0 && rx < GAME_W) fb[ly * GAME_W + rx] = GOLD;
    }
}

/* Performance readout ("FPS 60 G 9 MAX 31"): 3x5 font for digits and the few letters it needs. */
static const uint8_t PF_DIGITS[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
static const uint8_t PF_F[5] = {7,4,6,4,4}, PF_P[5] = {6,5,6,4,4}, PF_S[5] = {7,4,7,1,7}, PF_M[5] = {5,7,7,5,5}, PF_A[5] = {2,5,7,5,5}, PF_X[5] = {5,5,2,5,5}, PF_G[5] = {7,4,5,5,7};
static void pf_char(uint16_t *fb, int x, int y, char c, uint16_t col) {
    const uint8_t *g = 0;
    if (c >= '0' && c <= '9') g = PF_DIGITS[c - '0'];
    else switch (c) { case 'F': g = PF_F; break; case 'P': g = PF_P; break; case 'S': g = PF_S; break; case 'M': g = PF_M; break;
                      case 'A': g = PF_A; break; case 'X': g = PF_X; break; case 'G': g = PF_G; break; default: return; }
    for (int r = 0; r < 5; r++) for (int cx = 0; cx < 3; cx++) if ((g[r] >> (2 - cx)) & 1) {
        int px = x + cx, py = y + r; if (px >= 0 && px < GAME_W && py >= 0 && py < GAME_H) fb[py * GAME_W + px] = col; }
}
void native_draw_perf_overlay(uint16_t *fb, const char *text) {
    if (!fb || !text) return;
    int len = 0; while (text[len]) len++;
    for (int y = 1; y < 9; y++) for (int x = 1; x < len * 4 + 4; x++) fb[y * GAME_W + x] = blend565(fb[y * GAME_W + x], 0x0000, 200);
    for (int i = 0; i < len; i++) pf_char(fb, 3 + i * 4, 3, text[i], 0xFFFF);
}

/* ------------------------------------------------------------------ economy
 * The original sells "Coins" (the premium currency) for real money through Google Play. Here the Special Shop sells them for
 * Gold instead: the six price tiers keep the original coin amounts and "% MORE" bonuses but are priced in Gold, and the
 * Resurrection-Scroll pack offered on the death screen is bought with Gold too. Nothing touches the network.
 *   - gametxt_en.tdt: every sentence that talks about dollars/charging is rewritten when the asset is read (the APK is not modified);
 *   - the "$%d.%d" price format and the CASH_RATIO price column are patched in guest memory after init;
 *   - Comm_buyCash (the IAP request) is replaced: it records the purchase and economy_step() settles it a frame later.
 * Prices: 1 base coin = ECON_GOLD_PER_COIN gold, before the volume bonus (see docs/economy.md). */
#define ECON_GOLD_PER_COIN   2                                   /* gold per coin of the bonus-less base amount */
#define ECON_REVIVE_GOLD     2000                                /* Resurrection Scroll + 800 Coins pack */
static const int ECON_BASE_COINS[6] = {100000, 50000, 30000, 10000, 5000, 1000};   /* CASH_RATIO rows, biggest first */
static int econ_tier_gold(int t) { return ECON_BASE_COINS[t] * ECON_GOLD_PER_COIN; }

static const struct { const char *from, *to; } ECON_TEXT[] = {
    {"$%d.%d will be charged to purchase %d Coins.", "%d%.0d Gold will be spent to buy %d Coins."},
    {"$%d.%d will be charged.  (It may take a while to process your request, so please do not terminate the game in the meantime.)", "%d%.0d Gold will be spent."},
    {"$1.99 will be charged upon upgrading to the full version. Do you want to purchase the full version and continue playing the game?", "Do you want to unlock the full version and continue playing the game?"},
    {"$1.99 (Upgrade to full version)", "Upgrade to full version"},
    {"4000\xbf\xf8( (Upgrade to full version)", "Upgrade to full version"},
    {"You do not have a Resurrection Scroll. Do you want to purchase a Resurrection Scroll and 800 Coins?", "You have no Resurrection Scroll. Buy a Resurrection Scroll and 800 Coins for 2,000 Gold?"},
    {"You do not have a Resurrection Scroll. Do you want to purchase a Resurrection Scroll and 20 Coins?", "You have no Resurrection Scroll. Buy a Resurrection Scroll and 20 Coins for 2,000 Gold?"},
    {"You need Coins to use the Special Shop.", "Exchange Gold for Coins to use the Special Shop."},
};
/* gametxt_*en.tdt: u16 record count, u16 version, then {u16 length, bytes}. Returns a malloc'd replacement (or the input). */
uint8_t *economy_patch_asset(const char *name, uint8_t *data, size_t *len) {
    if (!data || !name || strncmp(name, "gametxt", 7) || !strstr(name, "_en.tdt") || *len < 4) return data;
    size_t cap = *len + 1024, o = 4, w = 4; uint8_t *out = (uint8_t *)malloc(cap); int changed = 0;
    memcpy(out, data, 4);
    while (o + 2 <= *len) {
        size_t l = (size_t)(data[o] | data[o + 1] << 8); o += 2;
        if (o + l > *len) { free(out); return data; }
        const char *to = NULL;
        for (size_t i = 0; i < sizeof ECON_TEXT / sizeof *ECON_TEXT; i++)
            if (strlen(ECON_TEXT[i].from) == l && !memcmp(ECON_TEXT[i].from, data + o, l)) { to = ECON_TEXT[i].to; break; }
        size_t nl = to ? strlen(to) : l; const uint8_t *src = to ? (const uint8_t *)to : data + o;
        if (w + 2 + nl > cap) { cap = (w + 2 + nl) * 2; out = (uint8_t *)realloc(out, cap); }
        out[w] = (uint8_t)nl; out[w + 1] = (uint8_t)(nl >> 8); memcpy(out + w + 2, src, nl); w += 2 + nl; o += l; changed |= to != NULL;
    }
    if (!changed) { free(out); return data; }
    free(data); *len = w; return out;
}

/* After game_init: price column of CASH_RATIO in Gold (x100: the game prints value/100 with the patched "%d G" format). */
void economy_install(void) {
    for (int t = 0; t < 6; t++) S32(LIB_BASE + 0x44fc8 + (uint32_t)t * 8 + 4, (uint32_t)econ_tier_gold(t) * 100u);
    uint32_t span = guest_lib_span();
    for (uint32_t a = LIB_BASE; a + 7 <= LIB_BASE + span; a++)
        if (!memcmp(HP(a), "$%d.%d", 7)) { memcpy(HP(a), "%d G\0\0\0", 7); }
}

static struct { int kind, tier; } g_buy;          /* 1 = Special Shop tier, 2 = death-screen revive pack */
static int g_buy_msg_swapped;
#define ECON_MSG_OK  "You have purchased Coins."
#define ECON_MSG_NO  "Not enough Gold."
/* The "purchased" popup is the only message popup that is safe to open from the tier list, so a refused purchase reuses it with
 * different text: every copy of the sentence in guest memory is overwritten and put back once the popup is gone. */
static void econ_swap_msg(int to_no) {          /* text records have no terminator: swap in place, same length, space padded */
    size_t n = strlen(ECON_MSG_OK); char pad[64]; memset(pad, ' ', n); memcpy(pad, ECON_MSG_NO, strlen(ECON_MSG_NO)); pad[n] = 0;
    const char *from = to_no ? ECON_MSG_OK : pad, *to = to_no ? pad : ECON_MSG_OK;
    for (int rg = 0; rg < 2; rg++) {            /* the sentence exists in the text table (heap) and in the library's rodata */
        uint32_t base = rg ? HEAP_BASE : LIB_BASE, hn = rg ? guest_heap_end() - HEAP_BASE : guest_lib_span(); uint8_t *h = HP(base);
        for (uint32_t i = 0; i + n <= hn; i++) if (h[i] == (uint8_t)from[0] && !memcmp(h + i, from, n)) memcpy(h + i, to, n);
    }
    g_buy_msg_swapped = to_no;
}
static int p_buy_cash(Cpu *c) {
    uint32_t lr = c->r[14] & ~1u, arg = c->r[0];
    if (lr >= LIB_BASE + 0x20159 && lr < LIB_BASE + 0x21065) { if (arg == 6) { g_buy.kind = 2; g_buy.tier = 6; } }
    else if (lr >= LIB_BASE + 0x35dbd && lr < LIB_BASE + 0x35dbd + 31168) { if (arg < 6) { g_buy.kind = 1; g_buy.tier = (int)arg; } }
    c->r[15] = c->r[14] & ~1u; return 1;                 /* no network: the purchase is settled by economy_step() */
}
void economy_step(void) {
    uint32_t menu = mem_r32(LIB_BASE + 0x441d4);
    if (!menu) return;
    if (g_buy_msg_swapped && mem_r32(menu + 0x7c) != 0x81100004) econ_swap_msg(0);    /* popup closed: restore the text */
    if (!g_buy.kind) return;
    int kind = g_buy.kind, tier = g_buy.tier; g_buy.kind = 0;
    uint32_t gold = game_call("getMyGold", 0, NULL);
    if (kind == 2) {
        /* Revive on death: full Revive Pack costs 2000 Gold (gives 800 Coins + instant revive).
         * If the player has less than 2000 Gold, charge an emergency fee (min(gold, 200)) and
         * still revive them immediately, so the player is never locked out with 0 response. */
        uint32_t cost = gold >= ECON_REVIVE_GOLD ? ECON_REVIVE_GOLD : (gold >= 200 ? 200 : gold);
        uint32_t coins = (gold >= ECON_REVIVE_GOLD) ? 800 : 0;
        uint32_t a[1];
        a[0] = gold >= cost ? gold - cost : 0; game_call("setMyGold", 1, a);
        if (coins) { a[0] = game_call("getMyCash", 0, NULL) + coins; game_call("setMyCash", 1, a); }
        game_call("MMain_SaveData", 0, NULL);
        game_call("GameCanvas_setHeroRevive", 0, NULL);
        return;
    }
    uint32_t cost = (uint32_t)econ_tier_gold(tier);
    uint32_t coins = mem_r32(LIB_BASE + 0x44fc8 + (uint32_t)tier * 8);
    if (gold < cost) { econ_swap_msg(1); mem_w32(menu + 0x7c, 0x81100004); return; }
    uint32_t a[1];
    a[0] = gold - cost; game_call("setMyGold", 1, a);
    a[0] = game_call("getMyCash", 0, NULL) + coins; game_call("setMyCash", 1, a);
    game_call("MMain_SaveData", 0, NULL);
    mem_w32(menu + 0x7c, 0x81100004);      /* "You have purchased Coins." */
}

typedef struct { const char *name; native_fn fn; } PatchDef;
static const PatchDef PATCHES[] = {
    {"MMain_drawTouchButton", p_hide_pad},
    {"GameUI_drawStatus", p_draw_status},
    {"GameUI_drawQuickSlot", p_draw_quick_slot},
    {"Comm_buyCash", p_buy_cash},
};
#define NPATCHES ((int)(sizeof PATCHES / sizeof *PATCHES))

/* ------------------------------------------------------------ verification */
static int g_verify_every; static uint64_t g_verified[64], g_calls[64];
static uint8_t *snap_a, *snap_n; static size_t snap_cap;

static void region(uint32_t base, uint32_t n, uint8_t **host) { *host = HP(base); (void)n; }

static int verify_hook(Cpu *c) {
    const NativeDef *d = &DEFS[g_hook_cur - 1];
    uint64_t n = ++g_calls[g_hook_cur];      /* first 200 calls of every function are checked, then every Nth */
    if (n > 200 && n % (uint64_t)g_verify_every) return d->fn(c);
    uint32_t lib_n = (guest_lib_span() + 0xfff) & ~0xfffu, heap_n = guest_heap_end() - HEAP_BASE;
    size_t need = (size_t)lib_n + heap_n;
    if (need > snap_cap) { snap_cap = need * 2; snap_a = (uint8_t *)realloc(snap_a, snap_cap); snap_n = (uint8_t *)realloc(snap_n, snap_cap); }
    uint8_t *lib, *heap; region(LIB_BASE, lib_n, &lib); region(HEAP_BASE, heap_n, &heap);
    memcpy(snap_a, lib, lib_n); memcpy(snap_a + lib_n, heap, heap_n);            /* A: state before the call */
    Cpu entry = *c; uint32_t entry_pc = c->r[15];
    uint32_t args[8]; for (int i = 0; i < 8; i++) args[i] = cpu_arg(c, i);
    int rc = d->fn(c);
    if (!rc) return 0;                           /* declined (not handled): guest code runs as-is, nothing was modified */
    uint32_t nat_r0 = c->r[0];
    memcpy(snap_n, lib, lib_n); memcpy(snap_n + lib_n, heap, heap_n);            /* N: native result */
    memcpy(lib, snap_a, lib_n); memcpy(heap, snap_a + lib_n, heap_n);            /* back to A */
    Cpu g = entry; cpu_run_original(&g, entry_pc, d->join ? ABS(d->join) : 0);   /* G: real guest code */
    int bad = 0;
    if (d->returns && nat_r0 != g.r[0]) { fprintf(stderr, "VERIFY %s: r0 native %#x guest %#x\n", d->name, nat_r0, g.r[0]); bad = 1; }
    for (size_t i = 0, shown = 0; i < need && shown < 8; i++) {
        uint8_t gv = i < lib_n ? lib[i] : heap[i - lib_n];
        if (gv != snap_n[i]) {
            uint32_t ga = i < lib_n ? LIB_BASE + (uint32_t)i : HEAP_BASE + (uint32_t)(i - lib_n);
            fprintf(stderr, "VERIFY %s: mem %08x native %02x guest %02x (was %02x)\n", d->name, ga, snap_n[i], gv, snap_a[i]); bad = 1; shown++;
        }
    }
    if (bad) {
        fprintf(stderr, "args: "); for (int i = 0; i < 8; i++) fprintf(stderr, "%08x ", args[i]);
        fprintf(stderr, "\nGrp mode=%u alpha=%u key=%#x\n", GRP_MODE(V.Grp), GRP_ALPHA(V.Grp), GRP_KEY(V.Grp));
        exit(20);
    }
    g_verified[g_hook_cur]++;
    for (int i = 0; i < 13; i++) c->r[i] = g.r[i];
    c->n = g.n; c->z = g.z; c->c = g.c; c->v = g.v;
    if (d->join) { c->r[15] = ABS(d->join); return 2; }
    return 1;
}

void natives_report(void) {
    if (!g_verify_every) return;
    for (int i = 1; i <= NDEFS; i++) fprintf(stderr, "verified %-28s %llu calls OK\n", DEFS[i - 1].name, (unsigned long long)g_verified[i]);
}

void natives_install(int verify_every) {
    g_hook_span = guest_lib_span(); g_hookmap = (uint8_t *)calloc(g_hook_span / 2 + 1, 1);
    for (int i = 0; i < NPATCHES; i++) {
        g_hookmap[((guest_sym(PATCHES[i].name) & ~1u) - LIB_BASE) >> 1] = (uint8_t)(NDEFS + 1 + i);
        g_hook_fn[NDEFS + 1 + i] = PATCHES[i].fn;
    }
    if (verify_every < 0) return;                       /* overrides disabled */
    #define SYMV(f, name) V.f = guest_sym(name) & ~1u
    SYMV(WIPIMEM, "WIPIMEM"); V.zstate = G32(LIB_BASE + 0x442b0); SYMV(Grp, "Grp"); SYMV(maxR, "maxR"); SYMV(maxG, "maxG"); SYMV(maxB, "maxB"); SYMV(maskR, "maskR"); SYMV(maskG, "maskG");
    SYMV(maskB, "maskB"); SYMV(shiftR, "shiftR"); SYMV(shiftG, "shiftG"); SYMV(lenR, "lenR"); SYMV(lenG, "lenG"); SYMV(lenB, "lenB");
    SYMV(gProcVarS, "gProcVarS"); SYMV(gProcVarB, "gProcVarB"); SYMV(gProcVarR, "gProcVarR");
    SYMV(frameBuffer, "frameBuffer"); SYMV(mainScreenWidth, "mainScreenWidth"); SYMV(mainScreenHeight, "mainScreenHeight");
    g_verify_every = verify_every;
    for (int i = 0; i < NDEFS; i++) {
        uint32_t a = DEFS[i].addr ? ABS(DEFS[i].addr) : (guest_sym(DEFS[i].name) & ~1u);
        g_hookmap[(a - LIB_BASE) >> 1] = (uint8_t)(i + 1);
        g_hook_fn[i + 1] = verify_every ? verify_hook : DEFS[i].fn;
    }
}
