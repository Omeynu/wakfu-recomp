# Son haché et vidéos au ralenti

**Statut : Résolu** · [Retour aux problèmes](README.md)

Retour d'un joueur sur portable : son haché partout, cinématiques à ≈ 6 images/s.

## Journal

| Date | Observation |
|---|---|
| 21/11 | Le jeu a 3 ou 4 sources sonores ; le pilote audio émulé mettait leurs trames bout à bout au lieu de les additionner. |
| 21/11 | Le moteur son tournait 3 à 4 fois trop lentement, et les vidéos, calées sur lui, aussi. |
| 21/11 | Trames additionnées ; vérifié à 60, 25 et 8 images/s : ≈ 1 875 tours / 10 s, aucun manque. |
| 22/11 | Journaux du joueur : son correct, ligne `[son]` stable. |

## Cause

Mixage audio faux, invisible sur la machine de développement (sans carte son).

## Solution

Mixage dans `kernel/xam.cpp` ; bilan `[son]` dans le journal toutes les 10 s.

## Pour aller plus loin

- [natif.md](../../natif.md)
