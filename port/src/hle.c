/* hle.c -- loads libdestinia_jni.so into the guest and implements everything it
 * imports: 28 libc/ABI symbols, the JNIEnv/JavaVM tables, and the Java-side
 * static methods the game calls back into (assets, sound, vibration). This is a C
 * port of harness/host.py, which is the verified reference (see docs/). MIT. */
#include "game.h"
#include "guest.h"
#include "jni_names.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Cpu g_cpu;
static GameHost g_host;
static int g_log;                       /* log libc/JNI/Java events to stderr */
static uint32_t g_vclock_us = 1000000;  /* deterministic clock when host.clock_us == NULL */
static unsigned g_frame_no;
static int g_pending_comm;      /* a purchase request waiting to be answered (see game_run) */
static int g_pending_buy_idx;
static uint32_t g_span;
static int g_native_mode = 0;           /* see game_set_natives */
void game_set_natives(int mode) { g_native_mode = mode; }                 /* byte span of the loaded library */

/* ------------------------------------------------------------- stubs ---- */
static int n_stubs;
static uint32_t new_stub(const char *name, hle_fn fn);
static uint32_t hle_unimpl(Cpu *c) {
    uint32_t idx = (c->r[15] - STUB_BASE) >> 3;
    static unsigned char seen[STUB_SIZE / 8];
    if (!seen[idx]) {
        seen[idx] = 1;
        fprintf(stderr, "!! unimplemented import %s (r0=%x r1=%x r2=%x r3=%x lr=%x)\n",
                g_stub_name[idx], c->r[0], c->r[1], c->r[2], c->r[3], c->r[14]);
    }
    return 0;
}
static uint32_t new_stub(const char *name, hle_fn fn) {
    int idx = n_stubs++;
    if (idx >= (int)(STUB_SIZE / 8)) guest_fatal(2, "too many stubs");
    g_stub_fn[idx] = fn ? fn : hle_unimpl;
    g_stub_name[idx] = name;
    return STUB_BASE + (uint32_t)idx * 8;
}

/* ------------------------------------------------------------ allocator -- */
/* Same policy as the reference harness: 16-byte rounding, bump allocation,
 * exact-size LIFO free lists. Keeping it identical keeps guest pointers identical. */
#define FREE_BUCKETS 4096
typedef struct FreeNode { uint32_t ptr, size; struct FreeNode *next; } FreeNode;
static FreeNode *g_free[FREE_BUCKETS];
static FreeNode *g_free_big;            /* sizes >= FREE_BUCKETS*16: exact match, most recent first */
static FreeNode *g_freenode_pool;

static inline FreeNode *alloc_freenode(void) {
    if (g_freenode_pool) {
        FreeNode *f = g_freenode_pool;
        g_freenode_pool = f->next;
        return f;
    }
    return (FreeNode *)malloc(sizeof(FreeNode));
}

static inline void free_freenode(FreeNode *f) {
    f->next = g_freenode_pool;
    g_freenode_pool = f;
}

#define SIZES_HASH_BITS 15
#define SIZES_HASH_SIZE (1 << SIZES_HASH_BITS)
#define SIZES_HASH_MASK (SIZES_HASH_SIZE - 1)

typedef struct SizeNode {
    uint32_t ptr;
    uint32_t size;
    struct SizeNode *next;
} SizeNode;

static SizeNode *g_size_buckets[SIZES_HASH_SIZE];
static SizeNode *g_size_free_list;
static uint32_t g_heap_ptr = HEAP_BASE + 0x1000;

static uint32_t rounded(uint32_t n) { if (n < 16) n = 16; return (n + 15) & ~15u; }

static void size_set(uint32_t p, uint32_t n) {
    unsigned h = ((p >> 4) * 2654435761u) & SIZES_HASH_MASK;
    SizeNode *node = g_size_free_list;
    if (node) {
        g_size_free_list = node->next;
    } else {
        node = (SizeNode *)malloc(sizeof(SizeNode));
    }
    node->ptr = p;
    node->size = n;
    node->next = g_size_buckets[h];
    g_size_buckets[h] = node;
}

static int size_take(uint32_t p, uint32_t *n) {
    unsigned h = ((p >> 4) * 2654435761u) & SIZES_HASH_MASK;
    for (SizeNode **pp = &g_size_buckets[h]; *pp; pp = &(*pp)->next) {
        if ((*pp)->ptr == p) {
            SizeNode *node = *pp;
            *n = node->size;
            *pp = node->next;
            node->next = g_size_free_list;
            g_size_free_list = node;
            return 1;
        }
    }
    return 0;
}

static uint32_t gmalloc(uint32_t n) {
    n = rounded(n);
    if (n / 16 < FREE_BUCKETS) {
        FreeNode *f = g_free[n / 16];
        if (f) {
            g_free[n / 16] = f->next;
            uint32_t p = f->ptr;
            free_freenode(f);
            size_set(p, n);
            return p;
        }
    } else {
        for (FreeNode **pp = &g_free_big; *pp; pp = &(*pp)->next) {
            if ((*pp)->size == n) {
                FreeNode *f = *pp;
                *pp = f->next;
                uint32_t p = f->ptr;
                free_freenode(f);
                size_set(p, n);
                return p;
            }
        }
    }
    uint32_t p = g_heap_ptr; g_heap_ptr += n;
    if (g_heap_ptr > HEAP_BASE + HEAP_SIZE) guest_fatal(6, "guest heap exhausted");
    size_set(p, n); return p;
}

