#!/usr/bin/env /usr/bin/python3
"""Host harness: runs the original libdestinia_jni.so (ARMv5, Android bionic) on x86 Linux via Unicorn.

Imports (libc, JNIEnv, JavaVM) are implemented in Python. Every call can be traced.
Usage: see run_frames.py
"""
import os, struct, sys, re
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LIB_BASE   = 0x10000000
STACK_TOP  = 0x7f100000
STACK_SIZE = 0x100000
HEAP_BASE  = 0x20000000
HEAP_SIZE  = 0x4000000
STUB_BASE  = 0x30000000
STUB_SIZE  = 0x10000
JNI_BASE   = 0x31000000
RET_MAGIC  = 0x3ffff000

JNI_NAMES = open(os.path.join(HERE, 'jni_names.txt')).read().split('\n')


def u32(b, o): return struct.unpack_from('<I', b, o)[0]


class Elf:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF' and d[4] == 1, 'need ELF32'
        (self.e_phoff, self.e_shoff) = struct.unpack_from('<II', d, 0x1c)
        self.e_phentsize, self.e_phnum = struct.unpack_from('<HH', d, 0x2a)
        self.segs = []
        self.dyn = {}
        for i in range(self.e_phnum):
            p_type, p_off, p_va, p_pa, p_fsz, p_msz, p_flags, p_al = struct.unpack_from('<8I', d, self.e_phoff + i * self.e_phentsize)
            if p_type == 1: self.segs.append((p_va, p_off, p_fsz, p_msz, p_flags))
            if p_type == 2:
                o = p_off
                while True:
                    tag, val = struct.unpack_from('<II', d, o); o += 8
                    if tag == 0: break
                    self.dyn.setdefault(tag, []).append(val)
        self.size = max(va + msz for va, _, _, msz, _ in self.segs)

    def v2o(self, va):
        for sva, off, fsz, msz, _ in self.segs:
            if sva <= va < sva + fsz: return off + va - sva
        raise ValueError(hex(va))

    def dynsyms(self):
        strtab = self.dyn[5][0]; symtab = self.dyn[6][0]
        # number of symbols: from hash table (DT_HASH=4) else gnu hash; assume DT_HASH
        nsyms = u32(self.d, self.v2o(self.dyn[4][0]) + 4)
        out = []
        for i in range(nsyms):
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack_from('<IIIBBH', self.d, self.v2o(symtab) + i * 16)
            o = self.v2o(strtab) + st_name
            name = self.d[o:self.d.index(b'\0', o)].decode()
            out.append((name, st_value, st_size, st_info & 15, st_shndx))
        return out

    def relocs(self):
        res = []
        for tagv, szt in ((17, 18), (23, 2)):  # DT_REL/DT_RELSZ, DT_JMPREL/DT_PLTRELSZ
            if tagv in self.dyn:
                base = self.v2o(self.dyn[tagv][0])
                sz = self.dyn[szt][0]
                for i in range(sz // 8):
                    off, info = struct.unpack_from('<II', self.d, base + i * 8)
                    res.append((off, info & 0xff, info >> 8))
        return res


class Emu:
    def __init__(self, so_path, assets_dir, save_dir, trace=False):
        self.trace = trace
        self.assets_dir, self.save_dir = assets_dir, save_dir
        os.makedirs(save_dir, exist_ok=True)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc = self.uc
        self.elf = Elf(so_path)
        size = (self.elf.size + 0xfff) & ~0xfff
        uc.mem_map(LIB_BASE, size)
        for va, off, fsz, msz, fl in self.elf.segs:
            uc.mem_write(LIB_BASE + va, self.elf.d[off:off + fsz])
        uc.mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE)
        uc.mem_map(HEAP_BASE, HEAP_SIZE)
        uc.mem_map(STUB_BASE, STUB_SIZE)
        uc.mem_map(JNI_BASE, 0x10000)
        uc.mem_map(RET_MAGIC & ~0xfff, 0x1000)
        self.heap_ptr = HEAP_BASE + 0x1000
        self.alloc_sizes = {}
        self.free_blocks = {}
        self.stubs = []           # index -> name
        self.stub_fn = {}
        self.syms = {}            # name -> addr (defined)
        self.sym_by_addr = []
        self.unimpl = set()
        self.log_lines = []
        self.handles = {1: ('null',)}
        self.next_handle = 0x1000
        self.files = {}; self.next_fd = 0x100
        self.sound_events = []
        self.vclock_us = 1_000_000   # deterministic virtual clock (microseconds)
        self._link()
        self._setup_jni()
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=STUB_BASE, end=STUB_BASE + STUB_SIZE)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)
        # stack guard value
        self.guard = self.malloc(4); uc.mem_write(self.guard, struct.pack('<I', 0xdeadbeef))

    # ---------- memory helpers ----------
    def malloc(self, n):
        n = max(16, (n + 15) & ~15)
        fl = self.free_blocks
        if n in fl and fl[n]:
            p = fl[n].pop(); self.alloc_sizes[p] = n; return p
        p = self.heap_ptr; self.heap_ptr += n
        if self.heap_ptr > HEAP_BASE + HEAP_SIZE: raise MemoryError('heap exhausted')
        self.alloc_sizes[p] = n
        return p

    def free(self, p):
        n = self.alloc_sizes.pop(p, None)
        if n is not None: self.free_blocks.setdefault(n, []).append(p)

    def rd(self, a, n): return bytes(self.uc.mem_read(a, n))
    def wr(self, a, b): self.uc.mem_write(a, bytes(b))
    def r32(self, a): return struct.unpack('<I', self.rd(a, 4))[0]
    def w32(self, a, v): self.wr(a, struct.pack('<I', v & 0xffffffff))
    def cstr(self, a):
        out = bytearray()
        while True:
            c = self.rd(a, 1)[0]
            if c == 0: return bytes(out)
            out.append(c); a += 1
    def reg(self, r): return self.uc.reg_read(r)
    def setreg(self, r, v): self.uc.reg_write(r, v & 0xffffffff)
    def arg(self, i):
        if i < 4: return self.reg(UC_ARM_REG_R0 + i)
        return self.r32(self.reg(UC_ARM_REG_SP) + 4 * (i - 4))
    def sarg(self, i):
        v = self.arg(i); return v - (1 << 32) if v & 0x80000000 else v

    # ---------- linking ----------
    def _stub(self, name, fn=None):
        idx = len(self.stubs); self.stubs.append(name)
        addr = STUB_BASE + idx * 8
        self.wr(addr, struct.pack('<II', 0xe12fff1e, 0))  # bx lr
        if fn: self.stub_fn[addr] = fn
        return addr

    def _link(self):
        e = self.elf
        syms = e.dynsyms()
        self.symtab = syms
        for name, val, size, typ, shndx in syms:
            if shndx != 0 and name:
                self.syms[name] = LIB_BASE + val
                if typ == 2: self.sym_by_addr.append((LIB_BASE + (val & ~1), size, name))
        self.sym_by_addr.sort()
        und = {}
        for off, rtype, symidx in e.relocs():
            where = LIB_BASE + off
            if rtype == 23:  # RELATIVE
                self.w32(where, self.r32(where) + LIB_BASE); continue
            name, val, size, typ, shndx = syms[symidx]
            if rtype in (21, 22, 2):  # GLOB_DAT / JUMP_SLOT / ABS32
                if shndx != 0:
                    target = LIB_BASE + val
                elif name == '__stack_chk_guard':
                    target = und.setdefault(name, self.malloc(4)); self.w32(target, 0xdeadbeef)
                else:
                    target = und.get(name) or und.setdefault(name, self._stub(name, getattr(self, 'c_' + name, None)))
                add = self.r32(where) if rtype == 2 else 0
                self.w32(where, target + add)
            else:
                raise NotImplementedError(f'reloc type {rtype} for {name}')
        self.und = und

    # ---------- JNI ----------
    def _setup_jni(self):
        # function table with 236+ entries
        tbl = JNI_BASE + 0x100
        for i, n in enumerate(JNI_NAMES):
            a = self._stub('JNI:' + n, getattr(self, 'j_' + n, None))
            self.w32(tbl + 4 * i, a)
        self.env = JNI_BASE; self.w32(self.env, tbl)
        vtbl = JNI_BASE + 0x800
        for i, n in enumerate(['reserved0', 'reserved1', 'reserved2', 'DestroyJavaVM', 'AttachCurrentThread',
                               'DetachCurrentThread', 'GetEnv', 'AttachCurrentThreadAsDaemon']):
            self.w32(vtbl + 4 * i, self._stub('JVM:' + n, getattr(self, 'v_' + n, None)))
        self.vm = JNI_BASE + 0x700; self.w32(self.vm, vtbl)

    def newh(self, obj):
        h = self.next_handle; self.next_handle += 4; self.handles[h] = obj; return h

    # ---------- execution ----------
    def _fn_at(self, pc):
        for a, s, n in self.sym_by_addr:
            if a <= pc < a + max(s, 2): return f'{n}+{pc - a:#x}'
        return hex(pc)

    def _on_code(self, uc, addr, size, ud):
        name = self.stubs[(addr - STUB_BASE) // 8]
        fn = self.stub_fn.get(addr)
        if fn is None:
            if name not in self.unimpl:
                self.unimpl.add(name)
                self.log(f'!! unimplemented {name} (args r0={self.arg(0):#x} r1={self.arg(1):#x} r2={self.arg(2):#x} r3={self.arg(3):#x}) from {self._fn_at(self.reg(UC_ARM_REG_LR))}')
            self.setreg(UC_ARM_REG_R0, 0); return
        r = fn()
        if r is not None:
            self.setreg(UC_ARM_REG_R0, r & 0xffffffff)
            if isinstance(r, int) and r >> 32: self.setreg(UC_ARM_REG_R1, r >> 32)

    def trace_funcs(self, names):
        """Print entry (name, r0-r3) of the given internal functions on every call."""
        want = {self.syms[n] & ~1: n for n in names}
        def cb(uc, addr, size, ud):
            self.log(f'>> {want[addr]}({self.arg(0):#x}, {self.arg(1):#x}, {self.arg(2):#x}, {self.arg(3):#x})')
        for a in want:
            self.uc.hook_add(UC_HOOK_CODE, cb, begin=a, end=a)

    def _on_unmapped(self, uc, access, addr, size, value, ud):
        pc = self.reg(UC_ARM_REG_PC); lr = self.reg(UC_ARM_REG_LR)
        self.log(f'!! unmapped access {addr:#x} size {size} access {access} pc={self._fn_at(pc)} lr={self._fn_at(lr)}')
        return False

    def log(self, s):
        self.log_lines.append(s)
        if self.trace: print(s, file=sys.stderr)

    def call(self, addr, *args, max_insns=200_000_000):
        uc = self.uc
        sp = STACK_TOP - 0x100
        stack_args = args[4:]
        sp -= 4 * len(stack_args); sp &= ~7
        for i, v in enumerate(stack_args): self.w32(sp + 4 * i, v)
        for i, v in enumerate(args[:4]): uc.reg_write(UC_ARM_REG_R0 + i, v & 0xffffffff)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET_MAGIC)
        try:
            uc.emu_start(addr, RET_MAGIC, count=max_insns)
        except UcError as ex:
            pc = self.reg(UC_ARM_REG_PC)
            raise RuntimeError(f'emulation error {ex} at pc={self._fn_at(pc)} lr={self._fn_at(self.reg(UC_ARM_REG_LR))}') from ex
        return uc.reg_read(UC_ARM_REG_R0)

    # ======================== libc ========================
    def c___stack_chk_fail(self): raise RuntimeError('stack smashing detected')
    def c___aeabi_unwind_cpp_pr0(self): return 0
    def c___aeabi_unwind_cpp_pr1(self): return 0
    def c___aeabi_idiv(self):
        a, b = self.sarg(0), self.sarg(1)
        return 0 if b == 0 else int(a / b) if abs(a) < 1 << 52 else a // b
    def c___aeabi_uidiv(self):
        a, b = self.arg(0), self.arg(1); return 0 if b == 0 else a // b
    def c___aeabi_uidivmod(self):
        a, b = self.arg(0), self.arg(1)
        q, r = (0, 0) if b == 0 else divmod(a, b); self.setreg(UC_ARM_REG_R1, r); return q
    def c___aeabi_idivmod(self):
        a, b = self.sarg(0), self.sarg(1)
        if b == 0: q = r = 0
        else:
            q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1); r = a - q * b
        self.setreg(UC_ARM_REG_R1, r); return q
    def c___aeabi_ldivmod(self):
        def s64(lo, hi):
            v = lo | (hi << 32); return v - (1 << 64) if v >> 63 else v
        a, b = s64(self.arg(0), self.arg(1)), s64(self.arg(2), self.arg(3))
        if b == 0: q = r = 0
        else:
            q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1); r = a - q * b
        q &= (1 << 64) - 1; r &= (1 << 64) - 1
        self.setreg(UC_ARM_REG_R1, q >> 32); self.setreg(UC_ARM_REG_R2, r & 0xffffffff); self.setreg(UC_ARM_REG_R3, r >> 32)
        return q & 0xffffffff
    def c___aeabi_lmul(self):
        a = self.arg(0) | (self.arg(1) << 32); b = self.arg(2) | (self.arg(3) << 32)
        p = (a * b) & ((1 << 64) - 1); self.setreg(UC_ARM_REG_R1, p >> 32); return p & 0xffffffff

    def c_malloc(self): return self.malloc(self.arg(0))
    def c_free(self): self.free(self.arg(0)); return 0
    def c_memcpy(self):
        d, s, n = self.arg(0), self.arg(1), self.arg(2)
        if n: self.wr(d, self.rd(s, n))
        return d
    c_memmove = c_memcpy
    def c_memset(self):
        d, v, n = self.arg(0), self.arg(1) & 0xff, self.arg(2)
        if n: self.wr(d, bytes([v]) * n)
        return d
    def c_strlen(self): return len(self.cstr(self.arg(0)))
    def c_strcpy(self):
        d, s = self.arg(0), self.arg(1); self.wr(d, self.cstr(s) + b'\0'); return d
    def c_strncmp(self):
        a, b, n = self.cstr(self.arg(0))[:self.arg(2)], self.cstr(self.arg(1))[:self.arg(2)], self.arg(2)
        return (a > b) - (a < b)
    def c_strstr(self):
        h, n = self.cstr(self.arg(0)), self.cstr(self.arg(1)); i = h.find(n)
        return 0 if i < 0 else self.arg(0) + i
    def c_clock(self):
        return self.vclock_us & 0x7fffffff
    def c___android_log_print(self):
        tag = self.cstr(self.arg(1)).decode('latin1'); msg = self._fmt(self.cstr(self.arg(2)).decode('latin1'), 3)
        self.log(f'[log:{tag}] {msg}'); return 0
    def c_sprintf(self):
        s = self._fmt(self.cstr(self.arg(1)).decode('latin1'), 2)
        self.wr(self.arg(0), s.encode('latin1') + b'\0'); return len(s)

    def _fmt(self, fmt, argi):
        out = []; i = 0
        for m in re.finditer(r'%([-+ 0#]*)(\d*)(?:\.(\d+))?(l|h|hh|ll)?([diuxXcsp%])', fmt):
            out.append(fmt[i:m.start()]); i = m.end()
            fl, w, pr, _, cv = m.groups()
            if cv == '%': out.append('%'); continue
            v = self.arg(argi); argi += 1
            if cv in 'di': v = v - (1 << 32) if v & 0x80000000 else v
            if cv == 's': v = self.cstr(v).decode('latin1'); spec = '%' + fl + w + ('.' + pr if pr else '') + 's'
            elif cv == 'c': v = chr(v & 0xff); spec = '%' + fl + w + 'c'
            elif cv == 'p': v = hex(v); spec = '%' + fl + w + 's'
            else: spec = '%' + fl + w + ('.' + pr if pr else '') + ('d' if cv in 'diu' else cv)
            out.append(spec % v)
        out.append(fmt[i:]); return ''.join(out)

    # file I/O: native paths are mapped to save_dir
    def _hostpath(self, p):
        p = p.decode('latin1').replace('\\', '/')
        return os.path.join(self.save_dir, os.path.basename(p))
    def c_fopen(self):
        p = self._hostpath(self.cstr(self.arg(0))); mode = self.cstr(self.arg(1)).decode()
        try: f = open(p, mode if 'b' in mode else mode + 'b')
        except OSError: self.log(f'fopen {p} {mode} -> NULL'); return 0
        fd = self.next_fd; self.next_fd += 4; self.files[fd] = f; self.log(f'fopen {p} {mode} -> {fd:#x}'); return fd
    def c_fclose(self): f = self.files.pop(self.arg(0), None); f and f.close(); return 0
    def c_fread(self):
        buf, sz, n, fd = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        data = self.files[fd].read(sz * n); self.wr(buf, data); return len(data) // sz if sz else 0
    def c_fwrite(self):
        buf, sz, n, fd = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        self.files[fd].write(self.rd(buf, sz * n)); return n
    def c_fseek(self): self.files[self.arg(0)].seek(self.sarg(1), self.arg(2)); return 0
    def c_ftell(self): return self.files[self.arg(0)].tell()
    def c_remove(self):
        try: os.remove(self._hostpath(self.cstr(self.arg(0)))); return 0
        except OSError: return 0xffffffff

    # ======================== JavaVM ========================
    def v_GetEnv(self): self.w32(self.arg(1), self.env); return 0
    def v_AttachCurrentThread(self): self.w32(self.arg(1), self.env); return 0
    def v_DetachCurrentThread(self): return 0

    # ======================== JNIEnv ========================
    def j_GetVersion(self): return 0x00010006
    def j_FindClass(self):
        name = self.cstr(self.arg(1)).decode(); self.log(f'JNI FindClass {name}'); return self.newh(('class', name))
    def j_NewGlobalRef(self): return self.arg(1)
    def j_DeleteLocalRef(self): return 0
    def j_DeleteGlobalRef(self): return 0
    def j_ExceptionCheck(self): return 0
    def j_ExceptionOccurred(self): return 0
    def j_ExceptionClear(self): return 0
    def j_ExceptionDescribe(self): return 0
    def j_GetJavaVM(self): self.w32(self.arg(1), self.vm); return 0
    def j_GetStaticMethodID(self):
        cls = self.handles[self.arg(1)]; n = self.cstr(self.arg(2)).decode(); sig = self.cstr(self.arg(3)).decode()
        self.log(f'JNI GetStaticMethodID {cls[1]}.{n}{sig}'); return self.newh(('method', cls[1], n, sig))
    j_GetMethodID = j_GetStaticMethodID
    def j_GetArrayLength(self): return len(self.handles[self.arg(1)][1])
    def j_NewByteArray(self): return self.newh(('bytes', bytearray(self.arg(1))))
    def j_GetByteArrayElements(self):
        arr = self.handles[self.arg(1)][1]; p = self.malloc(len(arr) + 4); self.wr(p, arr)
        self.handles.setdefault('elems', {})[p] = self.arg(1)
        if self.arg(2): self.w32(self.arg(2), 1)
        return p
    j_GetPrimitiveArrayCritical = j_GetByteArrayElements
    def j_ReleaseByteArrayElements(self):
        arr = self.handles[self.arg(1)][1]; p, mode = self.arg(2), self.arg(3)
        if mode != 2: arr[:] = self.rd(p, len(arr))
        if mode != 1: self.free(p)
        return 0
    j_ReleasePrimitiveArrayCritical = j_ReleaseByteArrayElements
    def j_GetByteArrayRegion(self):
        arr = self.handles[self.arg(1)][1]; s, n, dst = self.arg(2), self.arg(3), self.arg(4)
        self.wr(dst, arr[s:s + n]); return 0
    def j_SetByteArrayRegion(self):
        arr = self.handles[self.arg(1)][1]; s, n, src = self.arg(2), self.arg(3), self.arg(4)
        arr[s:s + n] = self.rd(src, n); return 0
    def j_GetStringUTFChars(self):
        s = self.handles[self.arg(1)][1].encode() + b'\0'; p = self.malloc(len(s)); self.wr(p, s); return p
    def j_ReleaseStringUTFChars(self): return 0
    def j_NewStringUTF(self): return self.newh(('str', self.cstr(self.arg(1)).decode('latin1')))
    def j_GetStringUTFLength(self): return len(self.handles[self.arg(1)][1].encode())

    # --- CallStatic*Method(env, cls, mid, ...) : variadic args from r3 / stack
    def _static_call(self):
        m = self.handles[self.arg(2)]; _, cls, name, sig = m
        args = []; ai = 3
        for t in re.match(r'\((.*)\)', sig).group(1).replace('[B', 'A').replace('[', 'A'):
            if t in 'JD':
                ai += ai & 1 if False else 0
            args.append(self.arg(ai)); ai += 1
        return cls, name, sig, args

    def _call_java(self, kind):
        cls, name, sig, args = self._static_call()
        def barr(h): return bytes(self.handles[h][1])
        ret = self.java(cls, name, sig, args, barr)
        self.log(f'JAVA {cls}.{name}{sig} args={[hex(a) for a in args]} -> {ret!r}')
        if kind == 'Object':
            return 0 if ret is None else self.newh(('bytes', bytearray(ret)))
        if kind == 'Void': return 0
        return ret or 0
    def j_CallStaticVoidMethod(self): return self._call_java('Void')
    def j_CallStaticIntMethod(self): return self._call_java('Int')
    def j_CallStaticObjectMethod(self): return self._call_java('Object')
    j_CallStaticBooleanMethod = j_CallStaticIntMethod

    # --- Java side stubs (what Android/Java provided)
    def java(self, cls, name, sig, args, barr):
        if name == 'getAssetResSize':
            p = os.path.join(self.assets_dir, barr(args[0]).decode()); return os.path.getsize(p) if os.path.exists(p) else 0
        if name == 'getAssetRes':
            p = os.path.join(self.assets_dir, barr(args[0]).decode())
            return open(p, 'rb').read() if os.path.exists(p) else None
        if name in ('playSound', 'setSoundVolume', 'playVib'):
            self.sound_events.append((name, args)); return None
        if name in ('logEvent', 'gotoURL'): return None
        return 0


