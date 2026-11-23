# 64 shaders que XenosRecomp ne traduit pas

**Statut : Sans objet** · [Retour aux problèmes](README.md)

XenosRecomp plantait sur 64 shaders intégrés au jeu.

## Journal

| Date | Observation |
|---|---|
| 14/11 | 64 vertex shaders en échec sur 85 conteneurs intégrés. |
| 19/11 | Ce sont ceux du décodeur vidéo matériel de l'XDK ; le jeu décode ses vidéos sur le CPU et ne les utilise pas. |

## Cause

Shaders d'une bibliothèque non utilisée.

## Solution

Rien à faire ; les 95 shaders utiles sont traduits.

## Pour aller plus loin

- [natif-plan.md](../../natif-plan.md)
