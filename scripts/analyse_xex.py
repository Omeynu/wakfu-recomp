#!/usr/bin/env python3
"""Analyse d'un XEX2 (en-têtes, bibliothèques XDK, imports, chaînes) -> Markdown.

Usage : python3 -I analyse_xex.py <xex_patché> <image.bin> <sortie_xexdump.txt> <dossier XenonUtils/xbox>

image.bin et le texte viennent de tools-src/xexdump. Attention : XenonUtils a déjà
remplacé les thunks d'import par des nops et inversé l'ordre des octets de chaque
enregistrement d'import dans image.bin ; on lit donc les enregistrements en little-endian.
"""
import collections
import os
import re
import struct
import sys

HEADER_NAMES = {
    0x2FF: 'RESOURCE_INFO', 0x3FF: 'FILE_FORMAT_INFO', 0x5FF: 'DELTA_PATCH_DESCRIPTOR',
    0x405: 'BASE_REFERENCE', 0x80FF: 'BOUNDING_PATH', 0x8105: 'DEVICE_ID',
    0x10001: 'ORIGINAL_BASE_ADDRESS', 0x10100: 'ENTRY_POINT', 0x10201: 'IMAGE_BASE_ADDRESS',
    0x103FF: 'IMPORT_LIBRARIES', 0x18002: 'CHECKSUM_TIMESTAMP', 0x18102: 'ENABLED_FOR_CALLCAP',
    0x18200: 'ENABLED_FOR_FASTCAP', 0x183FF: 'ORIGINAL_PE_NAME', 0x200FF: 'STATIC_LIBRARIES',
    0x20104: 'TLS_INFO', 0x20200: 'DEFAULT_STACK_SIZE', 0x20301: 'DEFAULT_FILESYSTEM_CACHE_SIZE',
    0x20401: 'DEFAULT_HEAP_SIZE', 0x28002: 'PAGE_HEAP_SIZE_AND_FLAGS', 0x30000: 'SYSTEM_FLAGS',
    0x40006: 'EXECUTION_INFO', 0x40201: 'TITLE_WORKSPACE_SIZE', 0x40310: 'GAME_RATINGS',
    0x40404: 'LAN_KEY', 0x405FF: 'XBOX360_LOGO', 0x406FF: 'MULTIDISC_MEDIA_IDS',
    0x407FF: 'ALTERNATE_TITLE_IDS', 0x40801: 'ADDITIONAL_TITLE_MEMORY', 0xE10402: 'EXPORTS_BY_NAME',
}
ENCRYPTION = {0: 'aucun', 1: 'AES (clé retail/devkit)'}
COMPRESSION = {0: 'aucune', 1: 'basique (blocs + zéros)', 2: 'normale (LZX)', 3: 'delta'}

KEYWORDS = ['Lua', 'lua_', 'LUA_', 'Havok', 'hk', 'XAudio2', 'XACT', 'XMA', 'xma', 'XNet', 'XLive',
            'XSocket', 'Direct3D', 'D3D', 'XUI', 'Xui', 'Bink', 'WMV', 'Scaleform', 'PhysX',
            'Box2D', 'boost', 'zlib', 'ogg', 'Vorbis', 'Wwise', 'FMOD', 'Ankama', 'Wakfu']


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def load_table(path):
    t = {}
    for m in re.finditer(r'XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)', open(path).read()):
        t[int(m.group(1), 16)] = (m.group(2), m.group(3))
    return t