static void gfree(uint32_t p) {
    uint32_t n;
    if (!size_take(p, &n)) return;
    FreeNode **head = n / 16 < FREE_BUCKETS ? &g_free[n / 16] : &g_free_big;
    FreeNode *f = alloc_freenode();
    f->ptr = p; f->size = n; f->next = *head; *head = f;
}

/* ------------------------------------------------------------ helpers ---- */
#define A(i) cpu_arg(c, (i))
static void gmemcpy(uint32_t d, uint32_t s, uint32_t n) {
    if (!n) return;
    uint8_t *tmp = (uint8_t *)malloc(n); mem_read(s, tmp, n); mem_write(d, tmp, n); free(tmp);
}
static void gmemset(uint32_t d, uint8_t v, uint32_t n) {
    while (n) { uint32_t ch = 4096 - (d & 0xfff); if (ch > n) ch = n; memset(mem_ptr(d), v, ch); d += ch; n -= ch; }
}
static char *gstr_dup(uint32_t a) {                    /* malloc'd host copy of a guest C string */
    uint32_t n = 0; while (mem_r8(a + n)) n++;
    char *s = (char *)malloc(n + 1); mem_read(a, s, n); s[n] = 0; return s;
}

/* -------------------------------------------------- printf formatting ---- */
static void gfmt(Cpu *c, const char *fmt, int argi, char *out, size_t outsz) {
    size_t o = 0;
    #define PUT(ch) do { if (o + 1 < outsz) out[o++] = (ch); } while (0)
    for (const char *p = fmt; *p;) {
        if (*p != '%') { PUT(*p++); continue; }
        const char *start = p++;
        char spec[32]; int sl = 0; spec[sl++] = '%';
        while (*p && strchr("-+ 0#", *p) && sl < 20) spec[sl++] = *p++;
        while (isdigit((unsigned char)*p) && sl < 24) spec[sl++] = *p++;
        if (*p == '.') { spec[sl++] = *p++; while (isdigit((unsigned char)*p) && sl < 28) spec[sl++] = *p++; }
        while (*p == 'l' || *p == 'h') p++;
        char cv = *p ? *p++ : 0; char tmp[512];
        if (cv == '%') { PUT('%'); continue; }
        if (!strchr("diuxXcsp", cv)) { for (; start < p; start++) PUT(*start); continue; }
        uint32_t v = cpu_arg(c, argi++);
        if (cv == 's') { char *s = gstr_dup(v); spec[sl++] = 's'; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, s); free(s); }
        else if (cv == 'c') { spec[sl++] = 'c'; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, (int)(v & 0xff)); }
        else if (cv == 'p') { spec[sl++] = 's'; spec[sl] = 0; char h[16]; snprintf(h, sizeof h, "0x%x", v); snprintf(tmp, sizeof tmp, spec, h); }
        else if (cv == 'd' || cv == 'i') { spec[sl++] = 'd'; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, (int)v); }
        else { spec[sl++] = cv == 'u' ? 'u' : cv; spec[sl] = 0; snprintf(tmp, sizeof tmp, spec, v); }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    out[o < outsz ? o : outsz - 1] = 0;
    #undef PUT
}

