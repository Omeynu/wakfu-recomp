# comparer-natif.py <captures> <référence> : compare chaque image_N.png de <captures> (WAKFU_CAPTURE) à la même image
# de <référence> (natif_N.png s'il existe, sinon image_N.png) : même parcours (entrées par numéro d'image), autre build.
# Capture k fois plus grande (echelle_rendu = k) : réduite à la taille de la référence (moyenne des blocs kxk).
# Critère : écart moyen <= 2/255 et pixels dont un canal s'écarte de plus de 8/255 <= 0,1 % de l'image ; aussi mesuré
# sans la dernière ligne ni la dernière colonne (bord d'1 px).
import os, re, sys
import numpy as np
from PIL import Image

def lire(p): return np.asarray(Image.open(p).convert('RGB'), dtype=np.int16)

def mesure(a, b):
    d = np.abs(a - b)
    return d.mean(), (d.max(axis=2) > 8).mean() * 100

def main(captures, reference):
    ns = sorted(int(m.group(1)) for f in os.listdir(captures) if (m := re.fullmatch(r'image_(\d+)\.png', f)))
    echecs = identiques = 0
    for n in ns:
        r = next((p for p in (f'{reference}/natif_{n}.png', f'{reference}/image_{n}.png') if os.path.exists(p)), None)
        if not r: continue
        a, b = lire(f'{captures}/image_{n}.png'), lire(r)
        k = a.shape[0] // b.shape[0]   # définition interne xk (echelle_rendu) : réduite par moyenne des blocs kxk
        if k > 1 and a.shape == (b.shape[0] * k, b.shape[1] * k, 3):
            a = np.rint(a.reshape(b.shape[0], k, b.shape[1], k, 3).mean(axis=(1, 3))).astype(np.int16)
        if a.shape != b.shape: print(f'{n} : tailles {a.shape} {b.shape}  ECHEC'); echecs += 1; continue
        m, p = mesure(a, b); mi, pi = mesure(a[:-1, :-1], b[:-1, :-1])
        ok = m <= 2 and p <= 0.1
        echecs += not ok
        identiques += not np.any(a != b)
        print(f'{n} : ecart moyen {m:.3f}/255, > 8/255 {p:.3f} % ; sans bord {mi:.3f}/255, {pi:.3f} %  {"OK" if ok else "ECHEC"}')
    print(f'{len(ns)} captures, {identiques} identiques, {echecs} hors critère')
    return echecs

if __name__ == '__main__':
    assert mesure(np.zeros((4, 4, 3), np.int16), np.full((4, 4, 3), 9, np.int16)) == (9.0, 100.0)   # auto-test
    sys.exit(1 if main(sys.argv[1], sys.argv[2]) else 0)
