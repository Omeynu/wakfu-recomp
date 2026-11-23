# 52 instructions PowerPC non traduites

**Statut : Résolu** · [Retour aux problèmes](README.md)

Au départ, 7 868 instructions de 695 fonctions n'étaient pas traduites par XenonRecomp.

## Journal

| Date | Observation |
|---|---|
| 14/11 | 52 instructions ajoutées par un patch ; test différentiel contre QEMU : 63 cas, 0 échec ; `bdnzf` généralisé. |
| 15/11 | 4 instructions piégées découvertes en jeu (`vcmpbfp128`, `vupkd3d128`, `vpkd3d128`, `blrl`) ; test porté à 66 cas. |

## Cause

XenonRecomp a été écrit pour d'autres jeux, qui n'utilisent pas ces instructions.

## Solution

`patches/xenonrecomp-wakfu.patch` ; vérification : `tests/insn/run_tests.py`.

## Pour aller plus loin

- [methode.md](../../methode.md)