/* ---------------------------------------------------------- libc imports - */
static uint32_t c_stack_chk_fail(Cpu *c) { (void)c; guest_fatal(7, "stack smashing detected"); }
static uint32_t c_unwind(Cpu *c) { (void)c; return 0; }
static uint32_t c_idiv(Cpu *c) { int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1]; return b == 0 ? 0 : (b == -1 ? (uint32_t)(0u - (uint32_t)a) : (uint32_t)(a / b)); }
static uint32_t c_uidiv(Cpu *c) { return c->r[1] ? c->r[0] / c->r[1] : 0; }
static uint32_t c_uidivmod(Cpu *c) { uint32_t a = c->r[0], b = c->r[1]; c->r[1] = b ? a % b : 0; return b ? a / b : 0; }
static uint32_t c_idivmod(Cpu *c) {
    int32_t a = (int32_t)c->r[0], b = (int32_t)c->r[1];
    if (b == 0) { c->r[1] = 0; return 0; }
    if (b == -1) { c->r[1] = 0; return 0u - (uint32_t)a; }
    c->r[1] = (uint32_t)(a % b); return (uint32_t)(a / b);
}
static uint32_t c_ldivmod(Cpu *c) {
    int64_t a = (int64_t)(((uint64_t)c->r[1] << 32) | c->r[0]), b = (int64_t)(((uint64_t)c->r[3] << 32) | c->r[2]);
    int64_t q = 0, rem = 0;
    if (b != 0) { if (b == -1) q = (int64_t)(0 - (uint64_t)a); else { q = a / b; rem = a % b; } }
    c->r[1] = (uint32_t)((uint64_t)q >> 32); c->r[2] = (uint32_t)rem; c->r[3] = (uint32_t)((uint64_t)rem >> 32);
    return (uint32_t)q;
}
static uint32_t c_lmul(Cpu *c) {
    uint64_t a = ((uint64_t)c->r[1] << 32) | c->r[0], b = ((uint64_t)c->r[3] << 32) | c->r[2], p = a * b;
    c->r[1] = (uint32_t)(p >> 32); return (uint32_t)p;
}
static uint32_t c_malloc(Cpu *c) { return gmalloc(c->r[0]); }
static uint32_t c_free(Cpu *c) { gfree(c->r[0]); return 0; }
static uint32_t c_memcpy(Cpu *c) { gmemcpy(c->r[0], c->r[1], c->r[2]); return c->r[0]; }
static uint32_t c_memset(Cpu *c) { gmemset(c->r[0], (uint8_t)c->r[1], c->r[2]); return c->r[0]; }
static uint32_t c_strlen(Cpu *c) { uint32_t n = 0; while (mem_r8(c->r[0] + n)) n++; return n; }
static uint32_t c_strcpy(Cpu *c) { uint32_t d = c->r[0], s = c->r[1], i = 0; for (;; i++) { uint8_t b = mem_r8(s + i); *mem_ptr(d + i) = b; if (!b) break; } return d; }
static uint32_t c_strncmp(Cpu *c) {
    uint32_t a = c->r[0], b = c->r[1], n = c->r[2];
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = mem_r8(a + i), y = mem_r8(b + i);
        if (x != y) return x > y ? 1u : 0xffffffffu;
        if (!x) break;
    }
    return 0;
}
static uint32_t c_strstr(Cpu *c) {
    char *h = gstr_dup(c->r[0]), *n = gstr_dup(c->r[1]); char *f = strstr(h, n);
    uint32_t r = f ? c->r[0] + (uint32_t)(f - h) : 0; free(h); free(n); return r;
}
static uint32_t c_clock(Cpu *c) { (void)c; return g_host.clock_us ? g_host.clock_us() : g_vclock_us; }
static uint32_t c_android_log_print(Cpu *c) {
    char *tag = gstr_dup(c->r[1]), *fmt = gstr_dup(c->r[2]); char msg[1024];
    gfmt(c, fmt, 3, msg, sizeof msg);
    if (g_log) fprintf(stderr, "[log:%s] %s\n", tag, msg);
    free(tag); free(fmt); return 0;
}
static uint32_t c_sprintf(Cpu *c) {
    char *fmt = gstr_dup(c->r[1]); char msg[2048];
    gfmt(c, fmt, 2, msg, sizeof msg); free(fmt);
    size_t n = strlen(msg); mem_write(c->r[0], msg, (uint32_t)n + 1); return (uint32_t)n;
}

/* files: native paths are mapped into the save dir by basename */
#define MAX_FILES 64
typedef struct {
    FILE *f;
    char path[512];
    char tmp[516];
    int is_write;
} HostFile;
static HostFile g_files[MAX_FILES];

