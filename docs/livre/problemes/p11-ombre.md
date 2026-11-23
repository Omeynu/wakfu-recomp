# Ombre absente sous le personnage

**Statut : Résolu** · [Retour aux problèmes](README.md)

L'ombre ronde sous le personnage n'apparaissait qu'avec le rendu natif.

## Journal

| Date | Observation |
|---|---|
| 20/11 | Écart entre rendu natif et émulation limité à cette ombre. |
| 20/11 | Cause : l'émulation n'effaçait qu'une bande de 32 pixels du masque d'ombres en mémoire EDRAM. C'est le rendu natif qui est juste. |

## Cause

Défaut de l'ancienne émulation.

## Solution

Aucune : disparu avec le rendu natif.

## Pour aller plus loin

- [natif.md](../../natif.md)
