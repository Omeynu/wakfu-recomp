# D3D natif — shaders précompilés (B3, XenosRecomp)

Évaluation du 18/11 : XenosRecomp (commit `70a084c`, `tools.lock`) sur tous les conteneurs de shaders du jeu, puis
HLSL → SPIR-V. Rien n'a été exécuté côté jeu ; seuls XenosRecomp, DXC, glslang et `spirv-val` ont tourné.

## 1. Résultats

| Source | Conteneurs | XenosRecomp → HLSL | DXC → SPIR-V (`spirv-val` Vulkan 1.2) | glslang `-D` |
|---|---|---|---|---|
| `Media/Shaders` (`.xpu`/`.xvu`) | 74 (74 empreintes distinctes) | 74 | 74 | 0 |
| intégrés au XEX (XDK : XUI, vidéo, D3DX) | 85 (16 PS, 69 VS) | 21 (16 PS, 5 VS) | 21 | 0 |
| **Total** | **159** | **95** | **95** | **0** |

- **64 vertex shaders XDK font planter XenosRecomp** (SIGSEGV, `assert` retiré en Release). Ce sont les shaders
  du décodeur WMV matériel de l'XDK (`codecs\wmv\...\dxva_pk`, PDB `Shader_Mocomp*`, `Shader_Residuals*`,
  `Shader_Detile*`, `Shader_Deblock*`, `Shader_RepeatPad*`...) : des « calculs » en vertex shader, **sans
  déclaration de sommets ni interpolateur** (0 et 0 dans l'en-tête), qui lisent leurs données par `vfetch` brut
  (constante de fetch 95, formats 6 et 25) et écrivent en mémoire par **memexport** (`eA`, `eM0`-`eM4` = exports
  32-37 ; vu sur 55 des 64, les 9 autres plantent avant). XenosRecomp ne gère ni l'un ni l'autre : 51 plantent sur
  `vertexElements.find()` (l. 180), 13 sur `interpolators.find()` (l. 629). L'en-tête n'est pas en cause :
  `fieldC = 0x24` (bloc de chemin PDB, table des constantes déplacée en `0xBC`) se retrouve sur 17 VS traduits.
  Corriger XenosRecomp demande d'ajouter memexport et `vfetch` brut (lecture/écriture de la mémoire invitée, format
  et boutisme de la constante de flux) **et** un contrat côté moteur natif (mémoire invitée et constantes de fetch
  liées au shader, synchronisation des écritures) : hors d'un correctif d'en-tête. Le traducteur maison (Xenia)
  gère memexport : il reste le repli pour ces 64 empreintes.
- **glslangValidator `-D` échoue sur les 95** : son frontal HLSL ne lit pas l'en-tête commun (fonctions à paramètre
  `Texture2D<float4>`, l. 55), ni `vk::RawBufferLoad`, ni `uint64_t` en push constant. Impasse.