static void host_path(uint32_t gpath, char *out, size_t sz) {
    char *p = gstr_dup(gpath); for (char *q = p; *q; q++) if (*q == '\\') *q = '/';
    const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
    snprintf(out, sz, "%s/%s", g_host.save_dir ? g_host.save_dir : ".", base); free(p);
}
static uint32_t c_fopen(Cpu *c) {
    char path[500], mode[16], m2[24]; host_path(c->r[0], path, sizeof path); mem_cstr(c->r[1], mode, sizeof mode);
    snprintf(m2, sizeof m2, "%s%s", mode, strchr(mode, 'b') ? "" : "b");
    int is_w = (strchr(mode, 'w') != NULL);
    char open_path[516];
    if (is_w) {
        snprintf(open_path, sizeof open_path, "%s.tmp", path);
    } else {
        snprintf(open_path, sizeof open_path, "%s", path);
    }
    FILE *f = fopen(open_path, m2);
    if (!f && !is_w) {
        char tmp_path[516];
        snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
        f = fopen(tmp_path, m2);
    }
    if (!f) { if (g_log) fprintf(stderr, "fopen %s %s -> NULL\n", open_path, mode); return 0; }
    for (int i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].f) {
            g_files[i].f = f;
            snprintf(g_files[i].path, sizeof g_files[i].path, "%s", path);
            snprintf(g_files[i].tmp, sizeof g_files[i].tmp, "%s", open_path);
            g_files[i].is_write = is_w;
            if (g_log) fprintf(stderr, "fopen %s %s -> %#x\n", open_path, mode, 0x100u + 4u * (unsigned)i);
            return 0x100u + 4u * (unsigned)i;
        }
    }
    fclose(f); return 0;
}
static FILE *file_of(uint32_t h) { uint32_t i = (h - 0x100) / 4; return (h >= 0x100 && i < MAX_FILES) ? g_files[i].f : NULL; }
static uint32_t c_fclose(Cpu *c) {
    uint32_t i = (c->r[0] - 0x100) / 4;
    if (c->r[0] >= 0x100 && i < MAX_FILES && g_files[i].f) {
        FILE *f = g_files[i].f;
        fflush(f);
        int fd = fileno(f);
        if (fd >= 0) fsync(fd);
        fclose(f);
        if (g_files[i].is_write) {
            remove(g_files[i].path);
            rename(g_files[i].tmp, g_files[i].path);
        }
        memset(&g_files[i], 0, sizeof(HostFile));
    }
    return 0;
}
static uint32_t c_fread(Cpu *c) {
    FILE *f = file_of(c->r[3]); uint32_t sz = c->r[1], n = c->r[2]; if (!f || !sz) return 0;
    uint8_t *tmp = (uint8_t *)malloc((size_t)sz * n + 1); size_t got = fread(tmp, 1, (size_t)sz * n, f);
    mem_write(c->r[0], tmp, (uint32_t)got); free(tmp); return (uint32_t)(got / sz);
}
static uint32_t c_fwrite(Cpu *c) {
    FILE *f = file_of(c->r[3]); uint32_t sz = c->r[1], n = c->r[2]; if (!f) return 0;
    uint8_t *tmp = (uint8_t *)malloc((size_t)sz * n + 1); mem_read(c->r[0], tmp, sz * n);
    fwrite(tmp, 1, (size_t)sz * n, f); free(tmp); return n;
}
static uint32_t c_fseek(Cpu *c) { FILE *f = file_of(c->r[0]); if (f) fseek(f, (int32_t)c->r[1], (int)c->r[2]); return 0; }
static uint32_t c_ftell(Cpu *c) { FILE *f = file_of(c->r[0]); return f ? (uint32_t)ftell(f) : 0; }
static uint32_t c_remove(Cpu *c) {
    char path[500], tmp[516]; host_path(c->r[0], path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(tmp);
    return remove(path) == 0 ? 0 : 0xffffffffu;
}

typedef struct Imp { const char *name; hle_fn fn; } Imp;
static const Imp IMPORTS[] = {
    {"__stack_chk_fail", c_stack_chk_fail}, {"__aeabi_unwind_cpp_pr0", c_unwind}, {"__aeabi_unwind_cpp_pr1", c_unwind},
    {"__aeabi_idiv", c_idiv}, {"__aeabi_uidiv", c_uidiv}, {"__aeabi_uidivmod", c_uidivmod},
    {"__aeabi_idivmod", c_idivmod}, {"__aeabi_ldivmod", c_ldivmod}, {"__aeabi_lmul", c_lmul},
    {"malloc", c_malloc}, {"free", c_free}, {"memcpy", c_memcpy}, {"memset", c_memset},
    {"strlen", c_strlen}, {"strcpy", c_strcpy}, {"strncmp", c_strncmp}, {"strstr", c_strstr},
    {"clock", c_clock}, {"__android_log_print", c_android_log_print}, {"sprintf", c_sprintf},
    {"fopen", c_fopen}, {"fclose", c_fclose}, {"fread", c_fread}, {"fwrite", c_fwrite},
    {"fseek", c_fseek}, {"ftell", c_ftell}, {"remove", c_remove},
};

/* ----------------------------------------------------------- JNI objects - */
enum { H_NONE, H_CLASS, H_METHOD, H_BYTES, H_STR };
typedef struct Handle {
    int kind; char *s1, *s2, *s3;   /* class name / method: class,name,sig / string */
    uint8_t *data; uint32_t len;    /* byte arrays */
    int is_global;
} Handle;
static Handle *g_h; static unsigned g_h_n, g_h_cap;
static int g_h_free_head = -1;
static unsigned g_h_checkpoint = 0;

static uint32_t new_handle(Handle h) {
    if (g_h_free_head >= 0) {
        unsigned i = (unsigned)g_h_free_head;
        g_h_free_head = (int)g_h[i].len;
        g_h[i] = h;
        return 0x1000u + 4u * i;
    }
    if (g_h_n == g_h_cap) { g_h_cap = g_h_cap ? g_h_cap * 2 : 256; g_h = (Handle *)realloc(g_h, g_h_cap * sizeof *g_h); }
    g_h[g_h_n] = h; return 0x1000u + 4u * g_h_n++;
}
static Handle *get_handle(uint32_t id) {
    unsigned i = (id - 0x1000u) / 4;
    if (id < 0x1000u || i >= g_h_n) guest_fatal(8, "bad JNI handle %#x", id);
    return &g_h[i];
}
static uint32_t new_bytes(const uint8_t *d, uint32_t n) {
    Handle h = {H_BYTES, NULL, NULL, NULL, (uint8_t *)calloc(n ? n : 1, 1), n, 0};
    if (d) memcpy(h.data, d, n);
    return new_handle(h);
}

static void jni_local_frame_pop(void) {
    if (!g_h_checkpoint) return;
    for (unsigned i = g_h_checkpoint; i < g_h_n; i++) {
        Handle *h = &g_h[i];
        if (h->kind != H_NONE && !h->is_global) {
            if (h->kind == H_BYTES && h->data) {
                free(h->data);
                h->data = NULL;
            } else if (h->kind == H_STR && h->s1) {
                free(h->s1);
                h->s1 = NULL;
            }
            h->kind = H_NONE;
            h->len = (uint32_t)g_h_free_head;
            g_h_free_head = (int)i;
        }
    }
}

/* ---- JNIEnv functions ---- */
static uint32_t j_GetVersion(Cpu *c) { (void)c; return 0x00010006; }
static uint32_t j_FindClass(Cpu *c) {
    char *n = gstr_dup(c->r[1]); if (g_log) fprintf(stderr, "JNI FindClass %s\n", n);
    Handle h = {H_CLASS, n, NULL, NULL, NULL, 0, 1}; return new_handle(h);
}
static uint32_t j_NewGlobalRef(Cpu *c) {
    uint32_t id = c->r[1];
    if (id >= 0x1000u) {
        unsigned i = (id - 0x1000u) / 4;
        if (i < g_h_n) g_h[i].is_global = 1;
    }
    return id;
}
static uint32_t j_zero(Cpu *c) { (void)c; return 0; }
static uint32_t j_DeleteLocalRef(Cpu *c) {
    uint32_t id = c->r[1];
    if (id >= 0x1000u) {
        unsigned i = (id - 0x1000u) / 4;
        if (i < g_h_n) {
            Handle *h = &g_h[i];
            if (h->kind != H_NONE && !h->is_global) {
                if (h->kind == H_BYTES && h->data) {
                    free(h->data);
                    h->data = NULL;
                } else if (h->kind == H_STR && h->s1) {
                    free(h->s1);
                    h->s1 = NULL;
                }
                h->kind = H_NONE;
                h->len = (uint32_t)g_h_free_head;
                g_h_free_head = (int)i;
            }
        }
    }
    return 0;
}
static uint32_t j_GetJavaVM(Cpu *c) { mem_w32(c->r[1], JNI_BASE + 0x700); return 0; }
static uint32_t j_GetStaticMethodID(Cpu *c) {
    Handle *cl = get_handle(c->r[1]); char *n = gstr_dup(c->r[2]), *sg = gstr_dup(c->r[3]);
    if (g_log) fprintf(stderr, "JNI GetStaticMethodID %s.%s%s\n", cl->s1, n, sg);
    Handle h = {H_METHOD, strdup(cl->s1), n, sg, NULL, 0, 1}; return new_handle(h);
}
static uint32_t j_GetArrayLength(Cpu *c) { return get_handle(c->r[1])->len; }
static uint32_t j_NewByteArray(Cpu *c) { return new_bytes(NULL, c->r[1]); }
static uint32_t j_GetByteArrayElements(Cpu *c) {
    Handle *h = get_handle(c->r[1]); uint32_t p = gmalloc(h->len + 4); mem_write(p, h->data, h->len);
    if (c->r[2]) mem_w32(c->r[2], 1);
    return p;
}
static uint32_t j_ReleaseByteArrayElements(Cpu *c) {
    Handle *h = get_handle(c->r[1]); uint32_t p = c->r[2], mode = c->r[3];
    if (mode != 2) mem_read(p, h->data, h->len);
    if (mode != 1) gfree(p);
    return 0;
}
static uint32_t j_GetByteArrayRegion(Cpu *c) {
    Handle *h = get_handle(c->r[1]); uint32_t s = c->r[2], n = c->r[3], dst = cpu_arg(c, 4);
    mem_write(dst, h->data + s, n); return 0;
}
static uint32_t j_SetByteArrayRegion(Cpu *c) {
    Handle *h = get_handle(c->r[1]); uint32_t s = c->r[2], n = c->r[3], src = cpu_arg(c, 4);
    mem_read(src, h->data + s, n); return 0;
}
static uint32_t j_GetStringUTFChars(Cpu *c) {
    Handle *h = get_handle(c->r[1]); uint32_t n = (uint32_t)strlen(h->s1) + 1, p = gmalloc(n); mem_write(p, h->s1, n); return p;
}
static uint32_t j_ReleaseStringUTFChars(Cpu *c) {
    uint32_t p = c->r[2];
    if (p) gfree(p);
    return 0;
}
static uint32_t j_NewStringUTF(Cpu *c) { Handle h = {H_STR, gstr_dup(c->r[1]), NULL, NULL, NULL, 0, 0}; return new_handle(h); }
static uint32_t j_GetStringUTFLength(Cpu *c) { return (uint32_t)strlen(get_handle(c->r[1])->s1); }

/* ---- Java side (what GameThread/CommManager static methods did on Android) ---- */
static char *name_from_array(uint32_t h) {
    Handle *a = get_handle(h); char *s = (char *)malloc(a->len + 1); memcpy(s, a->data, a->len); s[a->len] = 0; return s;
}
static int32_t java_static(const char *cls, const char *name, const uint32_t *args, uint32_t *obj_out) {
    (void)cls; *obj_out = 0;
    if (!strcmp(name, "getAssetResSize")) {
        char *n = name_from_array(args[0]); int sz = g_host.asset_size ? g_host.asset_size(n) : 0; free(n); return sz;
    }
    if (!strcmp(name, "getAssetRes")) {
        char *n = name_from_array(args[0]); size_t len = 0; uint8_t *d = g_host.asset_read ? g_host.asset_read(n, &len) : NULL; free(n);
        if (d) { *obj_out = new_bytes(d, (uint32_t)len); free(d); }
        return 0;
    }
    if (!strcmp(name, "playSound")) { if (g_host.play_sound) g_host.play_sound((int32_t)args[0]); return 0; }
    if (!strcmp(name, "setSoundVolume")) { if (g_host.set_volume) g_host.set_volume((int)args[0], (int)args[1], (int)args[2]); return 0; }
    if (!strcmp(name, "playVib")) { if (g_host.vibrate) g_host.vibrate((int)args[0] + 20); return 0; }
    if (!strcmp(name, "buyCash")) { g_pending_comm = 1; g_pending_buy_idx = (int)args[0]; return 0; }
    return 0;   /* logEvent, gotoURL, close: no-ops */
}
static uint32_t jni_call_static(Cpu *c, int kind) {
    Handle *m = get_handle(c->r[2]); uint32_t args[8]; int na = 0, ai = 3;
    if (!m || !m->s3) {
        fprintf(stderr, "[jni] ERROR: invalid method handle %#x\n", c->r[2]);
        return 0;
    }
    const char *lp = strchr(m->s3, '(');
    if (!lp) {
        fprintf(stderr, "[jni] ERROR: malformed signature in handle %#x: '%s'\n", c->r[2], m->s3);
        return 0;
    }
    const char *s = lp + 1;
    while (*s && *s != ')' && na < 8) {            /* one 32-bit slot per parameter (no long/double used) */
        while (*s == '[') s++;
        if (*s == 'L') while (*s && *s != ';') s++;
        args[na++] = cpu_arg(c, ai++); s++;
    }
    uint32_t obj; int32_t r = java_static(m->s1, m->s2, args, &obj);
    if (g_log) fprintf(stderr, "JAVA %s.%s%s -> %d/%#x\n", m->s1, m->s2, m->s3, r, obj);
    return kind == 1 ? obj : (uint32_t)r;     /* 1 = object */
}
static uint32_t j_CallStaticVoidMethod(Cpu *c) { return jni_call_static(c, 0); }
static uint32_t j_CallStaticIntMethod(Cpu *c) { return jni_call_static(c, 0); }
static uint32_t j_CallStaticObjectMethod(Cpu *c) { return jni_call_static(c, 1); }

/* JavaVM */
static uint32_t v_GetEnv(Cpu *c) { mem_w32(c->r[1], JNI_BASE); return 0; }

typedef struct JniImp { const char *name; hle_fn fn; } JniImp;
static const JniImp JNI_IMPS[] = {
    {"GetVersion", j_GetVersion}, {"FindClass", j_FindClass}, {"NewGlobalRef", j_NewGlobalRef},
    {"DeleteLocalRef", j_DeleteLocalRef}, {"DeleteGlobalRef", j_DeleteLocalRef}, {"ExceptionCheck", j_zero},
    {"ExceptionOccurred", j_zero}, {"ExceptionClear", j_zero}, {"ExceptionDescribe", j_zero},
    {"GetJavaVM", j_GetJavaVM}, {"GetStaticMethodID", j_GetStaticMethodID}, {"GetMethodID", j_GetStaticMethodID},
    {"GetArrayLength", j_GetArrayLength}, {"NewByteArray", j_NewByteArray},
    {"GetByteArrayElements", j_GetByteArrayElements}, {"GetPrimitiveArrayCritical", j_GetByteArrayElements},
    {"ReleaseByteArrayElements", j_ReleaseByteArrayElements}, {"ReleasePrimitiveArrayCritical", j_ReleaseByteArrayElements},
    {"GetByteArrayRegion", j_GetByteArrayRegion}, {"SetByteArrayRegion", j_SetByteArrayRegion},
    {"GetStringUTFChars", j_GetStringUTFChars}, {"ReleaseStringUTFChars", j_ReleaseStringUTFChars}, {"NewStringUTF", j_NewStringUTF},
    {"GetStringUTFLength", j_GetStringUTFLength},
    {"CallStaticVoidMethod", j_CallStaticVoidMethod}, {"CallStaticIntMethod", j_CallStaticIntMethod},
    {"CallStaticObjectMethod", j_CallStaticObjectMethod}, {"CallStaticBooleanMethod", j_CallStaticIntMethod},
};

/* ----------------------------------------------------------- ELF loader -- */
static struct { char *name; uint32_t addr, size; } *g_syms; static unsigned g_nsyms;
static uint32_t sym_addr(const char *name) {
    for (unsigned i = 0; i < g_nsyms; i++) if (!strcmp(g_syms[i].name, name)) return g_syms[i].addr;
    guest_fatal(9, "symbol %s not found in guest library", name);
}
static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t le16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int game_load(const uint8_t *so, size_t len, const GameHost *host) {
    g_host = *host;
    if (len < 0x34 || memcmp(so, "\x7f""ELF\1\1", 5) || le16(so + 0x12) != 40) { fprintf(stderr, "not an ARM ELF32\n"); return -1; }
    uint32_t phoff = le32(so + 0x1c); unsigned phnum = le16(so + 0x2c), phent = le16(so + 0x2a);
    uint32_t span = 0, dyn_off = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = so + phoff + i * phent; uint32_t type = le32(ph);
        if (type == 1) { uint32_t end = le32(ph + 8) + le32(ph + 20); if (end > span) span = end; }
        if (type == 2) dyn_off = le32(ph + 4);
    }
    g_span = span;
    mem_map(LIB_BASE, (span + 0xfff) & ~0xfffu);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *ph = so + phoff + i * phent;
        if (le32(ph) == 1) mem_write(LIB_BASE + le32(ph + 8), so + le32(ph + 4), le32(ph + 16));
    }
    mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE);
    mem_map(HEAP_BASE, HEAP_SIZE);
    mem_map(JNI_BASE, JNI_SIZE);

    uint32_t strtab = 0, symtab = 0, hash = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    for (const uint8_t *d = so + dyn_off;; d += 8) {
        uint32_t tag = le32(d), val = le32(d + 4); if (!tag) break;
        switch (tag) { case 5: strtab = val; break; case 6: symtab = val; break; case 4: hash = val; break;
                       case 17: rel = val; break; case 18: relsz = val; break; case 23: jmprel = val; break; case 2: pltrelsz = val; break; }
    }
    if (!hash) { fprintf(stderr, "library has no DT_HASH\n"); return -1; }
    unsigned nsyms = mem_r32(LIB_BASE + hash + 4);
    typedef struct { char *name; uint32_t value, shndx; } Sym;
    Sym *syms = (Sym *)calloc(nsyms, sizeof(Sym));
    g_syms = calloc(nsyms, sizeof *g_syms);
    for (unsigned i = 0; i < nsyms; i++) {
        uint32_t base = LIB_BASE + symtab + i * 16, nm = mem_r32(base), shndx = mem_r32(base + 12) >> 16;
        syms[i].name = gstr_dup(LIB_BASE + strtab + nm); syms[i].value = mem_r32(base + 4); syms[i].shndx = shndx;
        if (shndx && syms[i].name[0]) { g_syms[g_nsyms].name = syms[i].name; g_syms[g_nsyms].addr = LIB_BASE + syms[i].value; g_syms[g_nsyms].size = mem_r32(base + 8); g_nsyms++; }
    }

    uint32_t guard = 0;
    uint32_t tbls[2][2] = {{rel, relsz}, {jmprel, pltrelsz}};
    for (int t = 0; t < 2; t++) {
        for (uint32_t i = 0; i < tbls[t][1] / 8; i++) {
            uint32_t off = mem_r32(LIB_BASE + tbls[t][0] + i * 8), info = mem_r32(LIB_BASE + tbls[t][0] + i * 8 + 4);
            uint32_t type = info & 0xff, si = info >> 8, where = LIB_BASE + off;
            if (type == 23) { mem_w32(where, mem_r32(where) + LIB_BASE); continue; }
            if (type != 21 && type != 22 && type != 2) { fprintf(stderr, "unsupported reloc type %u\n", type); return -1; }
            Sym *s = &syms[si]; uint32_t target;
            if (s->shndx) target = LIB_BASE + s->value;
            else if (!strcmp(s->name, "__stack_chk_guard")) { if (!guard) { guard = gmalloc(4); mem_w32(guard, 0xdeadbeef); } target = guard; }
            else {
                static uint32_t und_addr[512]; static const char *und_name[512]; static int nund;
                target = 0;
                for (int k = 0; k < nund; k++) if (!strcmp(und_name[k], s->name)) target = und_addr[k];
                if (!target) {
                    hle_fn fn = NULL;
                    for (size_t k = 0; k < sizeof IMPORTS / sizeof *IMPORTS; k++) if (!strcmp(IMPORTS[k].name, s->name)) fn = IMPORTS[k].fn;
                    target = new_stub(s->name, fn); und_name[nund] = s->name; und_addr[nund++] = target;
                }
            }
            mem_w32(where, target + (type == 2 ? mem_r32(where) : 0));
        }
    }
    /* second stack-guard allocation (the reference harness does this after linking) */
    { uint32_t g2 = gmalloc(4); mem_w32(g2, 0xdeadbeef); }

    /* JNIEnv: JNI_BASE -> function table at +0x100; JavaVM at +0x700 -> table at +0x800 */
    uint32_t tbl = JNI_BASE + 0x100;
    for (unsigned i = 0; i < JNI_COUNT; i++) {
        hle_fn fn = NULL;
        for (size_t k = 0; k < sizeof JNI_IMPS / sizeof *JNI_IMPS; k++) if (!strcmp(JNI_IMPS[k].name, JNI_NAMES[i])) fn = JNI_IMPS[k].fn;
        mem_w32(tbl + 4 * i, new_stub(JNI_NAMES[i], fn));
    }
    mem_w32(JNI_BASE, tbl);
    static const char *const vm_names[] = {"reserved0", "reserved1", "reserved2", "DestroyJavaVM", "AttachCurrentThread",
                                          "DetachCurrentThread", "GetEnv", "AttachCurrentThreadAsDaemon"};
    for (int i = 0; i < 8; i++) mem_w32(JNI_BASE + 0x800 + 4 * (uint32_t)i, new_stub(vm_names[i], i == 6 || i == 4 || i == 7 ? v_GetEnv : (i == 5 ? j_zero : NULL)));
    mem_w32(JNI_BASE + 0x700, JNI_BASE + 0x800);
    free(syms);
    natives_install(g_native_mode);
    return 0;
}

