# profil-agg.py <dossier> <lwp> [profondeur] : fonctions les plus fréquentes dans les piles d'un thread (20 échantillons gdb)
import re, sys, glob, collections
d, lwp = sys.argv[1], sys.argv[2]; depth = int(sys.argv[3]) if len(sys.argv) > 3 else 6
top, incl, stacks = collections.Counter(), collections.Counter(), collections.Counter()
n = 0
for f in sorted(glob.glob(d + '/s*.txt')):
    txt = open(f, errors='replace').read()
    m = re.search(r'\(LWP %s\)[^\n]*\n((?:#\d+[^\n]*\n)+)' % lwp, txt)
    if not m: continue
    n += 1
    fr = []
    for line in m.group(1).splitlines():
        mm = re.match(r'#\d+\s+(?:0x[0-9a-f]+ in )?([^\s(]+)', line)
        if mm: fr.append(mm.group(1))
    top[fr[0] if fr else '?'] += 1
    for x in set(fr): incl[x] += 1
    stacks[' < '.join(fr[:depth])] += 1
print('échantillons', n)
print('-- haut de pile'); [print(c, k) for k, c in top.most_common(10)]
print('-- inclusif'); [print(c, k) for k, c in incl.most_common(25)]
print('-- piles'); [print(c, k[:300]) for k, c in stacks.most_common(6)]
