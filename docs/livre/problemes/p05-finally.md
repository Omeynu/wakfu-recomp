# Blocages avec le C++ optimisé

**Statut : Résolu** · [Retour aux problèmes](README.md)

Avec les optimisations de XenonRecomp, le jeu se bloquait sur la sauvegarde.

## Journal

| Date | Observation |
|---|---|
| 17/11 | Deux fils bloqués dans `RtlEnterCriticalSection`. |
| 17/11 | Les blocs `__finally` de MSVC lisent les registres de leur fonction parente ; optimisés en variables locales, ils valaient 0. |
| 18/11 | Optimisations finalement désactivées : elles abîmaient aussi la vidéo de l'écran-titre. |

## Cause

Hypothèse de XenonRecomp (registres locaux à chaque fonction) fausse pour ces 167 fonctions.

## Solution

Option `registers_in_context` (`scripts/find_context_functions.py`) ; optimisations coupées (`methode.md` §10).

## Pour aller plus loin

- [methode.md](../../methode.md)