def main():
    xex_path, img_path, dump_path, tables = sys.argv[1:5]
    x = open(xex_path, 'rb').read()
    img = open(img_path, 'rb').read()
    dump = open(dump_path).read().splitlines()
    base = int(dump[0].split()[1], 16)
    sections = [l.split() for l in dump if l.startswith('section ')]
    imports_named = {int(l.split()[1], 16): l.split()[2] for l in dump if l.startswith('import ')}

    out = []
    p = out.append
    assert x[:4] == b'XEX2', 'pas un XEX2'
    flags, pe_off, _, sec_off, count = struct.unpack_from('>5I', x, 4)
    p('## En-tête XEX2\n')
    p('- Drapeaux module : `%#x` ; données PE à `%#x` ; infos de sécurité à `%#x`' % (flags, pe_off, sec_off))
    image_size = be32(x, sec_off + 4)
    load_addr = be32(x, sec_off + 0x110)
    p('- Taille de l\'image : `%#x` (%d Kio) ; adresse de chargement : `%#x`' % (image_size, image_size // 1024, load_addr))
    hdr = {}
    for i in range(count):
        k, v = struct.unpack_from('>II', x, 0x18 + i * 8)
        hdr[k] = v
    p('\n### En-têtes optionnels (%d)\n' % count)
    p('| Clé | Nom | Valeur / décalage |')
    p('|---|---|---|')
    for k, v in hdr.items():
        p('| `%#08x` | %s | `%#x` |' % (k, HEADER_NAMES.get(k, '?'), v))

    def data(k):
        return x[hdr[k]:] if k in hdr else None

    ffi = data(0x3FF)
    if ffi:
        enc, comp = struct.unpack_from('>HH', ffi, 4)
        p('\n- Chiffrement : **%s** ; compression : **%s**' % (ENCRYPTION.get(enc, enc), COMPRESSION.get(comp, comp)))
    for k, label in ((0x10100, 'Point d\'entrée'), (0x10201, 'Adresse de base'), (0x20200, 'Pile par défaut'),
                     (0x20401, 'Tas par défaut'), (0x40201, 'Title workspace')):
        if k in hdr:
            p('- %s : `%#x`' % (label, hdr[k]))
    pe = data(0x183FF)
    if pe:
        p('- Nom PE d\'origine : `%s`' % pe[4:be32(pe, 0)].split(b'\0')[0].decode('ascii', 'replace'))
    ei = data(0x40006)
    if ei:
        media, ver, base_ver, tid = struct.unpack_from('>IIII', ei, 0)
        p('- Title ID : `%08X` ; version `%#x` (base `%#x`) ; disque %d/%d' % (tid, ver, base_ver, ei[0x12], ei[0x13]))

    p('\n### Sections\n')
    p('| Nom | Adresse | Taille | Type |')
    p('|---|---|---|---|')
    for s in sections:
        p('| `%s` | `%s` | `%s` | %s |' % (s[1], s[2], s[3], s[4]))

    sl = data(0x200FF)
    if sl:
        p('\n## Bibliothèques XDK liées statiquement\n')
        p('| Bibliothèque | Version |')
        p('|---|---|')
        n = (be32(sl, 0) - 4) // 16
        for i in range(n):
            e = sl[4 + i * 16:20 + i * 16]
            name = e[:8].split(b'\0')[0].decode('ascii', 'replace')
            ma, mi, bu, qfe = struct.unpack_from('>HHHH', e, 8)
            p('| %s | %d.%d.%d.%d |' % (name, ma, mi, bu, qfe & 0xFF))

    # --- imports ---
    tbl = {'xboxkrnl.exe': load_table(os.path.join(tables, 'xboxkrnl_table.inc')),
           'xam.xex': load_table(os.path.join(tables, 'xam_table.inc'))}
    il = data(0x103FF)
    libs = []
    if il:
        str_size, nlib = struct.unpack_from('>II', il, 4)
        names, o = [], 12
        while len(names) < nlib:
            s = il[o:].split(b'\0')[0].decode('ascii')
            names.append(s)
            o += (len(s) + 1 + 3) & ~3
        o = 12 + str_size
        for _ in range(nlib):
            size = be32(il, o)
            ver, ver_min = be32(il, o + 0x1C), be32(il, o + 0x20)
            name_idx, n = struct.unpack_from('>HH', il, o + 0x24)
            recs = [be32(il, o + 0x28 + 4 * j) for j in range(n)]
            libs.append((names[name_idx], ver, ver_min, recs))
            o += size

    # fréquence : appels « bl » vers chaque thunk dans .text
    calls = collections.Counter()
    for s in sections:
        if s[4] != 'code':
            continue
        a, sz = int(s[2], 16), int(s[3], 16)
        for off in range(a - base, a - base + sz - 3, 4):
            ins = be32(img, off)
            if ins >> 26 == 18 and ins & 3 == 1:     # bl (relatif)
                li = ins & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = base + off + li
                if t in imports_named:
                    calls[t] += 1
    by_name = collections.Counter()
    for t, c in calls.items():
        by_name[imports_named[t]] += c

    p('\n## Imports\n')
    rows = []
    for lib, ver, vmin, recs in libs:
        t = tbl.get(lib, {})
        ords = collections.OrderedDict()
        for r in recs:
            v = struct.unpack_from('<I', img, r - base)[0]
            if v >> 24 == 0 and v != 0x60:      # « variable » (porte l'ordinal) ; 0x60 = thunk réécrit en nop (60000000 lu en LE)
                ords[v & 0xFFFF] = True
        p('- **%s** v%d.%d.%d.%d (min %d.%d.%d.%d) : %d imports' % (
            lib, ver >> 28, (ver >> 24) & 0xF, (ver >> 8) & 0xFFFF, ver & 0xFF,
            vmin >> 28, (vmin >> 24) & 0xF, (vmin >> 8) & 0xFFFF, vmin & 0xFF, len(ords)))
        for o_ in ords:
            name, kind = t.get(o_, ('ordinal_%d' % o_, '?'))
            rows.append((by_name.get('__imp__' + name, 0), lib, o_, name, kind))
    rows.sort(key=lambda r: (-r[0], r[1], r[2]))
    p('\n| Appels `bl` | Module | Ordinal | Nom | Type |')
    p('|---:|---|---:|---|---|')
    for c, lib, o_, name, kind in rows:
        p('| %d | %s | %d (`%#x`) | `%s` | %s |' % (c, lib, o_, o_, name, kind.replace('k', '', 1)))
    p('\n(Comptage : appels directs `bl` vers le thunk dans le code. Les variables et appels '
      'indirects ne sont pas comptés.)')

    # --- chaînes ---
    p('\n## Indices dans les chaînes de l\'image\n')
    strs = re.findall(rb'[\x20-\x7e]{6,}', img)
    for kw in KEYWORDS:
        hits = sorted({s.decode() for s in strs if kw.encode() in s})
        if hits:
            p('- **%s** (%d) : %s' % (kw, len(hits), ', '.join('`%s`' % h.replace('`', "'")[:70] for h in hits[:6])))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
