# Natif B3 : déclarations de sommets et tampons VB/IB

Module autonome `runtime/src/natif/` (non branché, actif sous `WAKFU_NATIF=1`), pour le cœur natif.
Test : `wakfu_unit_natif_sommets` (chaque type ci-dessous, déclaration à deux flux, indices 16/32).

## API
- `natif::FromDecl(pDecl)` (objet de CreateVertexDeclaration 821AC9A8 : n à +0x18, éléments à +0x34) ou
  `FromElements(octets)` → `VertexLayout` : un `Attribute` par élément (flux, décalage, `usage`/`usageIndex` pour relier
  aux entrées du shader, `VkFormat`), location Vulkan = indice de l'attribut, `key` = empreinte (cache de pipelines).
- `Describe(layout, pas[16], &bindings, &attributs)` : binding = flux, pas PC = pas invité (SetStreamSource) + `extra[flux]`.
- `BuffersInit(ctx, images, octetsParImage)`, `BuffersBeginFrame(n)` (après l'attente de la barrière de l'image),
  `OnUnlock(pRes)` (après l'original de 821BAC30 / 821BACF0), `OnDestroy(pRes)` (avant 821BB548),
  `BufferFor(cmdEnvoi, pVB, layout, flux, offset, pas)` / `IndexBufferFor(cmdEnvoi, pIB, &type)` → `{VkBuffer, offset, size}`.
  `cmdEnvoi` : hors `vkCmdBeginRendering`, soumise avant les draws (copie + barrières).
- Ajout de `vkCmdCopyBuffer` à la liste de `gpu/vk.h`.

## D3DDECLTYPE 360
Encodage : format de lecture Xenos (bits 0-5) | boutisme (6-7 : 1 = 8in16, 2 = 8in32) | signé (8) | entier non
normalisé (9) | swizzle 4 × 3 bits (10-21 : 0-3 = XYZW, 4 = 0, 5 = 1). Décodage générique : tout type de cette forme
est traité, pas seulement la table.

| Type | Valeur | Xenos | Format Vulkan | Traitement |
|---|---|---|---|---|
| FLOAT1/2/3/4 | 2C83A4 / 2C23A5 / 2A23B9 / 1A23A6 | 32(_32…)_FLOAT | R32…_SFLOAT | inversion 32 bits |
| D3DCOLOR | 182886 | 8_8_8_8, ZYXW | R8G8B8A8_UNORM | inversion 32 bits + permutation (ARGB → RGBA) |
| UBYTE4 / UBYTE4_2 | 1A2286 / 1A2386 | 8_8_8_8 | R8G8B8A8_USCALED / _SSCALED | inversion 32 bits |
| UBYTE4N / UBYTE4N_2 | 1A2086 / 1A2186 | 8_8_8_8 | R8G8B8A8_UNORM / _SNORM | inversion 32 bits |
| SHORT2 / SHORT4 | 2C2359 / 1A235A | 16_16(_16_16) | R16G16(B16A16)_SSCALED | inversion 16 bits |
| SHORT2N / SHORT4N | 2C2159 / 1A215A | idem | …_SNORM | inversion 16 bits |
| USHORT2N / USHORT4N | 2C2059 / 1A205A | idem | …_UNORM | inversion 16 bits |
| FLOAT16_2 / FLOAT16_4 | 2C235F / 1A2360 | 16_16(_16_16)_FLOAT | R16G16(B16A16)_SFLOAT | inversion 16 bits |
| UDEC3 / DEC3N | 2A2287 / 2A2187 | 2_10_10_10, XYZ1 | A2B10G10R10_USCALED / _SNORM_PACK32 | inversion 32 bits, W réécrit à 1 |
| UINT1 | 2C82A1 | 32 (entier) | R32G32B32A32_SFLOAT | **converti** |
| DEC3N_2 / DEC3N_3 | 2A2190 / 2A2390 | 10_11_11 | R32G32B32A32_SFLOAT | **converti** |

Converti = aucun format Vulkan (10_11_11, 11_11_10, entiers/normalisés 32 bits) ou swizzle impossible par
permutation d'octets : valeur calculée en flottant (swizzle appliqué), 16 octets ajoutés en fin de sommet PC ;
les octets invités restent en place. Entier non normalisé = flottant de l'entier (SCALED), comme la lecture Xenos.

## Tampons
- Données invitées : VB adresse `+24 & ~3`, taille `+28 & 0x03FFFFFC` ; IB adresse +24, taille +28, 32 bits si bit 31
  de Common (Format << 29 : INDEX16 = 1, INDEX32 = 6).
- Conversion paresseuse au premier draw après Unlock : la permutation dépend de la déclaration, inconnue à l'Unlock
  (CreateVertexBuffer 360 n'a pas de données initiales). Une version par ressource, clé = (attributs du flux, offset, pas).
- Statique : écrit dans l'anneau (HOST_VISIBLE), copié vers un tampon DEVICE_LOCAL. Dynamique (deux Unlock à moins de
  4 images d'écart, définitif) : servi depuis l'anneau de l'image, reconverti à chaque image où il sert.
- Destructions Vulkan différées de `images` images.

## Non couvert / à vérifier
- SNORM/USCALED/SSCALED (8888, 16_16, 2_10_10_10) ne sont pas obligatoires en Vulkan : le cœur doit vérifier
  `vkGetPhysicalDeviceFormatProperties` (VERTEX_BUFFER) ; pas de repli automatique.
- Convention SNORM supposée D3D10 (max(v / (2^(n-1) - 1), -1)), non mesurée sur la console.
- Un même VB lu avec deux déclarations/offsets différents dans une image est reconverti à chaque changement.
- Anneau plein : `BufferFor` rend size 0 (message unique) ; pas d'agrandissement.
- Lecture de la mémoire invitée au draw : suppose un rendu synchrone avec le thread du jeu (sinon copier à l'Unlock).
- Swizzle 6/7 (inconnus) : composante 0. DrawVerticesUP / IndexedUP (sommets hors VB) : hors module.
- Aucun essai en jeu ni sur GPU (seule la conversion CPU est testée).
