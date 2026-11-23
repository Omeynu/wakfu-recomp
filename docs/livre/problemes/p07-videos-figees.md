# Vidéos figées

**Statut : Résolu** · [Retour aux problèmes](README.md)

Le lecteur vidéo de l'interface (XUI) se figeait une fois sur trois environ.

## Journal

| Date | Observation |
|---|---|
| 17/11 | `GetTickCount` du jeu restait à 0 : le compteur de temps partagé du noyau (`KeTimeStampBundle`) n'était jamais mis à jour. |
| 17/11 | Mis à jour toutes les millisecondes : 10 lancements sur 10 avec vidéos, contre 4 sur 6 avant. |

## Cause

Variable du noyau Xbox non émulée.

## Solution

Mise à jour périodique du compteur dans le noyau émulé.
