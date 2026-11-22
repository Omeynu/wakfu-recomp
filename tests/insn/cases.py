"""Cas de test des instructions ajoutées à XenonRecomp (patches/xenonrecomp-wakfu.patch).

Chaque cas : nom, corps assembleur (registres r3-r12, f1-f4, v1-v6 seulement), et pour les
VMX128 (inconnues de binutils et de QEMU) un corps « recomp » en .long + un corps « qemu »
équivalent en VMX standard. `wide` = exécuter sous qemu-ppc64 (instructions 64 bits).
`decode` = mnémonique attendu dans le C++ généré (vérifie l'encodage .long).
"""
import random

MEM = 0x10080000          # zone mémoire de test (64 octets), même adresse des deux côtés


def vx128(op, xop, mask, vd, va, vb):
    return (op << 26) | (xop & mask) | (vd << 21) | (va << 16) | (vb << 11)


def case(name, body, *, qemu=None, wide=False, decode=None, inputs=None, vcompare=None):
    return dict(name=name, body=body, qemu=qemu or body, wide=wide, decode=decode,
                inputs=inputs, vcompare=vcompare)


E32 = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE]
CR_SAMPLES = [0x00000000, 0xFFFFFFFF, 0x24800042, 0x9A3C5E71]


def gpr_inputs(rng, n=8, ca=True):
    out = []
    for i in range(n):
        st = dict(r={k: rng.getrandbits(32) for k in range(3, 13)},
                  cr=rng.choice(CR_SAMPLES) ^ rng.getrandbits(32) * (i % 2),
                  xer=(0x20000000 if ca and i % 2 else 0), ctr=rng.getrandbits(32))
        if i < len(E32):
            st['r'][4] = E32[i]
            st['r'][5] = E32[(i * 3 + 1) % len(E32)]
        out.append(st)
    return out


def single_as_double(rng):
    """Double exactement représentable en simple précision (cas réel : sortie de frsp/fadds).
    stfs tronque alors qu'XenonRecomp arrondit (float()) ; les deux coïncident sur ces valeurs."""
    import struct
    f = struct.unpack('>f', struct.pack('>I', rng.getrandbits(32) & 0x7F7FFFFF | 0x00800000))[0]
    return struct.unpack('>Q', struct.pack('>d', f))[0]


def mem_inputs(rng, n=6, regs=None):
    out = []
    for i in range(n):
        st = dict(r={k: rng.getrandbits(32) for k in range(3, 13)}, cr=0, xer=0, ctr=0,
                  mem=bytes(rng.getrandbits(8) for _ in range(64)),
                  f={k: single_as_double(rng) for k in range(1, 5)})
        st['r'].update(regs(i) if regs else {})
        out.append(st)
    return out


H_EDGE = [0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x8001, 0x00FF, 0x0F0F]


def vec_inputs(rng, n=6, elem=16):
    out = []
    for i in range(n):
        v = {}
        for k in range(1, 7):
            b = bytearray(rng.getrandbits(8) for _ in range(16))
            if i % 2 == 0 and elem == 16:
                for j in range(8):
                    if rng.random() < 0.5:
                        b[2 * j:2 * j + 2] = rng.choice(H_EDGE).to_bytes(2, 'big')
            if i % 2 == 0 and elem == 32:
                for j in range(4):
                    if rng.random() < 0.5:
                        b[4 * j:4 * j + 4] = rng.choice(E32).to_bytes(4, 'big')
            v[k] = bytes(b)
        out.append(dict(r={}, cr=rng.getrandbits(32), xer=0, ctr=0, v=v))
    return out


def float_vec_inputs(rng, n=6):
    import struct
    pool = [0.0, -0.0, 1.5, -1.5, 3e9, 4.3e9, 5e9, -7.0, 65535.7, 1e-40, float('inf'),
            float('-inf'), float('nan'), 2147483648.0, 123.456, 0.999]
    out = []
    for i in range(n):
        v = {k: b''.join(struct.pack('>f', rng.choice(pool) if rng.random() < 0.7 else rng.uniform(-1e10, 1e10))
                         for _ in range(4)) for k in range(1, 7)}
        out.append(dict(r={}, cr=0, xer=0, ctr=0, v=v))
    return out


