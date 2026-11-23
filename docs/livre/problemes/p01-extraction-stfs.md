# Pas d'extracteur de paquets XBLA sous Linux

**Statut : Résolu** · [Retour aux problèmes](README.md)

Le jeu est livré en paquets STFS (format Xbox Live Arcade) : il faut en extraire `default.xex` et la mise à jour.

## Journal

| Date | Observation |
|---|---|
| 14/11 | Aucun outil Linux maintenu ne lit les paquets STFS « female » du jeu. |
| 14/11 | Extracteur écrit pour le projet ; chaque bloc vérifié par SHA-1 ; testé sur la mise à jour 2 (4 677 + 49 fichiers). |
| 16/11 | Repris en C++ dans `wakfu-prepare`, qui lit aussi les images ISO/XISO et les dossiers extraits. |

## Cause

Pas d'outil existant pour ce format.

## Solution

`scripts/stfs_extract.py`, puis `tools-src/wakfu-prepare` (détection par contenu, contrôle SHA-1 de l'image).

## Pour aller plus loin

- [methode.md](../../methode.md)
