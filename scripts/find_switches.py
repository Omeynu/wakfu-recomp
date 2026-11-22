#!/usr/bin/env python3
"""Détecte les tables de sauts (switch) d'un binaire PPC Xbox 360 et écrit un TOML XenonRecomp.

Usage : python3 -I find_switches.py <image.bin> <xexdump.txt> <manuel.toml> <sortie.toml> [rapport.txt]

manuel.toml : cas relus à la main ([[switch]] bctr, start, r, count, default optionnel) —
garde absente ou portant sur une copie du registre d'index. L'émulation part de `start`.

Remplace XenonAnalyse, dont les motifs fixes ratent ce compilateur (nop intercalés).
Pour chaque `bctr` : remonte jusqu'au couple `cmplwi crN,rI,N` / `bgt crN,défaut`, puis
émule la ligne droite entre le `bgt` et le `bctr` pour chaque index 0..N (lectures
dans l'image). Refuse (et rapporte) dès qu'une instruction n'est pas gérée, qu'un
registre inconnu intervient, ou que rI est modifié avant le `bctr`.
`base` = adresse du `bctr` (XenonRecomp applique la table au prochain bctr).
"""
import struct
import sys

MASK = 0xFFFFFFFF


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def rotl(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & MASK if n else v


def mask(mb, me):
    m = 0
    for i in range(32):
        if (mb <= me and mb <= i <= me) or (mb > me and (i >= mb or i <= me)):
            m |= 1 << (31 - i)
    return m


class Unsupported(Exception):
    pass


class Img:
    def __init__(self, data, base):
        self.d, self.base = data, base

    def word(self, a):
        return struct.unpack_from('>I', self.d, a - self.base)[0]

    def load(self, a, n):
        o = a - self.base
        if not 0 <= o <= len(self.d) - n:
            raise Unsupported('lecture hors image %#x' % a)
        return int.from_bytes(self.d[o:o + n], 'big')


def fields(i):
    return i >> 26, (i >> 21) & 31, (i >> 16) & 31, (i >> 11) & 31, i & 0xFFFF


def is_nop(i):
    return i == 0x60000000


def step(img, ins, regs, idx_reg):
    """Exécute une instruction ; regs : dict registre -> valeur (absent = inconnu)."""
    op, d, a, b, imm = fields(ins)

    def get(r):
        if r not in regs:
            raise Unsupported('registre r%d inconnu' % r)
        return regs[r]

    def put(r, v):
        if r == idx_reg:
            raise Unsupported('registre d\'index r%d modifié' % r)
        regs[r] = v & MASK

    if is_nop(ins):
        return None
    if op in (36, 38, 44, 48, 50, 52, 54) or (op == 62 and ins & 3 == 0):
        return None                                # stores sans mise à jour, chargements flottants
    if op == 31 and (ins >> 1) & 0x3FF in (151, 215, 407, 149, 663, 727):
        return None                                # stwx/stbx/sthx/stdx/stfsx/stfdx
    if op == 14:                                   # addi / li
        put(d, (get(a) if a else 0) + s16(imm))
    elif op == 15:                                 # addis / lis
        put(d, (get(a) if a else 0) + (s16(imm) << 16))
    elif op == 24:                                 # ori
        put(a, get(d) | imm)
    elif op == 21:                                 # rlwinm (slwi, srwi, clrlwi…)
        sh, mb, me = b, (ins >> 6) & 31, (ins >> 1) & 31
        if ins & 1:
            raise Unsupported('rlwinm.')
        put(a, rotl(get(d), sh) & mask(mb, me))
    elif op in (32, 40, 34):                       # lwz / lhz / lbz
        n = {32: 4, 40: 2, 34: 1}[op]
        put(d, img.load(((get(a) if a else 0) + s16(imm)) & MASK, n))
    elif op == 31:
        xo = (ins >> 1) & 0x3FF
        if xo in (23, 279, 87):                    # lwzx / lhzx / lbzx
            n = {23: 4, 279: 2, 87: 1}[xo]
            put(d, img.load(((get(a) if a else 0) + get(b)) & MASK, n))
        elif xo == 266:                            # add
            put(d, get(a) + get(b))
        elif xo == 444 and not ins & 1:            # or (mr)
            put(a, get(d) | get(b))
        elif xo == 24 and not ins & 1:             # slw
            sh = get(b) & 63
            put(a, 0 if sh > 31 else get(d) << sh)
        elif xo == 467 and ((ins >> 11) & 0x3FF) == 0x120:   # mtctr
            return ('ctr', get(d))
        else:
            raise Unsupported('op31 xo=%d' % xo)
    else:
        raise Unsupported('opcode %d' % op)
    return None


def emulate(img, start, bctr, reg, count):
    labels = []
    for i in range(count):
        regs = {reg: i}
        ctr = None
        for p in range(start, bctr, 4):
            r = step(img, img.word(p), regs, reg)
            if r:
                ctr = r[1]
        if ctr is None:
            raise Unsupported('mtctr absent')
        labels.append(ctr)
    return labels


def find(img, code_start, code_end, func_bounds, manual):
    tables, failures = [], []
    for a in range(code_start, code_end, 4):
        if img.word(a) != 0x4E800420:              # bctr
            continue
        if a in manual:
            m = manual[a]
            try:
                labels = emulate(img, m['start'], a, m['r'], m['count'])
            except Unsupported as e:
                failures.append((a, 'manuel : %s' % e))
                continue
            fb = func_bounds(a)
            out = [l for l in labels if fb and not fb[0] <= l < fb[1]]
            if out:
                failures.append((a, 'manuel : cible hors fonction %s' % ', '.join('%#x' % l for l in out[:3])))
                continue
            tables.append((a, m['r'], m.get('default', 0), labels))
            continue
        # remonter jusqu'au bgt crN (bc BO=12, BI=4N+1) puis au cmplwi crN
        bgt = cmp = None
        for k in range(1, 40):
            p = a - 4 * k
            if p < code_start:
                break
            w = img.word(p)
            op = w >> 26
            if bgt is None:
                if op == 16 and not w & 3:
                    bo, bi = (w >> 21) & 31, (w >> 16) & 31
                    if bo == 12 and bi % 4 == 1:
                        bgt = (p, bi // 4, p + s16(w & 0xFFFC))
                        continue
                if w & 0xFFE0FFFF == 0x4D800020 and ((w >> 16) & 31) % 4 == 1:   # bgtlr crN
                    bgt = (p, ((w >> 16) & 31) // 4, 0)
                    continue
                if op in (16, 18) or (op == 19 and w != 0x4E800420 and ((w >> 1) & 0x3FF) in (16, 528)):
                    break                          # autre branche : pas de ligne droite
            else:
                if op == 10 and (w >> 23) & 7 == bgt[1] and not (w >> 21) & 1:   # cmplwi crN
                    cmp = (p, (w >> 16) & 31, w & 0xFFFF)
                    break
        if not bgt:
            continue                               # bctr sans garde : appel indirect / tail call
        if not cmp:
            failures.append((a, 'bgt sans cmplwi'))
            continue
        _, reg, n = cmp
        if n > 4096:
            failures.append((a, 'table trop grande (%d)' % (n + 1)))
            continue
        labels = []
        try:
            # rI ne doit pas changer entre cmplwi et bgt non plus
            for p in range(cmp[0] + 4, bgt[0], 4):
                w = img.word(p)
                if (w >> 21) & 31 == reg and w >> 26 not in (10, 11, 16, 36, 37, 38, 44, 45, 47) and not is_nop(w):
                    raise Unsupported('r%d peut être modifié avant le bgt (%#x)' % (reg, p))
            labels = emulate(img, bgt[0] + 4, a, reg, n + 1)
        except Unsupported as e:
            failures.append((a, str(e)))
            continue
        fb = func_bounds(a)
        out = [l for l in labels if fb and not fb[0] <= l < fb[1]]
        if out:
            failures.append((a, 'cible hors fonction (.pdata) : %s' % ', '.join('%#x' % l for l in out[:3])))
            continue
        tables.append((a, reg, bgt[2], labels))
    return tables, failures


def pdata_bounds(img, sections):
    """Bornes de fonctions depuis .pdata (début, longueur en mots sur 22 bits)."""
    ents = []
    for name, start, size, _ in sections:
        if name == '.pdata':
            for o in range(start, start + size - 7, 8):
                beg, info = img.word(o), img.word(o + 4)
                if beg:
                    ents.append((beg, beg + ((info >> 8) & 0x3FFFFF) * 4))
    ents.sort()
    starts = [e[0] for e in ents]
    import bisect

    def f(a):
        i = bisect.bisect_right(starts, a) - 1
        return ents[i] if i >= 0 and ents[i][0] <= a < ents[i][1] else None
    return f, ents


def leaf_functions(img, secs, tables, fb):
    """Bornes proposées pour les fonctions sans .pdata qui contiennent une table.

    Début = cible de `bl` la plus proche avant le bctr ; fin = prochain début de fonction
    (.pdata ou cible de `bl`) après la plus grande cible de la table, bourrage nul retiré.
    À recopier dans `functions` de wakfu.toml après relecture.
    """
    import bisect
    _, ents = pdata_bounds(img, secs)
    starts = {b for b, _ in ents}
    for name, start, size, kind in secs:
        if kind != 'code':
            continue
        for a in range(start, start + size, 4):
            w = img.word(a)
            if w >> 26 == 18 and w & 3 == 1:
                li = w & 0x03FFFFFC
                starts.add(a + (li - 0x04000000 if li & 0x02000000 else li))
    starts = sorted(starts)
    out, seen = [], {}
    for a, _, _, labels in tables:
        if fb(a):
            continue
        beg = starts[bisect.bisect_right(starts, a) - 1]
        end = starts[bisect.bisect_right(starts, max(labels))]
        while img.word(end - 4) == 0:
            end -= 4
        if beg in seen:
            seen[beg] = max(seen[beg], end)
        else:
            seen[beg] = end
        if not all(beg <= l < end for l in labels):
            out.append('ATTENTION %#x : cibles hors [%#x, %#x)' % (a, beg, end))
    out += ['FONCTION { address = 0x%X, size = 0x%X },' % (b, e - b) for b, e in sorted(seen.items())]
    return out


def main():
    import tomllib
    image, dump, man, out = sys.argv[1:5]
    rep = sys.argv[5] if len(sys.argv) > 5 else None
    with open(man, 'rb') as f:
        manual = {m['bctr']: m for m in tomllib.load(f).get('switch', [])}
    lines = open(dump).read().splitlines()
    base = int(lines[0].split()[1], 16)
    secs = [(l.split()[1], int(l.split()[2], 16), int(l.split()[3], 16), l.split()[4])
            for l in lines if l.startswith('section ')]
    img = Img(open(image, 'rb').read(), base)
    fb, _ = pdata_bounds(img, secs)
    tables, failures = [], []
    for name, start, size, kind in secs:
        if kind == 'code':
            t, f = find(img, start, start + size, fb, manual)
            tables += t
            failures += f
    with open(out, 'w') as o:
        o.write('# Généré par scripts/find_switches.py (émulation des séquences cmplwi/bgt…bctr)\n\n')
        for a, reg, dflt, labels in tables:
            o.write('[[switch]]\nbase = 0x%X\nr = %d\ndefault = 0x%X\nlabels = [\n' % (a, reg, dflt))
            o.write(''.join('    0x%X,\n' % l for l in labels))
            o.write(']\n\n')
    msg = ['tables : %d ; refus : %d' % (len(tables), len(failures))]
    msg += ['REFUS %#x : %s' % f for f in failures]
    msg += leaf_functions(img, secs, tables, fb)
    if rep:
        open(rep, 'w').write('\n'.join(msg) + '\n')
    print('\n'.join(msg[:40]))


if __name__ == '__main__':
    main()
