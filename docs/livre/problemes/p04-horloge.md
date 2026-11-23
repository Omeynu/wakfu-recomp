# Scripts de la zone 1 bloqués (horloge faussée)

**Statut : Résolu** · [Retour aux problèmes](README.md)

Après l'introduction, l'image restait figée : les scripts Lua de la zone 1 n'avançaient plus.

## Journal

| Date | Observation |
|---|---|
| 17/11 | L'instruction `mftb` (base de temps) lisait le compteur du processeur hôte, ≈ 2,9 GHz, alors que le jeu annonce et attend 50 MHz. |
| 17/11 | `mftb` relié à une horloge stable à 50 MHz ; le script de la zone 1 va jusqu'au bout. |

## Cause

Traduction de `mftb` par `__rdtsc()` brut dans XenonRecomp.

## Solution

Patch XenonRecomp (`PPC_QUERY_TIMEBASE`) et horloge du runtime (`indirect.h`).