/* ------------------------------------------------------- JNI entry points */
static uint32_t g_frame_handle;
static uint32_t call_jni(const char *sym, int nargs, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8) {
    uint32_t args[9] = {JNI_BASE, 1, a2, a3, a4, a5, a6, a7, a8};
    return cpu_call(&g_cpu, sym_addr(sym), nargs, args);
}
int game_init(void) {
    static const uint8_t uid[] = "0123456789", model[] = "Nexus";
    uint32_t a = new_bytes(uid, 10), b = new_bytes(model, 5);
    get_handle(a)->is_global = 1;
    get_handle(b)->is_global = 1;
    int r = (int)call_jni("Java_game_destiniaeng_GameThread_initGame", 9, a, 10, b, 5, GAME_W, GAME_H, 0);
    g_frame_handle = new_bytes(NULL, GAME_FB_BYTES);
    get_handle(g_frame_handle)->is_global = 1;
    g_h_checkpoint = g_h_n;
    return r;
}
/* Purchases disabled in offline mode: safely dismiss network waiting dialog */
static void proc_offline_buy(int idx) {
    (void)idx;
    call_jni("Java_game_destiniaeng_GameThread_procCommMsg", 4, 0, (uint32_t)-10, 0, 0, 0, 0, 0);
}

int game_run(void) {
    if (g_pending_comm) {
        g_pending_comm = 0;
        proc_offline_buy(g_pending_buy_idx);
    }
    int r = (int)call_jni("Java_game_destiniaeng_GameThread_jniRun", 4, g_frame_handle, g_frame_no++, 0, 0, 0, 0, 0);
    if (!g_host.clock_us) g_vclock_us += (uint32_t)((r == 0 ? 60 : (r < 1000 ? r : 1000)) * 1000);
    jni_local_frame_pop();
    return r;
}
void game_key(int code, int down) {
    uint32_t a = (uint32_t)code;
    cpu_call(&g_cpu, sym_addr(down ? "MMain_keyPressed" : "MMain_keyReleased"), 1, &a);
}
void game_touch(int x, int y, int type) { call_jni("Java_game_destiniaeng_GameThread_jniTouch", 5, (uint32_t)x, (uint32_t)y, (uint32_t)type, 0, 0, 0, 0); }
const uint8_t *game_frame(void) { return get_handle(g_frame_handle)->data; }
uint16_t *game_frame_ptr(void) { return (uint16_t *)get_handle(g_frame_handle)->data; }
extern int g_hotbar_page;
void game_set_hotbar_page(int page) { g_hotbar_page = page; }
int  game_get_hotbar_page(void) { return g_hotbar_page; }

