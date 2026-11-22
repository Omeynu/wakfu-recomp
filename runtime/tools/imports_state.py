#!/usr/bin/env python3
"""Écrit runtime/imports.md : état de chaque import (implémenté / stub) d'après les sources.
Usage : python3 -I imports_state.py <imports_list.inc> <runtime/src> <sortie.md> [journal d'exécution]"""
import os, re, sys
lst, src, out = sys.argv[1:4]
seen = set(re.findall(r'^\[premier\] \w+ (\w+)', open(sys.argv[4]).read(), re.M)) if len(sys.argv) > 4 else set()
impl = set()
for d, _, fs in os.walk(src):
    for f in fs:
        if f.endswith('.cpp'):
            impl |= set(re.findall(r'WK_IMPORT\((\w+)\)', open(os.path.join(d, f)).read()))
names = sorted(re.findall(r'X\(0x[0-9A-F]+, (\w+)\)', open(lst).read()))
rows = ['| `%s` | %s | %s |' % (n, 'implémenté' if n in impl else 'stub', 'oui' if n in seen else '') for n in names]
open(out, 'w').write('# État des imports du runtime\n\nGénéré par `runtime/tools/imports_state.py`. '
    '« Appelé » : vu lors de la dernière exécution de référence.\n\n%d implémentés sur %d.\n\n'
    '| Import | État | Appelé |\n|---|---|---|\n%s\n' % (sum(n in impl for n in names), len(names), '\n'.join(rows)))
print('%d/%d implémentés' % (sum(n in impl for n in names), len(names)))
