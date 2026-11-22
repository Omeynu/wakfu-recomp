#!/usr/bin/env python3
"""Fonctions à générer avec leurs registres dans ctx (registers_in_context de config/wakfu.toml).

Avec non_volatile_as_local / non_argument_as_local, chaque fonction a ses registres en variables
locales. Les funclets __finally de MSVC lisent pourtant les registres de leur parent (r12 = cadre,
r28-r31...) : avec des locales à 0, ils libèrent la section critique 0 et le jeu se bloque.
Ce script fait une analyse de flux par blocs sur le C++ généré : une fonction qui peut lire un
registre local r/f avant de l'écrire (sur un chemin) est listée, avec ses appelants directs.

À lancer sur un C++ généré avec registers_in_context = [] (sinon les fonctions déjà listées
n'ont plus de locales et disparaissent du résultat) :
    python3 -I scripts/find_context_functions.py <dossier ppc>   -> tableau TOML sur stdout
"""
import re, sys, glob
WR = re.compile(r'^\s*(r\d+|f\d+)\.\w+\s*=[^=]')
TOK = re.compile(r'\b(r\d+|f\d+)\.')
funcs = {}
for path in sorted(glob.glob(sys.argv[1] + '/ppc_recomp.*.cpp')):
    fn = None
    for line in open(path):
        m = re.match(r'PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)', line)
        if m: fn = m[1]; funcs[fn] = []; continue
        if fn is None: continue
        if line.startswith('}'): fn = None; continue
        if not line.lstrip().startswith('//'): funcs[fn].append(line.rstrip())
calls = {}
res = {}
for fn, lines in funcs.items():
    locs = {m[1] for l in lines for m in [re.match(r'\tPPCRegister (r\d+|f\d+)\{\};', l)] if m}
    body = [l.strip() for l in lines if not re.match(r'\tPPC(V|CR|XER)?Register |\tuint32_t ea|\tPPCContext env|\tPPC_FUNC_PROLOGUE', l)]
    for l in body:
        for c in re.findall(r'\b(sub_[0-9A-F]+)\(ctx, base\)', l): calls.setdefault(c, set()).add(fn)
    if not locs: continue
    # blocs : coupure aux étiquettes et après goto/return
    blocks, labels, cur = [], {}, []
    for l in body:
        m = re.match(r'(loc_[0-9A-F]+):$', l)
        if m:
            if cur: blocks.append(cur)
            cur = []; labels[m[1]] = len(blocks)
        cur.append(l)
        if re.match(r'(goto |return;)', l):
            blocks.append(cur); cur = []
    if cur: blocks.append(cur)
    labels = {k: i for i, b in enumerate(blocks) for k in [b[0][:-1]] if b[0].startswith('loc_') and b[0].endswith(':')}
    succ = []
    for i, b in enumerate(blocks):
        s = {labels[t] for l in b for t in re.findall(r'goto (loc_[0-9A-F]+)', l) if t in labels}
        last = b[-1]
        if not re.match(r'(goto |return;)', last) and i + 1 < len(blocks): s.add(i + 1)
        succ.append(s)
    undef_in = [None] * len(blocks); undef_in[0] = set(locs)
    work = [0]; bad = set()
    while work:
        i = work.pop(); u = set(undef_in[i])
        for l in blocks[i]:
            m = WR.match(l)
            rhs = l.split('=', 1)[1] if m else l
            if l.startswith('PPC_STORE_U64(ctx.r1.u32 + -') or l.startswith('PPC_STORE_U32(ctx.r1.u32 + -'):
                pass  # sauvegarde de prologue
            else:
                bad |= set(TOK.findall(rhs)) & u
            if m: u.discard(m[1])
        for s in succ[i]:
            if undef_in[s] is None or not u <= undef_in[s]:
                undef_in[s] = (undef_in[s] or set()) | u; work.append(s)
    if bad: res[fn] = bad
out = set(res) | {c for f in res for c in calls.get(f, ())}
for fn in sorted(res):
    print("#", fn, " ".join(sorted(res[fn])), "<-", " ".join(sorted(calls.get(fn, ()))), file=sys.stderr)
print("registers_in_context = [")
for fn in sorted(out): print(f"    0x{fn[4:]},")
print("]")
