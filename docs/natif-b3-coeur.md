# B3 — cœur du rendu natif (WAKFU_NATIF=1)

Rendu Vulkan natif des appels D3D du jeu, **en parallèle** de l'émulation PM4, qui reste la référence affichée.
Désactivé par défaut. Étape B4 (comparaison) : `WAKFU_NATIF_CAPTURE=<image>,<fichier.png>` écrit l'image native
de la texture passée à Swap (à défaut, la cible couleur 0) au Swap numéro `<image>` (compte des appels à 821B7968,
comme `WAKFU_OMBRE`), à comparer à la capture de l'émulation (`WAKFU_CAPTURE`, numérotée par VdSwap).

## Architecture

| Fichier | Rôle |
|---|---|
| `runtime/src/d3d_ombre.cpp` | crochets `PPC_FUNC` (l'original est toujours appelé) ; sans glslang : ombre seule |
| `runtime/src/natif/device.{h,cpp}` | lecture de l'état du device invité (fil du jeu), file de commandes, fil « natif » (Vulkan) |
| `runtime/src/natif/etats.{h,cpp}` | traduction pure registres/énumérations D3D 360 → états de pipeline Vulkan (test `wakfu_unit_natif_etats`) |
| `runtime/src/natif/interfaces.h` | interfaces attendues des chantiers shaders et sommets, état du draw (`Instantane`) |
| `runtime/src/natif/raccord.cpp` | shaders précompilés, sommets/tampons, textures de l'émulation (remplace les bouchons) |

Même schéma que `Video::` d'UnleashedRecomp (idées seulement, aucun code repris) : les crochets lisent l'état puis
envoient une commande ; un fil dédié l'exécute. Le fil natif partage le `vk::Context` de l'émulation
(`RendererContext()`), avec son propre pool de commandes et 2 lots en vol ; les soumissions sur la file Vulkan
passent maintenant toutes par `Context::queueLock` (émulation, fenêtre, natif). Le jeu peut avoir au plus 2 Swap
d'avance sur le fil natif. Avant que l'émulation soit prête, seules les créations et liaisons de cibles sont gardées.

Crochets : Draw* (821B1F28, 821B2340, 821B1900, 821B1E88, état relevé **avant** l'original qui vide les bits dirty),
ClearF 821B6F68 (cœur de Clear et BeginTiling), Resolve 821BA740, SetRenderTarget 821B57F8, SetDepthStencilSurface
821B5B48, SetVertexShader/SetPixelShader (→ `ShaderVu`), CreateSurface 821AA3F8, CreateTexture 821AA2D8, Swap
821B7968, vidage autonome 821B9390 (constantes relevées avant sa remise à zéro des bits dirty).

État lu au draw (docs/natif-b0-draws.md §2, natif-b0-cibles.md §2) : ombre des registres 0x2934 / 0x2938 / 0x293C /
0x2948 / 0x28DC / 0x2900 / 0x28FC, constantes de mélange 0x28E0, constantes float VS 0x780 / PS 0x1780 (copie locale
tenue à jour d'après dirty[0]/[1], bit de poids fort = registres 0-3 ; écritures en ligne du jeu comprises),
booléens 0x2780, entiers 0x27A0, constantes de lecture 0x480 (textures et flux), shaders 0x3290/0x328C, déclaration
0x2F58, IB 0x318C, VB/strides 0x31A4/0x31E8, textures 0x31F8, viewport 12896, scissor 12924 (actif : 12160).
Draws UP : sommets (et indices) recopiés, la mémoire de l'appelant étant temporaire.

## Ce qui marche (compilé ; non exécuté en jeu)

- Cibles : image Vulkan par surface à CreateSurface, au vrai format (`FormatCible` : 8888, 2_10_10_10, 565,
  16/32 bits flottants, k_24_8 → D24S8 ou D32S8 selon le GPU, k_24_8_FLOAT → D32S8) et au vrai MSAA (1/2/4).
  Textures destinations de resolve : créées à CreateTexture si `D3DUSAGE_RENDERTARGET`, sinon au premier resolve,
  d'après le fetch constant de l'en-tête (recréées si taille ou format changent).
- Passes de rendu dynamiques sur RT0-3 + profondeur (chargement/stockage), rouvertes au changement de cible.
- Clear natif : `vkCmdClearAttachments` (TARGET0-3 0x1-0x8, ZBUFFER 0x10, STENCIL 0x20), rectangle compris.
- Resolve natif : résolution MSAA (`vkCmdResolveImage`) ou copie (`vkCmdCopyImage`, profondeur comprise) ou blit si
  les formats couleur diffèrent, rectangle source et point de destination ; effacements 0x100 / 0x200 qui suivent.
- Traduction des états (testée) : mélange par cible, masque d'écriture, profondeur, stencil deux faces (référence et
  masques en états dynamiques), culling + sens (viewport : hauteur positive, Y inversé par les VS précompilés), mode de remplissage, test alpha et
  alpha-vers-couverture relevés pour le pixel shader ; pipelines en cache par clé (états, formats, échantillons,
  clés shader/sommets, topologie).
- Ligne `[natif N]` toutes les 60 images : draws reçus / dessinés / sans shader / sans sommets / primitive non gérée,
  effacements, resolves (non faits), cibles absentes, formats inconnus.

## Ce qui manque

- **Shaders et sommets** : raccordés (natif/raccord.cpp, voir natif.md § B3). Historique : bouchons ; les agents
  b3-shaders / b3-sommets fournissent `ObtenirShaders` (modules, disposition, liaison des constantes et textures)
  et `ObtenirSommets` (entrée de sommets, émission du draw) selon `natif/interfaces.h`.
- Primitives rectangle (8) et quad (13) : non prises en charge (comptées).
- Resolve : niveau de mip > 0, profondeur MSAA, `EndTiling` et le resolve interne de Present (passent par le cœur
  821B95F0, non crocheté) ; Begin*Vertices appelés directement par XUI (822CF798) ne sont pas vus.
- Surfaces créées hors CreateSurface (s'il y en a) : « cibles absentes » ; destruction des ressources non suivie
  (image remplacée seulement si le pointeur est réutilisé).
- Format `k_16_16_16_16` fixe traité en flottant ; format des indices UP : 6 supposé 32 bits.
- Toutes les images restent en `VK_IMAGE_LAYOUT_GENERAL` avec barrières globales (simple, pas optimal).
- Capture : formats 8 bits RGBA seulement.
