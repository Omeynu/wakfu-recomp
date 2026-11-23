# Cadence sur GPU intégré

**Statut : En cours** · [Retour aux problèmes](README.md)

Sur un portable (Ryzen 7 5700U, Radeon Vega 8), la cadence descendait sous 60 images/s.

## Journal

| Date | Observation |
|---|---|
| 21/11 | 40 à 56 images/s, GPU 15 à 17 ms par image. |
| 21/11 | Resolves et effacements du GPU optimisés, MSAA coupé par défaut sur GPU intégré, mixage du son corrigé (fiche p08). |
| 22/11 | Trois parties : médiane 60 images/s, GPU 5,5 à 6,7 ms ; baisses ponctuelles à 33-40 ; fil de rendu jusqu'à 14 ms dans les scènes chargées. |
| en cours | Réduction du temps CPU du fil de rendu. |

## Cause

Fil de rendu chargé dans certaines scènes.

## Solution

En cours.

## Pour aller plus loin

- [natif.md](../../natif.md)