- **DXC** : il est déjà fourni par XenosRecomp, sous-module `thirdparty/dxc-bin` (`renderbag/dxc-bin`, commit
  `12395e3` du 13/10/2026, `libdxcompiler.so` 1.8 dev 4662-416fab6b ; SHA-256 dans `tools.lock`). C'est une
  **redistribution tierce, pas une publication Microsoft**. Pour distribuer, prendre une publication officielle de
  github.com/microsoft/DirectXShaderCompiler (archive `linux_dxc_*.x86_64.tar.gz` et `dxc_*.zip` Windows, dernière
  v1.8.x publiée depuis au moins 2 semaines, SHA-256 de l'archive dans `tools.lock` et `provenance.md`). Licence :
  University of Illinois/NCSA (dérivée de LLVM), redistribuable avec son avis de licence. À proposer, pas installé.
- Options DXC utilisées (celles de `dxc_compiler.cpp`) : `-T vs_6_0|ps_6_0 -HV 2021 -all-resources-bound -spirv
  -fvk-use-dx-layout [-fvk-invert-y pour VS] -Qstrip_debug`. 95 fichiers : 221 Ko de SPIR-V, 4 s sur 8 cœurs.

## 2. Ce que contiennent les shaders du jeu (95 traduits)

- Entrées de sommets : seulement `POSITION0`, `TEXCOORD0`, `COLOR0` (22 VS) ; aucune normale/tangente
  (`tfetchR11G11B10` jamais appelé).
- Textures : 184 appels `tfetch2D`, aucun 3D, cube ni bicubique ; registres d'échantillonneur 0 à 8.
- Sorties PS : `SV_Target0` (73), `SV_Target1` (1), `SV_Depth` (1). Aucune constante booléenne (`g_Booleans` inutilisé).
- Toutes les PS à couleur ont le test alpha par constante de spécialisation (`clip(oC0.w - g_AlphaThreshold)`).

## 3. Interface attendue par le SPIR-V (ce que le moteur natif doit fournir)

Prérequis Vulkan : 1.2 avec `bufferDeviceAddress`, `descriptorIndexing` (`runtimeDescriptorArray`), `shaderInt64`.
Point d'entrée `main`. Les VS écrivent 18 `vec4` interpolés + position : `maxVertexOutputComponents ≥ 72`
(minimum Vulkan 64 ; 128 sur les GPU de bureau et lavapipe).

| Élément du shader | Forme SPIR-V | À fournir par le moteur (B3 cœur) | Source côté D3D 360 |
|---|---|---|---|
| Constantes float VS `c0..c255` | push constant `VertexShaderConstants` (u64, adresse de tampon), `vec4` en `16 × registre` | tampon 4 096 o (256 `float4`) par draw, adresse GPU dans le push constant | constantes du device (`dev+0x780`, cf. `natif-b0-draws.md`) |
| Constantes float PS `c0..c223` | push constant `PixelShaderConstants` (u64), même règle | tampon 4 096 o | `dev+0x1780` |
| Constantes partagées | push constant `SharedConstants` (u64), 276 o utiles | tampon ≥ 288 o, disposition ci-dessous | état du device |
| — index de texture 2D, échantillonneur `s` | `uint` en `SharedConstants + 4·s` | index dans le tableau sans borne `set 0, binding 0` (`Texture2D[]`) | `SetTexture(s)` |
| — index de texture 3D / cube | `+ 64 + 4·s` / `+ 128 + 4·s` | `set 1` / `set 2`, binding 0 (inutilisés par le jeu) | idem |
| — index d'échantillonneur | `+ 192 + 4·s` | index dans `set 3, binding 0` (`SamplerState[]`) | `SetSamplerState(s)` |
| — `g_Booleans` | `uint` en `+ 256` ; bit `i` = bool VS `i`, bit `16+i` = bool PS `i` | 0 suffit pour ce jeu | `SetVertex/PixelShaderConstantB` |
| — `g_SwappedTexcoords` | `uint` en `+ 260` (bit = TEXCOORDn lu `.yxwz`) | 0 (propre à Unleashed) | — |
| — `g_HalfPixelOffset` | `float2` en `+ 264`, ajouté à `oPos.xy · w` | décalage demi-pixel D3D9 en NDC ; le raccord natif le laisse à 0 (PA_SU_VTX_CNTL = 4, comme l'émulation) | viewport |
| — `g_AlphaThreshold` | `float` en `+ 272` | référence alpha / 255 | `D3DRS_ALPHAREF` |
| Constante de spécialisation | `constant_id 0` (`uint`), bit 0 normales R11G11B10, bit 1 test alpha | bit 1 si `D3DRS_ALPHATESTENABLE` (une variante de pipeline par valeur) ; fonction de comparaison toujours « ≥ » | `D3DRS_ALPHAFUNC` (à vérifier : autre que GREATEREQUAL non géré) |
| Entrées de sommets | `Location` fixe : POSITION0 → 0, TEXCOORD0 → 4, COLOR0 → 8 (table Unleashed : NORMAL 1, TANGENT 2, BINORMAL 3, TEXCOORD1-3 → 5-7, BLENDINDICES 9, BLENDWEIGHT 10, COLOR1 11, TEXCOORD4-7 → 12-15) | `VkVertexInputAttributeDescription` par `Location` depuis la déclaration de sommets ; données remises en petit-boutiste, format Vulkan tiré de `D3DDECLTYPE` (COLOR `D3DCOLOR` : ordre des octets à vérifier) | `CreateVertexDeclaration` (`natif-b0-draws.md` §4) |
| Interpolateurs VS → PS | `Location 0-15` = TEXCOORD0-15, `16` = COLOR0, `17` = COLOR1 (toujours tous déclarés, mis à 0) | rien : VS et PS concordent par construction | — |
| Sortie position | `gl_Position`, Y déjà inversé (`-fvk-invert-y`) | viewport à hauteur **positive** (pas de viewport négatif) | `SetViewport` |
| Sorties PS | `Location 0/1` (`SV_Target0/1`), `FragDepth` pour 1 shader | cibles de couleur selon le PS ; écriture de profondeur pour ce PS | `SetRenderTarget` |
| `iFace` / `iPos` | `FrontFacing`, `FragCoord` (r0 = position écran − 0,5) | rien | — |

Disposition `SharedConstants` : `[0,64)` index 2D des 16 échantillonneurs, `[64,128)` 3D, `[128,192)` cube,
`[192,256)` échantillonneurs, puis `256 g_Booleans`, `260 g_SwappedTexcoords`, `264 g_HalfPixelOffset`,
`272 g_AlphaThreshold`. Tableaux de descripteurs : un tableau par type, liés une fois (sans borne), accès par index.

## 4. Correspondance shader ↔ SPIR-V

Empreinte = **XXH3-64 du conteneur** (`virtualSize + physicalSize` octets depuis l'en-tête `0x102A110x`), comme
XenosRecomp en mode dossier et `CreateShader` d'Unleashed. Vérifié : `packaging/shaders/xr_hash.c` donne les 74
mêmes empreintes que le `shader_cache.cpp` produit par XenosRecomp. Le moteur doit hacher le conteneur reçu par
`CreateVertexShader`/`CreatePixelShader` (pointeur sur l'en-tête, pas sur le microcode seul).

## 5. Précompilation à l'installation (paquets natif)

`packaging/shaders/precompile-shaders.sh <conteneurs> <sortie>` : pour chaque `.xpu`/`.xvu`, XenosRecomp → HLSL →
DXC → `<hash>.spv`, plus `table.tsv` (`hash`, `vs|ps`, fichier, conteneur source) et `echecs.tsv`. Outils par
variables d'environnement (`XENOSRECOMP`, `XENOS_INCLUDE`, `DXC`, `DXC_LIB`, `XR_HASH`). Testé sur les 159
conteneurs : 95 SPIR-V valides, 64 échecs listés ; conteneur tronqué ou faux rejeté, doublons ignorés.
Paquets natif (19/11) : XenosRecomp (sources) est construit chez le joueur avec le DXC officiel v1.9.2609
(`packaging/shaders/xenosrecomp/CMakeLists.txt`, Linux et llvm-mingw) ; mêmes 95 SPIR-V / 64 échecs qu'avec dxc-bin.
Linux : `installer.sh --natif` (Media/Shaders + `xr_hash -x image.bin` : les 85 conteneurs du XEX, identiques à
`xex-split`), puis `essayer-natif.sh` retraduit `spirv/conteneurs` avant chaque lancement. Windows :
`WakfuRecomp-natif.exe --natif` (même boucle en C++, `natifShaders`, échecs notés dans `spirv\echecs\` et non
retentés, sans fenêtre d'erreur Windows). Le moteur lit `<hash>.spv` (pas `table.tsv`).

Shaders créés à l'exécution : le jeu compile aussi des shaders depuis du HLSL avec le D3DX intégré au XEX
(`sub_823CF4B0` → `sub_823F8378` → `CreateVertexShader`/`CreatePixelShader`, plus 3 shaders XDK construits en
mémoire). Leurs conteneurs n'existent dans aucun fichier (ni `Media`, ni `image.bin`) : 5 empreintes (dont
`94f641c89f822edf` VS et `e57bfecfdf4b8969` PS, ceux des arbres, lanternes et personnage en zone 1). Avec
`WAKFU_NATIF_SPIRV=<dossier>`, le runtime dépose au premier usage chaque conteneur sans `.spv` dans
`<dossier>/conteneurs/` ; `precompile-shaders.sh <dossier>/conteneurs <dossier>` les traduit (XenosRecomp les lit
tous) et complète `table.tsv`. Ils sont rendus au lancement suivant.

## 6. Recommandation

XenosRecomp pour les 95 shaders traduits (interface Unleashed ci-dessus, déjà faite pour un moteur D3D9 intercepté),
**traducteur maison en repli** pour les 64 VS XDK (memexport du décodeur WMV, non géré par XenosRecomp). DXC : publication
officielle Microsoft (dxc-bin n'est plus utilisé).
