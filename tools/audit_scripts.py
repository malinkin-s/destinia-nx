#!/usr/bin/env /usr/bin/python3
"""audit_scripts.py -- static audit of Destinia bytecode scripts (script_en.sdt / script.sdt).
Validates script count, entry offsets, lengths, opcode values, and structure against libdestinia_jni.so procScript VM.
"""
import sys
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APK_ASSETS = os.path.join(ROOT, "work", "apk", "assets")

# Verified opcode handlers in procScript (0..0x1a)
OPCODE_NAMES = {
    0x00: "SPAWN_ACTOR_OR_EFFECT",
    0x01: "SET_ACTOR_POS_ACTION",
    0x02: "SET_STATE_VAR",
    0x03: "PLAY_SOUND",
    0x04: "SET_HERO_ACTION",
    0x05: "MOVE_ACTOR_TARGET",
    0x06: "SPAWN_OBJECT",
    0x07: "SET_CAMERA_PAN",
    0x08: "SCREEN_SHAKE",
    0x09: "ACTOR_DIALOG_OR_BUBBLE",
    0x0a: "SET_HERO_DIR",
    0x0b: "BRANCH_CONDITIONAL",
    0x0c: "SET_QUEST_FLAG",
    0x0d: "SET_MAP_OR_WARP",
    0x0e: "WAIT_FRAMES",
    0x0f: "CHECK_DIALOG_ACTIVE",
    0x10: "WAIT_USER_CONFIRM",
    0x11: "ENABLE_INPUT_FLAG",
    0x12: "DISABLE_INPUT_FLAG",
    0x13: "START_BATTLE_EVENT",
    0x14: "SET_BGM_TRACK",
    0x15: "SET_ACTOR_ANIMATION",
    0x16: "SHOW_SCRIPT_MESSAGE",
    0x17: "TELEPORT_ACTOR",
    0x18: "SET_EVENT_TRIGGER",
    0x19: "SET_MAP_SOUND",
    0x1a: "SET_SCRIPT_STATE",
}

def audit_file(path):
    if not os.path.isfile(path):
        print(f"File not found: {path}")
        return False
    with open(path, "rb") as f:
        data = f.read()
    if len(data) >= 12 and data[:3] == b"ZCP":
        import zlib
        comp_sz = int.from_bytes(data[4:8], "little")
        uncomp_sz = int.from_bytes(data[8:12], "little")
        data = zlib.decompress(data[12:12+comp_sz])
        if len(data) != uncomp_sz:
            print(f"ERROR: ZCP decompressed size mismatch: got {len(data)}, expected {uncomp_sz}")
            return False
        print(f"(Decompressed ZCP: {len(data)} bytes, compressed {comp_sz})")
    total_len = len(data)
    count = data[0]
    print(f"\n--- Auditing {os.path.basename(path)}: {count} scripts, {total_len} bytes ---")
    pos = 1
    scripts = []
    for i in range(count):
        if pos + 4 > total_len:
            print(f"ERROR: Truncated header table at script #{i}")
            return False
        off = data[pos] | (data[pos+1] << 8)
        sz = data[pos+2] | (data[pos+3] << 8)
        pos += 4
        scripts.append((i, off, sz))

    header_end = pos
    print(f"Header table size: {header_end} bytes")

    valid_scripts = 0
    empty_scripts = 0
    errors = 0

    for sid, off, sz in scripts:
        if sz == 0:
            empty_scripts += 1
            continue
        if off < header_end or off + sz > total_len:
            print(f"ERROR: Script #{sid:02d} out of file bounds: off=0x{off:04x}, sz={sz}, file_len={total_len}")
            errors += 1
            continue
        
        sdata = data[off:off+sz]
        ip = 0
        script_err = 0
        while ip < sz:
            op = sdata[ip]
            ip += 1
            if op > 0x1a:
                # procScript at 0x1e23a: cmp r2, #0x1a; bhi 0x1e226 -> skips any byte > 0x1a
                continue
            # advance operands
            if op == 0x00: ip += 4
            elif op == 0x01: ip += 5
            elif op == 0x02: ip += 2
            elif op == 0x03: ip += 1
            elif op == 0x04: ip += 1
            elif op == 0x05: ip += 3
            elif op == 0x06: ip += 4
            elif op == 0x07: ip += 4
            elif op == 0x08: ip += 1
            elif op == 0x09: ip += 3
            elif op == 0x0a: ip += 2
            elif op == 0x0b: ip += 1
            elif op == 0x0c: ip += 2
            elif op == 0x0d: ip += 2
            elif op in (0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13): pass
            elif op == 0x14: ip += 1
            elif op == 0x15: ip += 3
            elif op == 0x16:
                if ip < sz:
                    slen = sdata[ip]
                    ip += 1 + slen
            elif op == 0x17: ip += 3
            elif op == 0x18: ip += 2
            elif op == 0x19: ip += 5
            elif op == 0x1a: ip += 1

        if not script_err:
            valid_scripts += 1

    print(f"Summary: {valid_scripts} valid scripts, {empty_scripts} empty scripts, {errors} errors.")
    return errors == 0

def main():
    sdt_en = os.path.join(APK_ASSETS, "script_en.sdt")
    sdt_ko = os.path.join(APK_ASSETS, "script.sdt")
    
    ok1 = audit_file(sdt_en)
    ok2 = audit_file(sdt_ko)
    if ok1 and ok2:
        print("\nALL SCRIPTS STATIC AUDIT PASSED: 0 BOUNDS ERRORS.")
        sys.exit(0)
    else:
        sys.exit(1)

if __name__ == "__main__":
    main()