void game_trigger_quick_slot(int slot_idx) {
    if (slot_idx < 0 || slot_idx >= 10) return;
    uint32_t a = (uint32_t)slot_idx;
    cpu_call(&g_cpu, sym_addr("_pressedQuickSlot"), 1, &a);
}

void game_set_trace(int count, int log_libc) { cpu_set_trace(count); g_log = log_libc; }
uint64_t game_icount(void) { return g_cpu.icount; }

/* ---- profiling: instructions retired per guest function ---- */

void game_profile_start(void) { g_prof_base = LIB_BASE; g_prof_span = g_span ? g_span : 0x60000; g_prof = (uint32_t *)calloc(g_prof_span / 2 + 1, 4); }
void game_profile_dump(int top) {
    if (!g_prof) return;
    typedef struct { const char *name; uint64_t n; } Row; Row *rows = (Row *)calloc(g_nsyms + 1, sizeof *rows); uint64_t total = 0, other = 0;
    for (uint32_t i = 0; i < g_prof_span / 2; i++) {
        if (!g_prof[i]) continue;
        uint32_t a = LIB_BASE + i * 2; total += g_prof[i]; int hit = 0;
        for (unsigned k = 0; k < g_nsyms; k++) if (a >= (g_syms[k].addr & ~1u) && a < (g_syms[k].addr & ~1u) + g_syms[k].size) { rows[k].name = g_syms[k].name; rows[k].n += g_prof[i]; hit = 1; break; }
        if (!hit) other += g_prof[i];
    }
    for (int t = 0; t < top; t++) {
        unsigned best = 0; for (unsigned k = 1; k < g_nsyms; k++) if (rows[k].n > rows[best].n) best = k;
        if (!rows[best].n) break;
        fprintf(stderr, "%6.2f%%  %12llu  %s\n", 100.0 * (double)rows[best].n / (double)total, (unsigned long long)rows[best].n, rows[best].name); rows[best].n = 0;
    }
    fprintf(stderr, "(unattributed %.2f%%, total %llu)\n", 100.0 * (double)other / (double)total, (unsigned long long)total);
}

uint32_t guest_sym(const char *name) { return sym_addr(name); }
uint32_t game_call(const char *sym, int nargs, const uint32_t *args) { return cpu_call(&g_cpu, sym_addr(sym), nargs, args); }   /* debug/test: call any exported guest function */
uint32_t guest_heap_end(void) { return g_heap_ptr; }
uint32_t guest_lib_span(void) { return g_span; }
