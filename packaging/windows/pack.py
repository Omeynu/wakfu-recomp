#!/usr/bin/env python3
"""Archive de l'installateur Windows (lue par tools-src/wakfu-installer).

  pack.py <arbre> <fichier_id>            -> flux non compressé sur stdout, empreinte (16 hex) dans <fichier_id>
  pack.py --append <amorce.exe> <archive.zst> <id> <sortie.exe>

Flux : "WKPK0001", puis enregistrements triés (reproductible, sans dates ni droits) :
  'D' u16 lg chemin           dossier
  'F' u16 lg u64 taille chemin données
  'E'                         fin
Chemins UTF-8 relatifs, séparateur '/'. Entiers petit-boutistes.
Fin de l'exécutable : amorce, archive zstd, puis 48 octets : "WAKFUPK1" u64 décalage u64 taille id(16 hex) 8 octets nuls.
"""
import hashlib
import os
import struct
import sys

MAGIC = b'WKPK0001'


def pack(root, id_path):
    out = sys.stdout.buffer
    h = hashlib.sha256()

    def emit(b):
        h.update(b)
        out.write(b)

    emit(MAGIC)
    for d, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(d, root)
        if rel != '.':
            p = rel.replace(os.sep, '/').encode()
            emit(b'D' + struct.pack('<H', len(p)) + p)
        for f in sorted(files):
            full = os.path.join(d, f)
            if os.path.islink(full) or not os.path.isfile(full):
                raise SystemExit('fichier non ordinaire refusé : ' + full)
            p = os.path.relpath(full, root).replace(os.sep, '/').encode()
            size = os.path.getsize(full)
            emit(b'F' + struct.pack('<HQ', len(p), size) + p)
            with open(full, 'rb') as fh:
                while chunk := fh.read(1 << 20):
                    emit(chunk)
    emit(b'E')
    with open(id_path, 'w') as fh:
        fh.write(h.hexdigest()[:16])


def append(stub, payload, ident, dest):
    assert len(ident) == 16
    with open(dest, 'wb') as out:
        with open(stub, 'rb') as fh:
            out.write(fh.read())
        off = out.tell()
        with open(payload, 'rb') as fh:
            while chunk := fh.read(1 << 20):
                out.write(chunk)
        size = out.tell() - off
        out.write(b'WAKFUPK1' + struct.pack('<QQ', off, size) + ident.encode() + bytes(8))


if __name__ == '__main__':
    if sys.argv[1] == '--append':
        append(*sys.argv[2:6])
    else:
        pack(sys.argv[1], sys.argv[2])
