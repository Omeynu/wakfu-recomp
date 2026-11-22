#!/usr/bin/env python3
"""Test différentiel des instructions ajoutées à XenonRecomp : QEMU (référence) contre C++ recompilé.

Usage : python3 -I run_tests.py <XenonRecomp> <XenonUtils> <simde> <dossier_travail>

Pour chaque cas de cases.py : le corps est assemblé (binutils powerpc64-linux-gnu) ;
- côté QEMU, lié à un harnais qui lit l'état d'entrée sur stdin, appelle le corps et écrit l'état
  de sortie (qemu-ppc -cpu 7400, ou qemu-ppc64 -cpu 970 pour les instructions 64 bits) ;
- côté recomp, lié seul en ELF32, recompilé par XenonRecomp (mode test), appelé par un pilote C++
  qui lit/écrit le même format d'état.
Les deux états de sortie sont comparés champ par champ.
"""
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases as C  # noqa: E402

AS, LD = 'powerpc64-linux-gnu-as', 'powerpc64-linux-gnu-ld'
# Format d'état (big-endian) : r3..r12 (10x8) | cr(4) xer(4) ctr(8) | f1..f4 (4x8) | v1..v6 (6x16) | mem(64)
OFF_CR, OFF_XER, OFF_CTR, OFF_F, OFF_V, OFF_MEM, SIZE = 80, 84, 88, 96, 128, 224, 288


def pack_state(st):
    b = bytearray(SIZE)
    for k, val in st.get('r', {}).items():
        struct.pack_into('>Q', b, (k - 3) * 8, val)
    struct.pack_into('>IIQ', b, OFF_CR, st.get('cr', 0), st.get('xer', 0), st.get('ctr', 0))
    for k, val in st.get('f', {}).items():
        struct.pack_into('>Q', b, OFF_F + (k - 1) * 8, val)
    for k, val in st.get('v', {}).items():
        b[OFF_V + (k - 1) * 16:OFF_V + k * 16] = val
    b[OFF_MEM:OFF_MEM + 64] = st.get('mem', bytes(64))
    return bytes(b)


def harness(wide):
    ld, st = ('ld', 'std') if wide else ('lwz', 'stw')
    g = 0 if wide else 4                       # en 32 bits : mot de poids faible de l'emplacement 8 octets
    L = ['.abiversion 2' if wide else '', '.text', '.globl _start', '_start:',
         'lis r31,inbuf@ha', 'addi r31,r31,inbuf@l',
         'li r0,3', 'li r3,0', 'mr r4,r31', 'li r5,%d' % SIZE, 'sc',
         'lis r30,%d' % (C.MEM >> 16), 'ori r30,r30,%d' % (C.MEM & 0xFFFF),
         'li r29,0', '1: addi r28,r29,%d' % OFF_MEM, 'lbzx r27,r31,r28', 'stbx r27,r30,r29',
         'addi r29,r29,1', 'cmpwi r29,64', 'blt 1b']
    L += ['%s r%d,%d(r31)' % (ld, k, (k - 3) * 8 + g) for k in range(3, 13)]
    L += ['lwz r0,%d(r31)' % OFF_CR, 'mtcrf 0xff,r0', 'lwz r0,%d(r31)' % OFF_XER, 'mtxer r0',
          '%s r0,%d(r31)' % (ld, OFF_CTR + g), 'mtctr r0']
    L += ['lfd f%d,%d(r31)' % (k, OFF_F + (k - 1) * 8) for k in range(1, 5)]
    if not wide:
        L += ['li r0,%d' % (OFF_V + (k - 1) * 16) + '\nlvx v%d,r31,r0' % k for k in range(1, 7)]
    L += ['bl body', 'lis r31,outbuf@ha', 'addi r31,r31,outbuf@l']
    L += ['%s r%d,%d(r31)' % (st, k, (k - 3) * 8 + g) for k in range(3, 13)]
    L += ['mfcr r0', 'stw r0,%d(r31)' % OFF_CR, 'mfxer r0', 'stw r0,%d(r31)' % OFF_XER,
          'mfctr r0', '%s r0,%d(r31)' % (st, OFF_CTR + g)]
    L += ['stfd f%d,%d(r31)' % (k, OFF_F + (k - 1) * 8) for k in range(1, 5)]
    if not wide:
        L += ['li r0,%d' % (OFF_V + (k - 1) * 16) + '\nstvx v%d,r31,r0' % k for k in range(1, 7)]
    L += ['li r29,0', '2: addi r28,r29,%d' % OFF_MEM, 'lbzx r27,r30,r29', 'stbx r27,r31,r28',
          'addi r29,r29,1', 'cmpwi r29,64', 'blt 2b',
          'li r0,4', 'li r3,1', 'mr r4,r31', 'li r5,%d' % SIZE, 'sc', 'li r0,1', 'li r3,0', 'sc',
          '.data', '.balign 16', 'inbuf: .space %d' % SIZE, '.balign 16', 'outbuf: .space %d' % SIZE,
          '.section .testmem,"aw"', '.space 64']
    return '\n'.join(L) + '\n'


