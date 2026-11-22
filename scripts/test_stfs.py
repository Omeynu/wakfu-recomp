#!/usr/bin/env python3
"""Test de stfs_extract : formules d'adressage + extraction réelle du paquet TU.

Usage : python3 -I test_stfs.py <paquet_TU> [dossier_tmp]
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stfs_extract as se  # noqa: E402


def test_addressing():
    s = se.Stfs.__new__(se.Stfs)
    # Disposition attendue (une table par niveau) : L0#0, données 0..169, L1, L0#1, données 170...
    assert s.data_backing(0) == 1
    assert s.data_backing(169) == 170
    assert s.l0_backing(170) == 0xAC
    assert s.data_backing(170) == 0xAD
    # Chaque bloc de données est distinct de toute table L0
    used = set()
    for b in range(0, 3 * se.L1, 37):
        d = s.data_backing(b)
        assert d != s.l0_backing(b) and d not in used
        used.add(d)


def test_tu(pkg, tmp):
    s = se.Stfs(pkg)
    files = dict(s.extract(tmp))
    xexp = [f for f in files if f.lower().endswith('.xexp')]
    assert xexp, 'aucun .xexp dans la TU : %r' % files
    with open(os.path.join(tmp, xexp[0]), 'rb') as f:
        assert f.read(4) == b'XEX2'
    # Un bloc corrompu doit être détecté
    s.m = bytearray(s.m)
    s.m[s.base + s.data_backing(s.ft_block) * se.BLOCK] ^= 0xFF
    try:
        s.entries()
    except se.StfsError:
        pass
    else:
        raise AssertionError('corruption non détectée')


if __name__ == '__main__':
    test_addressing()
    tmp = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()
    test_tu(sys.argv[1], tmp)
    print('OK')
