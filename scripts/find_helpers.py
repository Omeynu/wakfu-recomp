#!/usr/bin/env python3
"""Trouve et VÉRIFIE les fonctions d'aide du compilateur Xbox 360 (save/rest gpr/fpr/vmx).

Usage : python3 -I find_helpers.py <image.bin> <xexdump.txt>

Méthode : on cherche le premier mot attendu de chaque routine (ex. `std r14,-0x98(r1)`
= 0xF9C1FF68), puis on vérifie la séquence complète instruction par instruction
(registres 14..31 ou 64..127, décalages, épilogue `blr`). Sortie : lignes TOML pour [main].
"""
import struct
import sys


def main():
    img_path, dump = sys.argv[1:3]
    lines = open(dump).read().splitlines()
    base = int(lines[0].split()[1], 16)
    text = [l.split() for l in lines if l.startswith('section ') and l.split()[4] == 'code'][0]
    t0, t1 = int(text[2], 16), int(text[2], 16) + int(text[3], 16)
    d = open(img_path, 'rb').read()

    def w(a):
        return struct.unpack_from('>I', d, a - base)[0]

    def find_unique(word):
        hits = [a for a in range(t0, t1, 4) if w(a) == word]
        if len(hits) != 1:
            raise SystemExit('motif %#010x : %d occurrences (attendu 1)' % (word, len(hits)))
        return hits[0]

    BLR, MTLR12, LWZ12, STW12 = 0x4E800020, 0x7D8803A6, 0x8181FFF8, 0x9181FFF8

    def d16(v):
        return v & 0xFFFF

    def expect(a, words, name):
        for i, x in enumerate(words):
            if w(a + 4 * i) != x:
                raise SystemExit('%s : mot %d à %#x = %#010x, attendu %#010x' % (name, i, a + 4 * i, w(a + 4 * i), x))

    res = {}
    # GPR : std/ld rN,-(0x98-8k)(r1) ; save finit par stw r12,-8(r1) ; blr — rest par lwz r12 ; mtlr ; blr
    a = find_unique(0xF9C1FF68)
    expect(a, [(62 << 26) | (r << 21) | (1 << 16) | d16(-0x98 + 8 * (r - 14)) for r in range(14, 32)] + [STW12, BLR], 'savegprlr')
    res['savegprlr_14_address'] = a
    a = find_unique(0xE9C1FF68)
    expect(a, [(58 << 26) | (r << 21) | (1 << 16) | d16(-0x98 + 8 * (r - 14)) for r in range(14, 32)] + [LWZ12, MTLR12, BLR], 'restgprlr')
    res['restgprlr_14_address'] = a
    # FPR : stfd/lfd fN,-(0x90-8k)(r12) ; blr
    for name, op, first in (('savefpr_14_address', 54, 0xD9CCFF70), ('restfpr_14_address', 50, 0xC9CCFF70)):
        a = find_unique(first)
        expect(a, [(op << 26) | (r << 21) | (12 << 16) | d16(-0x90 + 8 * (r - 14)) for r in range(14, 32)] + [BLR], name)
        res[name] = a
    # VMX 14..31 : li r11,-0x120+16k ; stvx/lvx vN,r11,r12 ; blr
    for name, xo in (('savevmx_14_address', 231), ('restvmx_14_address', 103)):
        seq = []
        for k, v in enumerate(range(14, 32)):
            seq += [(14 << 26) | (11 << 21) | d16(-0x120 + 16 * k), (31 << 26) | (v << 21) | (11 << 16) | (12 << 11) | (xo << 1)]
        cands = [x for x in range(t0, t1, 4) if w(x) == seq[0] and w(x + 4) == seq[1]]
        if len(cands) != 1:
            raise SystemExit('%s : %d candidats' % (name, len(cands)))
        expect(cands[0], seq + [BLR], name)
        res[name] = cands[0]
    # VMX128 64..127 : li r11,-0x400+16k ; stvx128/lvx128 (opcode 4) ; blr. Situé juste après la routine 14..31.
    for name, after in (('savevmx_64_address', 'savevmx_14_address'), ('restvmx_64_address', 'restvmx_14_address')):
        a = res[after] + 18 * 8 + 4
        for k in range(64):
            if w(a + 8 * k) != (14 << 26) | (11 << 21) | d16(-0x400 + 16 * k) or w(a + 8 * k + 4) >> 26 != 4:
                raise SystemExit('%s : séquence VMX128 inattendue à %#x' % (name, a + 8 * k))
        if w(a + 8 * 64) != BLR:
            raise SystemExit('%s : blr absent' % name)
        res[name] = a
    for k in ('savegprlr_14_address', 'restgprlr_14_address', 'savefpr_14_address', 'restfpr_14_address',
              'savevmx_14_address', 'restvmx_14_address', 'savevmx_64_address', 'restvmx_64_address'):
        print('%s = 0x%08X' % (k, res[k]))


if __name__ == '__main__':
    main()