def body_src(lines, wide):
    return '\n'.join(['.text', '.globl body', 'body:'] + lines + ['blr']) + '\n'


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    if r.returncode:
        raise SystemExit('ÉCHEC %s\n%s%s' % (' '.join(cmd), r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')))
    return r.stdout


def asm(src, path, wide, a64):
    with open(path + '.s', 'w') as f:
        f.write(src)
    flags = ['-a64' if a64 else '-a32', '-mregnames', '-many', '-mppc64' if wide else '-maltivec']
    run([AS] + flags + ['-o', path + '.o', path + '.s'])
    return path + '.o'


DRIVER_HEAD = r'''
#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <string>
static uint64_t rd64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }
static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static void wr64(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; i--) { p[i] = uint8_t(v); v >>= 8; } }
static void wr32(uint8_t* p, uint32_t v) { for (int i = 3; i >= 0; i--) { p[i] = uint8_t(v); v >>= 8; } }
static void setcr(PPCContext& c, uint32_t v) {
    PPCCRRegister* f[8] = { &c.cr0, &c.cr1, &c.cr2, &c.cr3, &c.cr4, &c.cr5, &c.cr6, &c.cr7 };
    for (int n = 0; n < 8; n++) { uint32_t x = v >> (28 - 4 * n); f[n]->lt = x >> 3 & 1; f[n]->gt = x >> 2 & 1; f[n]->eq = x >> 1 & 1; f[n]->so = x & 1; }
}
static uint32_t getcr(PPCContext& c) {
    PPCCRRegister* f[8] = { &c.cr0, &c.cr1, &c.cr2, &c.cr3, &c.cr4, &c.cr5, &c.cr6, &c.cr7 };
    uint32_t v = 0;
    for (int n = 0; n < 8; n++) v |= uint32_t((f[n]->lt & 1) << 3 | (f[n]->gt & 1) << 2 | (f[n]->eq & 1) << 1 | (f[n]->so & 1)) << (28 - 4 * n);
    return v;
}
typedef void Fn(PPCContext&, uint8_t*);
'''

DRIVER_MAIN = r'''
int main(int argc, char** argv) {
    uint8_t in[%(SIZE)d], out[%(SIZE)d] = {};
    if (read(0, in, sizeof in) != sizeof in) return 2;
    Fn* fn = nullptr;
    std::string t = argv[1];
%(DISPATCH)s
    if (!fn) return 3;
    uint8_t* base = (uint8_t*)mmap(nullptr, 0x10100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    memcpy(base + 0x%(MEM)X, in + %(OFF_MEM)d, 64);
    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    PPCRegister* r[10] = { &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10, &ctx.r11, &ctx.r12 };
    for (int k = 0; k < 10; k++) r[k]->u64 = rd64(in + 8 * k);
    setcr(ctx, rd32(in + %(OFF_CR)d));
    uint32_t xer = rd32(in + %(OFF_XER)d);
    ctx.xer.so = xer >> 31 & 1; ctx.xer.ov = xer >> 30 & 1; ctx.xer.ca = xer >> 29 & 1;
    ctx.ctr.u64 = rd64(in + %(OFF_CTR)d);
    decltype(&ctx.f1) fr[4] = { &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4 };
    for (int k = 0; k < 4; k++) fr[k]->u64 = rd64(in + %(OFF_F)d + 8 * k);
    PPCVRegister* vr[6] = { &ctx.v1, &ctx.v2, &ctx.v3, &ctx.v4, &ctx.v5, &ctx.v6 };
    for (int k = 0; k < 6; k++) for (int w = 0; w < 4; w++) vr[k]->u32[3 - w] = rd32(in + %(OFF_V)d + 16 * k + 4 * w);
    fn(ctx, base);
    for (int k = 0; k < 10; k++) wr64(out + 8 * k, r[k]->u64);
    wr32(out + %(OFF_CR)d, getcr(ctx));
    wr32(out + %(OFF_XER)d, uint32_t(ctx.xer.so) << 31 | uint32_t(ctx.xer.ov) << 30 | uint32_t(ctx.xer.ca) << 29);
    wr64(out + %(OFF_CTR)d, ctx.ctr.u64);
    for (int k = 0; k < 4; k++) wr64(out + %(OFF_F)d + 8 * k, fr[k]->u64);
    for (int k = 0; k < 6; k++) for (int w = 0; w < 4; w++) wr32(out + %(OFF_V)d + 16 * k + 4 * w, vr[k]->u32[3 - w]);
    memcpy(out + %(OFF_MEM)d, base + 0x%(MEM)X, 64);
    return write(1, out, sizeof out) == sizeof out ? 0 : 4;
}
'''


def compare(a, b, wide, vcompare, st):
    """Liste des différences entre l'état QEMU (a) et l'état recompilé (b)."""
    diffs = []
    for k in range(10):
        x, y = (struct.unpack_from('>Q', s, k * 8)[0] for s in (a, b))
        if not wide:
            x, y = x & 0xFFFFFFFF, y & 0xFFFFFFFF
        if x != y:
            diffs.append('r%d qemu=%#x recomp=%#x' % (k + 3, x, y))
    for name, off, mask in (('cr', OFF_CR, 0xFFFFFFFF), ('xer', OFF_XER, 0x20000000)):
        x, y = (struct.unpack_from('>I', s, off)[0] & mask for s in (a, b))
        if x != y:
            diffs.append('%s qemu=%#010x recomp=%#010x' % (name, x, y))
    x, y = (struct.unpack_from('>Q', s, OFF_CTR)[0] for s in (a, b))
    if (x if wide else x & 0xFFFFFFFF) != (y if wide else y & 0xFFFFFFFF):
        diffs.append('ctr qemu=%#x recomp=%#x' % (x, y))
    if a[OFF_F:OFF_V] != b[OFF_F:OFF_V]:
        diffs.append('fpr qemu=%s recomp=%s' % (a[OFF_F:OFF_V].hex(), b[OFF_F:OFF_V].hex()))
    for k in range(6):
        va, vb = (s[OFF_V + 16 * k:OFF_V + 16 * k + 16] for s in (a, b))
        if vcompare == 'lvehx' and k == 0:      # seul l'élément adressé est défini par l'architecture
            e = ((C.MEM + st['r'][5]) & 0xF) & ~1
            va, vb = va[e:e + 2], vb[e:e + 2]
        if va != vb:
            diffs.append('v%d qemu=%s recomp=%s' % (k + 1, va.hex(), vb.hex()))
    if a[OFF_MEM:] != b[OFF_MEM:]:
        diffs.append('mem qemu=%s recomp=%s' % (a[OFF_MEM:].hex(), b[OFF_MEM:].hex()))
    return diffs


def main():
    xrecomp, xutils, simde, work = sys.argv[1:5]
    srcdir, gen = os.path.join(work, 'src'), os.path.join(work, 'gen')
    for d in (srcdir, gen):
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
    cases = C.all_cases()
    hw = {}
    for wide in (False, True):
        hw[wide] = asm(harness(wide), os.path.join(work, 'harness%d' % wide), wide, wide)
    symbols = {}
    for c in cases:
        n, wide = c['name'], c['wide']
        q = asm(body_src(c['qemu'], wide), os.path.join(work, n + '_q'), wide, wide)
        run([LD, '-m', 'elf64ppc' if wide else 'elf32ppc', '--section-start=.testmem=0x%X' % C.MEM,
             '-o', os.path.join(work, n + '_qemu'), hw[wide], q])
        r = asm(body_src(c['body'], wide), os.path.join(work, n + '_r'), wide, False)
        run([LD, '-m', 'elf32ppc', '-e', 'body', '-Ttext=0x10000', '-o', os.path.join(srcdir, n + '.o'), r])
        nm = run(['powerpc64-linux-gnu-nm', os.path.join(srcdir, n + '.o')]).decode()
        symbols[n] = int([l for l in nm.splitlines() if l.endswith(' body')][0].split()[0], 16)
    run([xrecomp, srcdir, gen])
    for c in cases:
        cpp = open(os.path.join(gen, c['name'] + '.cpp')).read()
        if c['decode'] and '// %s ' % c['decode'] not in cpp:
            raise SystemExit('%s : le mot .long n\'est pas décodé en %s' % (c['name'], c['decode']))
        if 'unimplemented' in cpp.lower():
            raise SystemExit('%s : instruction non implémentée dans le C++ généré' % c['name'])
    decl = ''.join('PPC_FUNC(%s_%X);\n' % (c['name'], symbols[c['name']]) for c in cases)
    disp = ''.join('    if (t == "%s") fn = %s_%X;\n' % (c['name'], c['name'], symbols[c['name']]) for c in cases)
    with open(os.path.join(gen, 'driver.cpp'), 'w') as f:
        f.write(DRIVER_HEAD + decl + DRIVER_MAIN % dict(SIZE=SIZE, DISPATCH=disp, MEM=C.MEM, OFF_MEM=OFF_MEM,
                                                        OFF_CR=OFF_CR, OFF_XER=OFF_XER, OFF_CTR=OFF_CTR,
                                                        OFF_F=OFF_F, OFF_V=OFF_V))
    srcs = [os.path.join(gen, c['name'] + '.cpp') for c in cases] + [os.path.join(gen, 'driver.cpp')]
    exe = os.path.join(work, 'driver')
    run(['clang++', '-std=c++20', '-O1', '-march=sandybridge', '-fno-strict-aliasing', '-ffp-model=strict',
         '-Wno-unused-label', '-Wno-unused-variable', '-I', xutils, '-I', simde, '-o', exe] + srcs)
    total = bad = 0
    report = []
    for c in cases:
        qemu = ['qemu-ppc64', '-cpu', '970'] if c['wide'] else ['qemu-ppc', '-cpu', '7400_v2.0']
        for i, st in enumerate(c['inputs']):
            blob = pack_state(st)
            a = run(qemu + [os.path.join(work, c['name'] + '_qemu')], input=blob)
            b = run([exe, c['name']], input=blob)
            if len(a) != SIZE or len(b) != SIZE:
                raise SystemExit('%s : état de sortie tronqué' % c['name'])
            d = compare(a, b, c['wide'], c['vcompare'], st)
            total += 1
            if d:
                bad += 1
                report.append('ÉCHEC %s #%d : %s' % (c['name'], i, ' ; '.join(d)))
    print('\n'.join(report))
    print('%d cas, %d exécutions, %d échecs' % (len(cases), total, bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
