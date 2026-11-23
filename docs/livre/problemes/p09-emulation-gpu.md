# L'émulation du GPU plafonnait vers 30 images/s

**Statut : Résolu** · [Retour aux problèmes](README.md)

La première version émulait le GPU de la Xbox 360 (flux de commandes PM4).

## Journal

| Date | Observation |
|---|---|
| 16/11 | 6 à 18 images/s en zone 1. |
| 18/11 | Optimisations successives : jusqu'à 46-54 images/s, mais plafond structurel : CPU et GPU s'attendent ≈ 9 fois par image. |
| 18/11 | Décision : intercepter Direct3D, lié dans le jeu sans symboles, ≈ 25 fonctions identifiées une à une. |
| 20/11 | Rendu natif Vulkan par défaut ; émulation retirée (≈ 5 400 lignes en moins). Zone 1 : 60 images/s verrouillées, GPU 3,8 ms. |

## Cause

Architecture : le processeur de commandes émulé attendait la fin du travail du GPU à chaque barrière.

## Solution

Rendu natif (`runtime/src/natif/`).

## Pour aller plus loin

- [natif.md](../../natif.md)
- [natif-plan.md](../../natif-plan.md)