# JNI entry points
def jarr(e, data): return e.newh(('bytes', bytearray(data)))

class Game:
    def __init__(self, so, assets, save, trace=False):
        self.e = Emu(so, assets, save, trace)
        self.frame_no = 0
    def sym(self, n): return self.e.syms[n]
    def init(self, uid=b'0123456789', model=b'Nexus', w=400, h=240, korean=0):
        e = self.e
        a, b = jarr(e, uid), jarr(e, model)
        return e.call(self.sym('Java_game_destiniaeng_GameThread_initGame'), e.env, 1, a, len(uid), b, len(model), w, h, korean)
    def run(self, buf_h):
        e = self.e
        r = e.call(self.sym('Java_game_destiniaeng_GameThread_jniRun'), e.env, 1, buf_h, self.frame_no)
        self.frame_no += 1
        self.e.vclock_us += (60 if r == 0 else min(r, 1000)) * 1000   # advance by the sleep the Java loop would have done
        return r
    def touch(self, x, y, t):
        e = self.e; return e.call(self.sym('Java_game_destiniaeng_GameThread_jniTouch'), e.env, 1, x, y, t)
    def banner(self):
        e = self.e; return e.call(self.sym('Java_game_destiniaeng_GameThread_bannerShow'), e.env, 1)

    def key(self, code, down=True):
        """Inject a WIPI key directly (what the gamepad mapping will use on Switch)."""
        e = self.e
        return e.call(self.sym('MMain_keyPressed' if down else 'MMain_keyReleased'), code)

    def warp(self, area, sub):
        """Warp the hero to map area_sub with the game's own warp sequence (as checkWarpData does) -- test tool."""
        e = self.e; L = 0x10000000; c = lambda n, *a: e.call(self.sym(n), *a)
        c('setHeroPos', (10 * 0x100000 + 0x80000) >> 16, (10 * 0x100000 + 0x80000) >> 16, 1)
        e.wr(e.r32(L + 0x4439c) + 0xc2, struct.pack('<HH', area, sub))
        c('Map_freeData'); c('Map_freeTile'); c('Map_loadData', area, sub, 1); c('Map_loadTile', 0); c('setHeroAction', 2, 0xffffffff)
        e.wr(e.r32(L + 0x44278) + 0x489a, b'\xff\xff'); c('setMapSound')
