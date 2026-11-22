#!/usr/bin/env python3
"""Vérifie que la bibliothèque générée se lie, avec pour seuls manques les imports du XEX.

Usage : python3 -I link_check.py <libwakfu_ppc.a> <xexdump.txt> <out/ppc> <dossier_travail>

1) lie toute la bibliothèque (--whole-archive) avec un main qui référence PPCFuncMappings,
   sans aucun stub : relève les symboles indéfinis ;
2) vérifie que ce sont exactement des imports (__imp__<nom> listés par xexdump) ;
3) génère des stubs vides pour ces imports et relie : l'édition de liens doit réussir.
"""
import os
import re
import subprocess
import sys


def link(lib, ppc_dir, srcs, out):
    cmd = ['clang++', '-std=c++20', '-O0', '-march=sandybridge', '-I', ppc_dir, '-I', SIMDE,
           '-o', out] + srcs + ['-Wl,--whole-archive', lib, '-Wl,--no-whole-archive']
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    global SIMDE
    lib, dump, ppc_dir, work = sys.argv[1:5]
    SIMDE = os.environ.get('SIMDE_DIR', '')
    os.makedirs(work, exist_ok=True)
    imports = {l.split()[2] for l in open(dump) if l.startswith('import ')}
    main_cpp = os.path.join(work, 'main.cpp')
    with open(main_cpp, 'w') as f:
        f.write('#include "ppc_context.h"\n'
                'int main() { size_t n = 0; for (auto* m = PPCFuncMappings; m->host; m++) n++; return n == 0; }\n')
    r = link(lib, ppc_dir, [main_cpp], os.path.join(work, 'sans_stubs'))
    # Les imports sont déclarés en linkage C++ (PPC_EXTERN_FUNC) : le lieur affiche la signature.
    missing = sorted({m.split('(')[0] for m in re.findall(r"undefined reference to [`']([^']+)'", r.stderr)})
    if r.returncode == 0:
        print('Lien sans stubs réussi (aucun symbole manquant).')
    other = [m for m in missing if m not in imports]
    print('Symboles manquants : %d (imports : %d, autres : %d)' % (len(missing), len(missing) - len(other), len(other)))
    for m in other:
        print('  AUTRE : %s' % m)
    if r.returncode and not missing:
        print(r.stderr[-3000:])
        sys.exit(1)
    stubs = os.path.join(work, 'stubs.cpp')
    with open(stubs, 'w') as f:
        f.write('#include "ppc_context.h"\n// Stubs vides des imports noyau/XAM (vérification de lien uniquement)\n')
        for m in missing:
            f.write('PPC_FUNC(%s) {}\n' % m)
    r2 = link(lib, ppc_dir, [main_cpp, stubs], os.path.join(work, 'avec_stubs'))
    if r2.returncode:
        print(r2.stderr[-3000:])
        print('ÉCHEC : le lien avec stubs échoue')
        sys.exit(1)
    print('Lien avec stubs réussi : %s' % os.path.join(work, 'avec_stubs'))
    with open(os.path.join(work, 'imports_manquants.txt'), 'w') as f:
        f.write('\n'.join(missing) + '\n')
    sys.exit(1 if other else 0)


if __name__ == '__main__':
    main()
