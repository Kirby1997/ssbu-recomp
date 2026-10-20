#!/usr/bin/env python3
"""Lifter checks for memory ordering on an ARM host (PORT_AUDIT Phase 2). No game binary is used or needed.

    python3 test_lifter_atomics.py                 decoder test: synthetic instruction words -> expected C,
                                                   for tools/lifter/recomp.py and tools/nro/recomp_nro.py
    python3 test_lifter_atomics.py --gen OUT.h     lift the litmus guest programs below into OUT.h
                                                   (included by litmus.c; done by run_frame.sh)
    python3 test_lifter_atomics.py --asm           print the litmus guest programs as assembly (run_frame.sh
                                                   assembles them and checks the instruction words below)

The litmus programs are guest AArch64 code; their words were produced by GNU as on the target and are checked
against it again by run_frame.sh, so the C in OUT.h is exactly what the lifter emits for that guest code.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
os.environ['RECOMP_ELF'] = os.path.join(HERE, 'no-game-binary-here.elf')   # never read a game binary
os.environ['RECOMP_NO_IMAGE'] = '1'                                         # recomp.py imports without one
os.environ.pop('RECOMP_PATCHES', None)
os.environ.pop('RECOMP_NRO', None)
sys.path[:0] = [os.path.join(ROOT, 'tools', 'lifter'), os.path.join(ROOT, 'tools', 'nro')]
# C-spike: upstream's recomp.py reads <RECOMP_BIN>/main.elf at import and has no RECOMP_NO_IMAGE. Give it a synthetic
# 4 KiB+ "ELF" with only what _layout() reads (one PT_LOAD, MOD0, a dynamic section): no game code, no game data.
if not os.environ.get('RECOMP_BIN'):
    import struct, tempfile
    d = tempfile.mkdtemp(prefix='fake-main-')
    img = bytearray(0x1100)
    struct.pack_into('<Q', img, 0x20, 0x40); struct.pack_into('<H', img, 0x38, 1)
    struct.pack_into('<I', img, 0x40, 1); struct.pack_into('<Q', img, 0x40 + 32, 0x100)
    struct.pack_into('<I', img, 0x1004, 0x10); img[0x1010:0x1014] = b'MOD0'; struct.pack_into('<i', img, 0x1014, 0x10)
    off = 0x1020
    for tag in (7, 8, 23, 2, 6, 5, 0):
        struct.pack_into('<qQ', img, off, tag, 0); off += 16
    open(os.path.join(d, 'main.elf'), 'wb').write(img)
    os.environ['RECOMP_BIN'] = d
import recomp          # noqa: E402
import recomp_nro      # noqa: E402

PC = 0x7100001000

# (assembly, word, C the lifter must emit)  -- words from GNU as on the Frame
CASES = [
    ('dmb ish',             0xd5033bbf, 'RT_FENCE_FULL();'),
    ('dmb ishld',           0xd50339bf, 'RT_FENCE_ACQ();'),
    ('dmb ishst',           0xd5033abf, 'RT_FENCE_ST();'),
    ('dmb sy',              0xd5033fbf, 'RT_FENCE_FULL();'),
    ('dmb ld',              0xd5033dbf, 'RT_FENCE_ACQ();'),
    ('dmb st',              0xd5033ebf, 'RT_FENCE_ST();'),
    ('dmb osh',             0xd50333bf, 'RT_FENCE_FULL();'),
    ('dmb oshst',           0xd50332bf, 'RT_FENCE_ST();'),
    ('dmb nsh',             0xd50337bf, 'RT_FENCE_FULL();'),
    ('dmb nshld',           0xd50335bf, 'RT_FENCE_ACQ();'),
    ('dmb #0 (reserved)',   0xd50330bf, 'RT_FENCE_FULL();'),
    ('dsb ish',             0xd5033b9f, 'RT_FENCE_FULL();'),
    ('dsb sy',              0xd5033f9f, 'RT_FENCE_FULL();'),
    ('dsb ishst',           0xd5033a9f, 'RT_FENCE_FULL();'),
    ('isb',                 0xd5033fdf, 'RT_COMPILER_BARRIER();'),
    ('clrex',               0xd5033f5f, 'c->ex_addr = ~0ull;'),
    ('ldar x0, [x1]',       0xc8dffc20, 'c->x[0] = (uint64_t)(rdacq64(c->x[1]));'),
    ('ldar w0, [x1]',       0x88dffc20, 'c->x[0] = (uint64_t)(rdacq32(c->x[1]));'),
    ('ldarb w0, [x1]',      0x08dffc20, 'c->x[0] = (uint64_t)(rdacq8(c->x[1]));'),
    ('ldarh w0, [x1]',      0x48dffc20, 'c->x[0] = (uint64_t)(rdacq16(c->x[1]));'),
    ('ldar x0, [sp]',       0xc8dfffe0, 'c->x[0] = (uint64_t)(rdacq64(c->x[31]));'),
    ('stlr x0, [x1]',       0xc89ffc20, 'wrrel64(c->x[1], c->x[0]);'),
    ('stlr w0, [x1]',       0x889ffc20, 'wrrel32(c->x[1], (uint32_t)c->x[0]);'),
    ('stlrb w0, [x1]',      0x089ffc20, 'wrrel8(c->x[1], (uint32_t)c->x[0]);'),
    ('stlrh w0, [x1]',      0x489ffc20, 'wrrel16(c->x[1], (uint32_t)c->x[0]);'),
    ('stlr wzr, [x0]',      0x889ffc1f, 'wrrel32(c->x[0], 0U);'),
    ('ldaxr x0, [x1]',      0xc85ffc20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rdacq64(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldaxr w0, [x1]',      0x885ffc20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rdacq32(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxr x0, [x1]',       0xc85f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd64(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxr w0, [x1]',       0x885f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd32(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('ldxrb w0, [x1]',      0x085f7c20, '{ uint64_t a = c->x[1]; c->ex_addr = a; c->ex_val = rd8(a); c->x[0] = (uint64_t)(c->ex_val); }'),
    ('stxr w2, x0, [x1]',   0xc8027c20, 'c->x[2] = (uint32_t)(stx64(c, c->x[1], c->x[0]));'),
    ('stxr w2, w0, [x1]',   0x88027c20, 'c->x[2] = (uint32_t)(stx32(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxr w2, x0, [x1]',  0xc802fc20, 'c->x[2] = (uint32_t)(stlx64(c, c->x[1], c->x[0]));'),
    ('stlxr w2, w0, [x1]',  0x8802fc20, 'c->x[2] = (uint32_t)(stlx32(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxrh w2, w0, [x1]', 0x4802fc20, 'c->x[2] = (uint32_t)(stlx16(c, c->x[1], (uint32_t)c->x[0]));'),
    ('stlxr w2, wzr, [x0]', 0x8802fc1f, 'c->x[2] = (uint32_t)(stlx32(c, c->x[0], 0U));'),
    ('prfm pldl1keep, [x0]',       0xf9800000, ';'),       # PRFM stays a no-op
    ('prfm pldl1keep, [x0, x1]',   0xf8a16800, ';'),
    ('prfum pldl1keep, [x0, #1]',  0xf8801000, ';'),
]

# still not translated (would need the guest binary to know whether they occur): pairs and ARMv8.1 atomics
UNIMPL = [
    ('ldxp x0, x1, [x2]',      0xc87f0440),
    ('stxp w3, x0, x1, [x2]',  0xc8230440),
    ('casal x0, x1, [x2]',     0xc8e0fc41),
    ('ldapr x0, [x1]',         0xf8bfc020),
    ('ldadd x0, x1, [x2]',     0xf8200041),
    ('ldadda x0, x1, [x2]',    0xf8a00041),   # was lifted as PRFM (a no-op) before
    ('swpal x0, x1, [x2]',     0xf8e08041),
    ('ldapr w0, [x1]',         0xb8bfc020),
]

LITMUS_ASM = '''
mp_rel_writer:
  str x2, [x0]
  stlr x2, [x1]
  ret
mp_acq_reader:
1: ldar x3, [x1]
  cbz x3, 1b
  ldr x4, [x0]
  ret
mp_fence_writer:
  str x2, [x0]
  dmb ishst
  str x2, [x1]
  ret
mp_fence_reader:
1: ldr x3, [x1]
  cbz x3, 1b
  dmb ishld
  ldr x4, [x0]
  ret
mp_plain_writer:
  str x2, [x0]
  str x2, [x1]
  ret
mp_plain_reader:
1: ldr x3, [x1]
  cbz x3, 1b
  ldr x4, [x0]
  ret
sb_fence:
  mov x2, #1
  str x2, [x0]
  dmb ish
  ldr x3, [x1]
  ret
sb_dsb:
  mov x2, #1
  str x2, [x0]
  dsb sy
  ldr x3, [x1]
  ret
sb_rel_acq:
  mov x2, #1
  stlr x2, [x0]
  ldar x3, [x1]
  ret
sb_plain:
  mov x2, #1
  str x2, [x0]
  ldr x3, [x1]
  ret
lock_inc_a:
  mov w3, #1
1: ldaxr w1, [x0]
  cbnz w1, 1b
  stxr w2, w3, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
  stlr wzr, [x0]
  subs x6, x6, #1
  b.ne 1b
  ret
lock_inc_b:
  mov w3, #1
1: ldxr w1, [x0]
  cbnz w1, 1b
  stxr w2, w3, [x0]
  cbnz w2, 1b
  dmb ish
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
2: ldxr w1, [x0]
  stlxr w2, wzr, [x0]
  cbnz w2, 2b
  subs x6, x6, #1
  b.ne 1b
  ret
lock_inc_byte:
  mov w3, #1
1: ldaxrb w1, [x0]
  cbnz w1, 1b
  stxrb w2, w3, [x0]
  cbnz w2, 1b
  ldr x4, [x5]
  add x4, x4, #1
  str x4, [x5]
  stlrb wzr, [x0]
  subs x6, x6, #1
  b.ne 1b
  ret
atomic_inc_sc:
1: ldaxr x4, [x5]
  add x4, x4, #1
  stlxr w2, x4, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
atomic_inc_relaxed:
1: ldxr w4, [x5]
  add w4, w4, #1
  stxr w2, w4, [x5]
  cbnz w2, 1b
  subs x6, x6, #1
  b.ne 1b
  ret
clrex_check:
  ldxr x1, [x0]
  clrex
  stxr w2, x1, [x0]
  ldxr x1, [x0]
  stxr w3, x1, [x0]
  ret
'''

# GNU as output for LITMUS_ASM, in order; the functions follow each other as in the listing
LITMUS_WORDS = [
    ('mp_rel_writer', [0xf9000002, 0xc89ffc22, 0xd65f03c0]),
    ('mp_acq_reader', [0xc8dffc23, 0xb4ffffe3, 0xf9400004, 0xd65f03c0]),
    ('mp_fence_writer', [0xf9000002, 0xd5033abf, 0xf9000022, 0xd65f03c0]),
    ('mp_fence_reader', [0xf9400023, 0xb4ffffe3, 0xd50339bf, 0xf9400004, 0xd65f03c0]),
    ('mp_plain_writer', [0xf9000002, 0xf9000022, 0xd65f03c0]),
    ('mp_plain_reader', [0xf9400023, 0xb4ffffe3, 0xf9400004, 0xd65f03c0]),
    ('sb_fence', [0xd2800022, 0xf9000002, 0xd5033bbf, 0xf9400023, 0xd65f03c0]),
    ('sb_dsb', [0xd2800022, 0xf9000002, 0xd5033f9f, 0xf9400023, 0xd65f03c0]),
    ('sb_rel_acq', [0xd2800022, 0xc89ffc02, 0xc8dffc23, 0xd65f03c0]),
    ('sb_plain', [0xd2800022, 0xf9000002, 0xf9400023, 0xd65f03c0]),
    ('lock_inc_a', [0x52800023, 0x885ffc01, 0x35ffffe1, 0x88027c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4,
                    0x889ffc1f, 0xf10004c6, 0x54fffee1, 0xd65f03c0]),
    ('lock_inc_b', [0x52800023, 0x885f7c01, 0x35ffffe1, 0x88027c03, 0x35ffffa2, 0xd5033bbf, 0xf94000a4, 0x91000484,
                    0xf90000a4, 0x885f7c01, 0x8802fc1f, 0x35ffffc2, 0xf10004c6, 0x54fffe81, 0xd65f03c0]),
    ('lock_inc_byte', [0x52800023, 0x085ffc01, 0x35ffffe1, 0x08027c03, 0x35ffffa2, 0xf94000a4, 0x91000484, 0xf90000a4,
                       0x089ffc1f, 0xf10004c6, 0x54fffee1, 0xd65f03c0]),
    ('atomic_inc_sc', [0xc85ffca4, 0x91000484, 0xc802fca4, 0x35ffffa2, 0xf10004c6, 0x54ffff61, 0xd65f03c0]),
    ('atomic_inc_relaxed', [0x885f7ca4, 0x11000484, 0x88027ca4, 0x35ffffa2, 0xf10004c6, 0x54ffff61, 0xd65f03c0]),
    ('clrex_check', [0xc85f7c01, 0xd5033f5f, 0xc8027c01, 0xc85f7c01, 0xc8037c01, 0xd65f03c0]),
]


class NoCalls:
    """Env stand-in: the test programs never call out of themselves."""
    full = False

    def call(self, target):
        raise AssertionError('unexpected call to %x' % target)


def lift_one(mod, w):
    return mod.Fn(PC, PC + 4, NoCalls()).lift(w, PC)


def test_decoder():
    fails = passes = 0
    for mod in (recomp, recomp_nro):
        for asm, w, want in CASES:
            got = lift_one(mod, w)
            if got != want:
                fails += 1
                print('FAIL %-11s %-22s %08x\n     got:  %s\n     want: %s' % (mod.__name__, asm, w, got, want))
            else:
                passes += 1
        for asm, w in UNIMPL:
            try:
                got = lift_one(mod, w)
            except mod.Unimpl:
                passes += 1
            else:
                fails += 1
                print('FAIL %-11s %-22s %08x expected Unimpl, got %s' % (mod.__name__, asm, w, got))
    # the two lifters must agree on everything above
    print('decoder: %d passed, %d failed' % (passes, fails))
    return fails == 0


def gen_litmus(out_path):
    """Each program becomes `void g_<name>(Ctx *c)` built the way lift_function builds a function."""
    out = ['/* generated by tests/arm_host/test_lifter_atomics.py --gen from tools/lifter/recomp.py -- do not edit */']
    addr = PC
    for name, words in LITMUS_WORDS:
        end = addr + 4 * len(words)
        fn = recomp.Fn(addr, end, NoCalls())
        out.append('static void g_%s(Ctx *c) {' % name)
        for i, w in enumerate(words):
            out.append('L_%x: %s' % (addr + 4 * i, fn.lift(w, addr + 4 * i)))
        out.append('  return;')
        out.append('}')
        addr = end
    with open(out_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s (%d functions)' % (out_path, len(LITMUS_WORDS)))


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--gen':
        gen_litmus(sys.argv[2])
    elif len(sys.argv) > 1 and sys.argv[1] == '--asm':
        sys.stdout.write(LITMUS_ASM.lstrip())
    elif len(sys.argv) > 1 and sys.argv[1] == '--words':
        for name, words in LITMUS_WORDS:
            for w in words:
                print('%08x' % w)
    else:
        sys.exit(0 if test_decoder() else 1)
