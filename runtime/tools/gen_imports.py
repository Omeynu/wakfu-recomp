#!/usr/bin/env python3
"""Génère les stubs d'imports et la table des variables noyau du runtime.

Usage : python3 -I gen_imports.py <xex_patché> <xexdump.txt> <XenonUtils/xbox> <dossier_sortie>

Sorties :
- imports_stub.cpp : une définition FAIBLE par import de fonction (__imp__<Nom>) qui journalise
  l'appel et retourne 0 ; une implémentation réelle (définition forte) la remplace au lien.
- imports_list.inc : X(adresse_thunk, Nom) pour toutes les fonctions importées.
- kernel_vars.inc  : V(adresse_slot, module, ordinal, Nom) pour les variables importées ; le
  runtime écrit dans chaque slot l'adresse invitée de l'objet qu'il alloue.
Le XEX doit être non compressé (vérifié) : l'image brute suit l'en-tête, à l'offset headerSize.
"""
import os
import re
import struct
import sys


def main():
    xex_path, dump, tables, out = sys.argv[1:5]
    x = open(xex_path, 'rb').read()
    be = lambda o: struct.unpack_from('>I', x, o)[0]
    assert x[:4] == b'XEX2'
    header_size = be(8)
    hdr = {}
    for i in range(be(0x14)):
        k, v = struct.unpack_from('>II', x, 0x18 + i * 8)
        hdr[k] = v
    enc, comp = struct.unpack_from('>HH', x, hdr[0x3FF] + 4)
    if enc != 0 or comp not in (0, 1):
        raise SystemExit('XEX chiffré ou compressé : non géré')
    if comp == 1:
        raise SystemExit('compression basique (blocs) : lire l\'image depuis xexdump à la place')
    base = 0x82000000
    img = lambda a: be(header_size + a - base)

    names = {}
    for lib, f in (('xboxkrnl.exe', 'xboxkrnl_table.inc'), ('xam.xex', 'xam_table.inc')):
        for m in re.finditer(r'XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)', open(os.path.join(tables, f)).read()):
            names[(lib, int(m.group(1), 16))] = (m.group(2), m.group(3))

    il = hdr[0x103FF]
    str_size, nlib = be(il + 4), be(il + 8)
    libnames, o = [], il + 12
    while len(libnames) < nlib:
        s = x[o:x.index(b'\0', o)].decode()
        libnames.append(s)
        o += (len(s) + 1 + 3) & ~3
    o = il + 12 + str_size
    variables = []
    for _ in range(nlib):
        size = be(o)
        name_idx, n = struct.unpack_from('>HH', x, o + 0x24)
        lib = libnames[name_idx]
        for j in range(n):
            rec = be(o + 0x28 + 4 * j)
            v = img(rec)
            if v >> 24 == 0:
                nm, kind = names.get((lib, v & 0xFFFF), ('ordinal_%d' % (v & 0xFFFF), '?'))
                if kind == 'kVariable':
                    variables.append((rec, lib, v & 0xFFFF, nm))
        o += size

    funcs = [(int(l.split()[1], 16), l.split()[2][len('__imp__'):]) for l in open(dump) if l.startswith('import ')]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'imports_list.inc'), 'w') as f:
        f.write('// Généré par runtime/tools/gen_imports.py\n')
        f.writelines('X(0x%08X, %s)\n' % a for a in funcs)
    with open(os.path.join(out, 'kernel_vars.inc'), 'w') as f:
        f.write('// Généré par runtime/tools/gen_imports.py\n')
        f.writelines('V(0x%08X, "%s", %d, %s)\n' % v for v in variables)
    with open(os.path.join(out, 'imports_stub.cpp'), 'w') as f:
        f.write('// Généré par runtime/tools/gen_imports.py : stubs faibles, remplacés par les implémentations.\n'
                '#include "kernel/stub.h"\n\n')
        for addr, nm in funcs:
            f.write('__attribute__((weak)) PPC_FUNC(__imp__%s) { wk::StubCall("%s", 0x%08X, ctx); }\n' % (nm, nm, addr))
    print('%d fonctions, %d variables' % (len(funcs), len(variables)))


if __name__ == '__main__':
    main()
