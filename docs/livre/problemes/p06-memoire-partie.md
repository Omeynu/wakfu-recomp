# Mémoire épuisée au lancement d'une partie

**Statut : Résolu** · [Retour aux problèmes](README.md)

« New Game » ne lançait jamais la partie.

## Journal

| Date | Observation |
|---|---|
| 16/11 | Le compilateur de shaders du jeu échoue faute de mémoire, l'initialisation de la partie avorte. |
| 16/11 | `NtAllocateVirtualMemory` lisait le drapeau `MEM_HEAP` (0x40000000) comme « pages de 16 Mo » : chaque allocation alignée sur 16 Mo épuisait l'espace virtuel. |

## Cause

Drapeau Xbox mal interprété.

## Solution

Correction de l'allocation ; partie lancée, scripts Lua chargés.
