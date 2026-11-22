# Rapport — préparation de la recompilation d'Islands of Wakfu (2026-11-14)

**Résultat** : le C++ généré par XenonRecomp (patché) depuis le XEX + TU #2 **compile sans
avertissement** (Clang 19, `-O2`) et **se lie** : les seuls symboles manquants sont les 179 imports
noyau/XAM. Les 74 shaders du jeu passent XenosRecomp jusqu'au SPIR-V. Rien n'a été exécuté :
le runtime reste à écrire (voir « Travail restant »).

Détails de méthode : `docs/methode.md`. Analyse du binaire : `docs/analyse.md`.

## État des étapes

| Étape | État | Vérification |
|---|---|---|
| 0. Machine de test agrandie | Réussi | 8 cœurs, 12 Go |
| 1. Outils | Réussi | Clang 19.1.7, CMake 3.31.6 ; XenonRecomp/XenonAnalyse/XenosRecomp compilés et lancés ; `tools.lock` |
| 2. Récupération / extraction | Réussi | 2 fichiers, HTTP 200, tailles exactes (859 414 528 / 1 839 104) ; extracteur STFS maison avec SHA-1 par bloc + test ; `default.xex` et `default.xexp` au magic `XEX2` ; 4 677 + 49 fichiers |
| 3. Analyse du XEX | Réussi | TU appliquée par `XexPatcher` ; `docs/analyse.md` |
| 4. Config XenonRecomp | Réussi | 8 fonctions d'aide vérifiées mot à mot ; setjmp/longjmp ; 147 tables de sauts ; 17 fonctions sans `.pdata` ; 4 motifs de données |
| 5. Génération C++ | Réussi après patch | 0 avertissement (au départ : 7 868 instructions non traduites, 509 sauts hors fonction) |
| 6. Compilation / lien | Réussi | 60 fichiers, 93 Mo, 96 s en `-j6` (pic ≈ 2,4 Go en `-j4`), 0 avertissement ; lien : 179 manquants = 179 imports, 0 autre |
| 7. Shaders | Réussi (jeu) / partiel (XDK) | 74/74 shaders du jeu → HLSL + SPIR-V ; shaders intégrés au XEX : 21/85, 64 vertex shaders font planter XenosRecomp |
| 8. Rapport / livraison | Réussi | dépôt privé poussé ; release `ppc-20261114-9ace96b` (archive 5,5 Mo dans le registre de paquets générique, retéléchargée et comparée) |
| 9. Remise en état de la machine de test | Reportée (choix de l'utilisateur) | machine laissée agrandie, espace de travail conservé pour enchaîner sur le runtime |

## Ce qui a dû être corrigé ou ajouté

1. **Extraction STFS** : pas d'outil Linux maintenu → `scripts/stfs_extract.py` (paquets « female »
   seulement, chaque bloc vérifié par SHA-1), testé sur la TU.
2. **Tables de sauts** : XenonAnalyse n'en trouvait qu'**une** (motifs cassés par les `nop` de ce
   compilateur). Détecteur par émulation → 140 automatiques + 7 manuelles = 147.
3. **16 fonctions feuilles sans `.pdata`** contenant une table, et une 17ᵉ (« switch » en chaîne de
   `bdzf`) : coupées par l'analyse de XenonRecomp → déclarées dans `functions`.
4. **XenonRecomp ne traduisait pas 52 instructions** utilisées par le jeu (7 868 occurrences, 695
   fonctions). Patch `patches/xenonrecomp-wakfu.patch` + test différentiel contre QEMU
   (63 cas, 405 exécutions, 0 échec ; mutation détectée). `bdnzf` généralisé (l'original supposait
   le bit `eq`, faux pour ce jeu).
5. **Piège** : XenonRecomp n'efface pas les anciens `ppc_recomp.N.cpp` ; quand le nombre de fichiers
   baisse, un fichier périmé provoque des définitions en double. Toujours vider `out/ppc` avant.

## Imports à émuler (HLE)

179 fonctions (148 xboxkrnl + 43 xam, dont 12 variables côté données). Liste complète triée par
nombre d'appels directs dans `docs/analyse.md`. En tête :

| Appels `bl` | Import |
|---:|---|
| 271 | `RtlLeaveCriticalSection` |
| 213 | `RtlEnterCriticalSection` |
| 51 | `KeGetCurrentProcessType` |
| 38 | `RtlInitializeCriticalSection` |
| 15 / 13 | `KeTlsSetValue` / `KeTlsGetValue` |
| 12 | `ExGetXConfigSetting` |
| 11 / 11 | `MmGetPhysicalAddress` / `NtAllocateVirtualMemory` |
| 9 | `XMsgStartIORequest`, `ExRegisterTitleTerminateNotification`, `KeSetEvent`, `RtlInitAnsiString` |
| 8 | `ObDereferenceObject`, `swprintf` |
| 7 | `KeBugCheck`, `RtlCompareMemoryUlong`, `sprintf` |