def all_cases():
    rng = random.Random(360)
    C = []
    # --- scalaire ---
    for m in ('eqv', 'eqv.', 'addc', 'addc.'):
        C.append(case(m.replace('.', '_rc'), [f'{m} r3,r4,r5'], inputs=gpr_inputs(rng)))
    for m in ('addme', 'addme.', 'subfze', 'subfze.'):
        C.append(case(m.replace('.', '_rc'), [f'{m} r3,r4'], inputs=gpr_inputs(rng)))
    C.append(case('cror', ['cror 2,5,26', 'cror 27,0,31'], inputs=gpr_inputs(rng)))
    C.append(case('crorc', ['crorc 0,9,27', 'crorc 30,30,1'], inputs=gpr_inputs(rng)))
    for m, bi in (('bdzf', 25), ('bdzf', 2), ('bdnzt', 13), ('bdnzt', 26), ('bdnzf', 24), ('bdnzf', 7)):
        ins = gpr_inputs(rng)
        for k, st in enumerate(ins):
            st['ctr'] = [1, 2, 1, 2, 0x10, 1, 2, 1][k]
        C.append(case(f'{m}_{bi}', [f'{m} {bi},1f', 'li r3,1', 'blr', '1: li r3,2'], inputs=ins))
    # --- chargements / stores avec mise à jour ---
    base = lambda off: (lambda i: {4: MEM + off, 5: [0, 8, 16, 24, 32, 40][i]})
    for m, d in (('lhzu', 6), ('lhau', 10), ('sthu', 12), ('lfsu', 8), ('lfdu', 16), ('stfsu', 20), ('stfdu', 24)):
        reg = 'f1' if m.startswith(('lf', 'stf')) else 'r3'
        C.append(case(m, [f'{m} {reg},{d}(r4)'], inputs=mem_inputs(rng, regs=base(0))))
    for m in ('lbzux', 'lhzux', 'lwzux', 'stbux', 'sthux', 'lfsux', 'lfdux'):
        reg = 'f1' if 'f' in m[:3] else 'r3'
        C.append(case(m, [f'{m} {reg},r4,r5'], inputs=mem_inputs(rng, regs=base(0))))
    for m in ('ldux', 'stdux'):
        C.append(case(m, [f'{m} r3,r4,r5'], wide=True, inputs=mem_inputs(rng, regs=base(0))))
    C.append(case('lvehx', ['lvehx v1,r4,r5'], vcompare='lvehx',
                  inputs=mem_inputs(rng, regs=lambda i: {4: MEM, 5: [0, 2, 6, 14, 18, 30][i]})))
    # --- VMX élément par élément ---
    for m in ('vslh', 'vsrh', 'vsrah', 'vrlh', 'vsubshs', 'vmaxsh', 'vminsh', 'vavguh',
              'vcmpgtsh', 'vcmpgtsh.', 'vcmpequh', 'vcmpequh.', 'vcmpgtuh.'):
        C.append(case(m.replace('.', '_rc'), [f'{m} v1,v2,v3', f'{m} v4,v4,v5'], inputs=vec_inputs(rng)))
    for m in ('vsrab', 'vaddsbs', 'vsububm'):
        C.append(case(m, [f'{m} v1,v2,v3'], inputs=vec_inputs(rng, elem=8)))
    for m in ('vaddsws', 'vcmpgtsw', 'vcmpgtsw.', 'vpkswss', 'vpkshss'):
        C.append(case(m.replace('.', '_rc'), [f'{m} v1,v2,v3', f'{m} v4,v4,v5'],
                      inputs=vec_inputs(rng, elem=32 if 'w' in m[-3:] else 16)))
    for m in ('vcmpbfp', 'vcmpbfp.'):
        C.append(case(m.replace('.', '_rc'), [f'{m} v1,v2,v3', f'{m} v4,v4,v5'], inputs=float_vec_inputs(rng)))
    C.append(case('vcmpbfp128', ['.long 0x%08X' % vx128(6, 384, 0x3D0, 1, 2, 3)], qemu=['vcmpbfp v1,v2,v3'],
                  decode='vcmpbfp128', inputs=float_vec_inputs(rng)))
    C.append(case('vspltish', ['vspltish v1,-1', 'vspltish v2,15', 'vspltish v3,-16', 'vspltish v4,5'],
                  inputs=vec_inputs(rng, n=1)))
    # --- VMX128 : .long côté recomp, équivalent VMX côté QEMU ---
    M5, M3 = 0x3D0, 0x7F0
    for m, xop, eq in (('vpkswss128', 640, 'vpkswss'), ('vpkswus128', 704, 'vpkswus'),
                       ('vpkshss128', 512, 'vpkshss'), ('vpkuhus128', 832, 'vpkuhus')):
        C.append(case(m, ['.long 0x%08X' % vx128(5, xop, M5, 1, 2, 3)], qemu=[f'{eq} v1,v2,v3'],
                      decode=m, inputs=vec_inputs(rng, elem=32 if 'sw' in m else 16)))
    C.append(case('vsel128', ['.long 0x%08X' % vx128(5, 848, M5, 1, 2, 3)], qemu=['vsel v1,v2,v3,v1'],
                  decode='vsel128', inputs=vec_inputs(rng)))
    for u in (0, 4, 31):
        w = (6 << 26) | (624 & M3) | (1 << 21) | (u << 16) | (2 << 11)
        C.append(case(f'vcfpuxws128_{u}', ['.long 0x%08X' % w], qemu=[f'vctuxs v1,v2,{u}'],
                      decode='vcfpuxws128', inputs=float_vec_inputs(rng)))
    return C
