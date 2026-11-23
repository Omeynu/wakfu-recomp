# Chutes de cadence sous Windows

**Statut : En cours** · [Retour aux problèmes](README.md)

Sous Windows, le jeu tombe par moments à 32-38 images/s sur un PC puissant, alors qu'un portable Linux bien plus lent tient 60.

## Journal

| Date | Observation |
|---|---|
| 22/11 | Journal Windows (RX 7900 XT) : médiane 45 images/s ; GPU 0,7 ms et fil de rendu 2,6 ms par image : presque inactifs. |
| 22/11 | Journaux Linux (portable Vega 8) : médiane 60 images/s avec GPU 6 ms et fil de rendu 4 à 8 ms. |
| 23/11 | Nouveau journal Windows : médiane 59,4, 48 % du temps à 60, chutes à 32-38 ; pendant les chutes, GPU 1,5 ms et fil de rendu 4 ms ; son et textures normaux. |
| 23/11 | Hypothèse : la synchro verticale émulée attend 16,7 ms avec le minuteur de Windows, de résolution 15,6 ms par défaut ; des réveils en retard font rater la synchro (images de 33,3 ms). |
| 23/11 | Correctif 0.1.2 : minuteur haute résolution ; ligne `[vblank]` (retard des réveils toutes les 10 s). Linux : 0,06 ms en moyenne ; Windows sous Wine : 0,57 ms, 0 au-delà de 2 ms. |
| à venir | Journal d'un vrai PC Windows avec la 0.1.2 : lignes `[vblank]` et `[cadence]`. |

## Cause

Probable : résolution du minuteur de Windows. À confirmer.

## Solution

`platform::SleepUntil` (minuteur haute résolution) dans `runtime/src/platform.cpp`, utilisé par la synchro de `kernel/gpu.cpp`.