Par famille : 32 `Xam*`, 30 `Ke*`, 25 `Nt*` (fichiers, mémoire, synchro), **24 `Vd*`** (anneau de
commandes GPU, `VdSwap`, modes vidéo), 20 `Rtl*`, 7 `Mm*`, 6 `Ex*`, 5 `Xex*`, **5 `XAudio*`**
(pilote de rendu audio), 4 `XMsg*`, 3 `XeCrypt*`, 2 `XNotify*`, `NetDll_WSAStartup`.

## Fonctions non résolues / points ouverts du C++

- Toutes les fonctions de `.pdata` (11 254) + celles trouvées par analyse : 15 014 fonctions générées.
- **211 pointeurs vers `.text`** trouvés dans `.rdata`/`.data` ne tombent ni sur un début de fonction
  ni sur une cible de table ; aucun ne pointe sur un prologue. Hypothèse principale : entrées des
  tables de portée SEH (`__C_specific_handler`, 73 fonctions) qui désignent des filtres et
  gestionnaires `__except` en milieu de fonction. **À trier avant le runtime** : un appel indirect
  vers une adresse absente de `PPCFuncMappings` plante.
- Approximations héritées de XenonRecomp, à garder en tête : `XER[CA]` calculé sur 32 bits ;
  `stfs` arrondit au lieu de tronquer (identique si la valeur est déjà en simple précision).
- Aucune optimisation activée (`skip_lr`, `cr_as_local`, …) : à évaluer une fois le runtime en place.

## Shaders

- `Media/Shaders` : 57 `.xpu` + 17 `.xvu` (+ `.pdb` associés) → **74/74** convertis en HLSL et
  compilés en SPIR-V par DXC (`shader_cache.cpp`, 74 entrées).
- Shaders intégrés au XEX (bibliothèques XDK : XUI, lecteur vidéo, D3DX) : 85 conteneurs ;
  21 passent, **64 vertex shaders font planter XenosRecomp** (en-tête d'une variante plus ancienne :
  `fieldC = 0`, table de constantes en `0x24`). À traiter dans XenosRecomp ou en remplaçant ces
  chemins (XUI, vidéo) côté runtime.

## Estimation du travail pour le runtime

Ordre de grandeur, en s'appuyant sur l'architecture d'UnleashedRecomp (référence, sans copier son code) :

| Bloc | Contenu | Charge |
|---|---|---|
| Socle | mémoire invitée 4 Go, chargement de l'image, threads (`ExCreateThread`), TLS, sections critiques, événements, SEH | 2–3 semaines |
| Noyau/XAM HLE | ~180 imports ; fichiers `game:\` → paquet extrait ; profils, contenus et sauvegardes (`XamContent*`) ; notifications ; réseau neutralisé | 2–4 semaines |
| Rendu | D3D9 Xbox **lié statiquement** (pas d'imports D3D) : soit émuler l'anneau de commandes Xenos (`Vd*`, comme un émulateur), soit identifier les fonctions D3D du binaire (sans symboles) et les remplacer, comme UnleashedRecomp ; plus EDRAM, tiling, formats | 2–4 mois |
| Audio | XACT3/XAudio2 liés statiquement, sortie par `XAudioSubmitRenderDriverFrame` ; décodage XMA des wavebanks (`.xwb`) | 3–6 semaines |
| Vidéo | 6 `.wmv` (décodeur XDK lié) : laisser le code recompilé décoder ou remplacer par un décodeur hôte | 1–3 semaines |
| Entrées | `XamInput*` → manette SDL | quelques jours |
| Build Windows x64 | Clang-cl ou Clang + CMake, comme UnleashedRecomp | 1–2 semaines |

## Points de vigilance propres au jeu

- **Lua 5.1 + setjmp/longjmp** : XenonRecomp place un `jmp_buf` hôte dans le tampon invité. Il faut
  vérifier que le tampon invité est assez grand pour le `jmp_buf` de l'hôte (Windows x64 : 256 octets).
- **XUI** : interface en XUI (`IslandsOfWakfu.xzp`, 259 Mio) ; le moteur XUI est lié statiquement,
  donc recompilé, mais ses shaders XDK font partie des 64 échecs.
- **Réseau** : XONLINE/XNET liés, matchmaking HTTP `/xmatch/*.ashx` (service probablement disparu) :
  prévoir des stubs « hors ligne ».
- **Moteur maison `MikIso`** (145 classes RTTI) et 602 scripts Lua en clair : bon point pour le
  débogage (logique de jeu lisible).
- Un « switch » en chaîne de `bdzf`, des tables intégrées au code, des gardes de `switch` éloignées
  ou absentes : le compilateur (16.0.11886)
  produit des motifs que les heuristiques prévues pour Sonic ne voient pas ; revérifier après
  toute nouvelle TU.

## Livraison

- Dépôt privé (sans fichiers du jeu ni C++ généré).
- C++ généré : release privée `ppc-20261114-9ace96b` (paquet générique `wakfu-ppc`, version `20261114-9ace96b`).
