#!/usr/bin/env python3
"""Extracteur STFS minimal (paquets LIVE/PIRS/CON Xbox 360), d'après le wiki Free60.

Usage : python3 -I stfs_extract.py <paquet> <dossier_sortie>

Chaque bloc de données lu est vérifié contre son SHA-1 de la table de hachage
de niveau 0 : une erreur d'adressage ou un paquet abîmé fait échouer l'extraction.
"""
import hashlib
import mmap
import os
import struct
import sys

BLOCK = 0x1000
L0 = 0xAA        # blocs par table de niveau 0
L1 = 0x70E4      # blocs couverts par une table de niveau 1


class StfsError(Exception):
    pass


class Stfs:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        m = self.m
        if m[0:4] not in (b'LIVE', b'PIRS', b'CON '):
            raise StfsError('magic STFS inconnu : %r' % m[0:4])
        header_size = struct.unpack('>I', m[0x340:0x344])[0]
        self.content_type = struct.unpack('>I', m[0x344:0x348])[0]
        vd = m[0x379:0x379 + 0x24]
        if vd[0] != 0x24:
            raise StfsError('descripteur de volume STFS attendu (taille 0x24), lu %#x' % vd[0])
        self.shift = 0 if vd[2] & 1 else 1   # 1 table par niveau (female) ou 2 (male)
        if self.shift:
            raise StfsError('paquet « male » (2 tables par niveau) non géré')
        self.ft_count = struct.unpack('<H', vd[3:5])[0]
        self.ft_block = int.from_bytes(vd[5:8], 'little')
        self.alloc = struct.unpack('>I', vd[0x1C:0x20])[0]
        self.base = (header_size + 0xFFF) & ~0xFFF

    # --- adressage (formules Free60/Velocity, cas une table par niveau) ---
    def data_backing(self, b):
        r = (b + L0) // L0 + b
        if b < L0:
            return r
        r += (b + L1) // L1
        return r if b < L1 else r + 1

    def l0_backing(self, b):
        if b < L0:
            return 0
        n = (b // L0) * (L0 + 1) + (b // L1) + 1
        return n if b < L1 else n + 1

    def hash_entry(self, b):
        off = self.base + self.l0_backing(b) * BLOCK + (b % L0) * 0x18
        e = self.m[off:off + 0x18]
        return e[:0x14], e[0x14], int.from_bytes(e[0x15:0x18], 'big')

    def block(self, b):
        if b >= self.alloc:
            raise StfsError('bloc %d hors du paquet (%d alloués)' % (b, self.alloc))
        off = self.base + self.data_backing(b) * BLOCK
        data = self.m[off:off + BLOCK]
        sha, _, _ = self.hash_entry(b)
        if hashlib.sha1(data).digest() != sha:
            raise StfsError('SHA-1 incorrect pour le bloc %d' % b)
        return data

    def chain(self, start, count, consecutive):
        b = start
        for i in range(count):
            yield b
            b = start + i + 1 if consecutive else self.hash_entry(b)[2]

    # --- table des fichiers ---
    def entries(self):
        out = []
        for b in self.chain(self.ft_block, self.ft_count, False):
            data = self.block(b)
            for i in range(0, BLOCK, 0x40):
                e = data[i:i + 0x40]
                flags = e[0x28]
                n = flags & 0x3F
                if n == 0:
                    return out
                out.append({
                    'name': e[:n].decode('ascii', 'replace'),
                    'dir': bool(flags & 0x80),
                    'consecutive': bool(flags & 0x40),
                    'blocks': int.from_bytes(e[0x29:0x2C], 'little'),
                    'start': int.from_bytes(e[0x2F:0x32], 'little'),
                    'parent': struct.unpack('>h', e[0x32:0x34])[0],
                    'size': struct.unpack('>I', e[0x34:0x38])[0],
                })
        return out

    def extract(self, outdir):
        ents = self.entries()

        def path(i):
            parts = []
            while i != -1:
                if not 0 <= i < len(ents) or len(parts) > 64:
                    raise StfsError('chemin invalide dans la table des fichiers')
                name = ents[i]['name']
                if name in ('', '.', '..') or '/' in name or '\\' in name:
                    raise StfsError('nom de fichier refusé : %r' % name)
                parts.append(name)
                i = ents[i]['parent']
            return os.path.join(outdir, *reversed(parts))

        listing = []
        for i, e in enumerate(ents):
            p = path(i)
            if e['dir']:
                os.makedirs(p, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(p), exist_ok=True)
            need = (e['size'] + BLOCK - 1) // BLOCK
            if need > e['blocks']:
                raise StfsError('%s : taille %d > %d blocs' % (p, e['size'], e['blocks']))
            left = e['size']
            with open(p, 'wb') as o:
                for b in self.chain(e['start'], need, e['consecutive']):
                    d = self.block(b)
                    o.write(d[:min(left, BLOCK)])
                    left -= BLOCK
            if os.path.getsize(p) != e['size']:
                raise StfsError('%s : taille écrite incorrecte' % p)
            listing.append((os.path.relpath(p, outdir), e['size']))
        return listing


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    s = Stfs(sys.argv[1])
    for rel, size in s.extract(sys.argv[2]):
        print('%12d  %s' % (size, rel))


if __name__ == '__main__':
    main()
