# Tables de sauts introuvables

**Statut : Résolu** · [Retour aux problèmes](README.md)

XenonRecomp doit connaître les tables de sauts (`switch`) pour traduire les branchements indirects.

## Journal

| Date | Observation |
|---|---|
| 14/11 | XenonAnalyse n'en trouve qu'une sur 147 : le compilateur de ce jeu insère des `nop` qui cassent ses motifs. |
| 14/11 | Détecteur par émulation des instructions : 140 tables automatiques, 7 relues à la main ; 17 fonctions sans `.pdata` déclarées. |

## Cause

Motifs de code propres au compilateur de ce jeu, que les heuristiques prévues pour d'autres jeux ne voient pas.

## Solution

`scripts/find_switches.py` + `config/switches_manual.toml` ; à revérifier après toute nouvelle mise à jour du jeu.

## Pour aller plus loin

- [methode.md](../../methode.md)
