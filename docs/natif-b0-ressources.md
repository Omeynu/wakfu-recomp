# Natif B0 : ressources D3D (textures, tampons, verrous, libération)

Analyse statique seulement (désassemblage `work/game/patched/text.dis`, index des `bl`/`b` contre `ppc_func_mapping.cpp`).
Aucun binaire lancé. Confiance : **sûr** (motif + champs + appelants concordants), **probable**, **hypothèse**.

## 1. Allocation (sous les créations)

| Adresse | Fonction | Preuve | Confiance |
|---|---|---|---|
| 821A1148 | `XMemAlloc(size, attr)` (implémentation par défaut, non surchargée) | bit 31 de attr → `XPhysicalAllocEx`/`XPhysicalAlloc`, sinon tas ; bit 30 → memset 0 ; 427 appels / 224 fonctions | sûr |
| 821A1268 | `XMemFree(p, attr)` | bit 31 → 821A2708, sinon tas ; 499 appels | sûr |
| 821A1288 | `XMemSize` | même aiguillage → 821A2700 (MmQueryAllocationSize) | sûr |
| 821A2608 | `XPhysicalAlloc(size, maxAddr, align, protect)` | seul appel `MmAllocatePhysicalMemoryEx` (r3=0) + tail `b` | sûr |
| 821A2690 | `XPhysicalAllocEx(size, min, max, align, protect)` | idem, 5 arguments | sûr |
| 821A2708 | `XPhysicalFree(p)` | `MmFreePhysicalMemory(0, p)` | sûr |

Attributs XALLOC vus (octet d'allocateur = bits 16-23) : `0x6480xxxx` en-têtes D3D (virtuel, mis à zéro, id 0x80 = D3D) ;
`0x8280` + protection (rlwimi bits 28-30) tampons VB/IB ; `0xAC80/0xBC80` (calculé) mémoire de texture, alignée 4 Ko ;
`0xB580` microcode / anneau ; `0xB180` libération des textures. Le runtime répond à tout cela par
`Alloc(Heap::Physical, …)` (kernel/core.cpp:136) : la mémoire des ressources est donc de la mémoire invitée ordinaire.

## 2. Disposition des en-têtes (observée)

En-têtes alloués 0x6480 (zéro-initialisés), tous commencent par **D3DResource (24 octets)** :

| Off. | Champ | Valeur écrite à la création |
|---|---|---|
| +0 | Common | type (bits 0-3 : 1 VB, 2 IB, 3 texture, 4 surface…) \| `0x00100000` D3DCREATED \| `0x00200000` si Usage&4 \| `0x00400000` si Usage&0x200 ; surface enfant : bit 30 (`0x40000000`) |
| +4 | ReferenceCount | 1 (lwarx/stwcx dans AddRef/Release) |
| +8 | Fence | 0 (lu par Lock et Destruct → BlockOnFence 821BD620) |
| +12 | ReadFence | 0 (zéro d'allocation) |
| +16 | Identifier | 0 |
| +20 | BaseFlush | `0xFFFF0000` |

- **D3DBaseTexture, 52 octets** (821AA2D8 alloue 52) : +24 MipFlush = `0xFFFF0000`, **+28..+48 Format = fetch constant 6 dwords**.
  +32 (dword1) : format GPU bits 0-5, adresse de base dans bits 12-31 (`rlwimi r11,r29,0,0,19`) ;
  +36 dword2 : taille ; +44 dword4 : MaxMipLevel bits 6-9 (`GetLevelCount` = ((+44>>6)&15)+1) ;
  +48 dword5 : adresse des mips bits 12-31, dimension bits 9-10 (0=1D, 1=2D, 2=3D, 3=cube). Tableau = dword1 bit 10.
  `GetType` 821BA790 rend 3/17(volume)/18(cube)/19(array)/20(line).
- **D3DSurface, 48 octets** (821AA3F8, 821AA088) : +24 parent (texture) si Common bit 30, sinon info EDRAM ;
  +28 niveau (bits 28-31) et face ; +36 taille ; +40 format (22/23 = k_24_8 → profondeur, EDRAM via 821BCCF0).
- **D3DVertexBuffer, 32 octets** : +24 dword0 = adresse|3 (type fetch 3), +28 dword1 = taille (`&0x03FFFFFC`) | endian 2 | `0x10000000`.
- **D3DIndexBuffer, 32 octets** : Common |= Format<<29 (r5 : 1 ou 6 aux appels), +24 adresse, +28 taille.

## 3. Fonctions (adresses, signatures, preuves)

| Adresse | Fonction | Signature probable | Preuves | Appels | Unleashed | Confiance |
|---|---|---|---|---|---|---|
| 821AA2D8 | **CreateTexture** (générique 2D/volume/cube/tableau) | `(W, H, Depth, Levels, Usage, Format, Pool, D3DRTYPE)` → en-tête | alloue 52 o., appelle 821A9B10 (écrit Common=0x00100003, refs, flushes, fetch), alloue base puis mips en physique, ré-injecte les adresses dans +32/+48 | 17 sites / 10 fonctions, r10 = 3 partout dans le jeu (17 dans D3DX 821CF548) | `CreateTexture` sub_82BE9498, mêmes 8 arguments | sûr |
| 821A9B10 | remplissage d'en-tête texture (cœur de XGSetTextureHeaderEx) | (type, W, H, D, Levels, Usage, Format, …, pTexture, &base, &mip) | écrit +0..+48 ; appelé aussi par XG 8220C498 | 6 | — | sûr |
| — | CreateCubeTexture / CreateVolumeTexture / CreateArrayTexture | **pas de fonction séparée** : 821AA2D8 avec type 18/17/19 | aucun appelant avec 18/19 ; 17 seulement dans D3DX | — | type==17 géré | probable |
| 821BAB68 | **CreateVertexBuffer** | `(Length, Usage)` → en-tête | alloue 32 o. + `XMemAlloc(Length, 0x8280…)`, Common 0x00100001, +24 = p\|3 | 9 | sub_82BE6AD0 `(length)` | sûr |
| 821BAC40 | **CreateIndexBuffer** | `(Length, Usage, Format)` | 32 o., Common = Format<<29 \| 0x00100002 | 6 | sub_82BE6BF8 `(length, , format)` | sûr |
| 821AA3F8 | CreateSurface (rendu/profondeur) | `(W, H, Format, MultiSample, pParams)` | 48 o., 821A9E08 (en-tête surface), EDRAM 821BCCF0 si pParams nul | 12 | sub_82BE95B8 | sûr |
| 8220C6A0 | XGSetTextureHeader | `(W, H, Levels, Usage, Format, Pool, BaseOff, MipOff, Pitch, pTex, pBaseSize, pMipSize)` | → 8220C498 (XGSetTextureHeaderEx : en-tête sur pile, memcpy 52 o. si pTex) | 2 (XUI 822C4128/822C42B8, calcul de taille 128×1 seulement) | — | probable |
| — | XGSetVertexBufferHeader / XGSetIndexBufferHeader | absents | recherche du motif `lis -1` → `stw +20` : seuls les créateurs D3D | 0 | — | probable |
| 821AA140 | **Texture LockRect** | `(pTex, Level, pLockedRect, pRect, Flags)` | tail → 821A99D0(pTex, 0, 0, Level, pLocked, pRect, Flags) | 6 (XUI 822CF708/822CF818, vidéo 822DB878 ×3, 822ED1F8) | LockTextureRect sub_82BE9300 | sûr |
| 821AA5C8 | Surface LockRect | `(pSurf, pLockedRect, pRect, Flags)` | parent +24, face/niveau depuis +28, tail → 821A99D0 | 4 (D3DX 821D9138/821D9390) | — | sûr |
| 821A99D0 | LockRect générique | `(pBaseTex, ArrayIndex, Face, Level, D3DLOCKED_RECT*, pRect, Flags)` | 821A98F8 → 821BB790 (attente GPU, code 14) ; écrit {Pitch, pBits = base/mip + décalage} | 2 | — | sûr |
| 821A86F8 | **Texture UnlockRect** | `(pTex)` | `821BA820(pTex, +32&~FFF, +48&~FFF)` | 6, mêmes appelants que LockRect | UnlockTextureRect sub_82BE7780 | sûr |
| 821A8710 | Surface UnlockRect | `(pSurf)` | idem sur le parent +24 | 4 (D3DX) | — | sûr |
| 821BBB58 | **LockVertexBuffer** | `(pVB, Offset, Size, Flags)` → pointeur | 821BB790(…, code 10, adresse +24, taille +28, Flags) ; Offset/Size ignorés | 30 / 20 | sub_82BE6B98 | sûr |
| 821BAC30 | **UnlockVertexBuffer** | `(pVB)` | tail → 821BA820(pVB, +24&~3, 0) | 28 / 20 | sub_82BE6BE8 | sûr |
| 821BBBA8 | **LockIndexBuffer** | `(pIB, Offset, Size, Flags)` | idem, Flags\|2 | 34 / 26 | sub_82BE6CA8 | sûr |
| 821BACF0 | **UnlockIndexBuffer** | `(pIB)` | tail → 821BA820(pIB, +24, 0) | 36 / 26 | sub_82BE6CF0 | sûr |
| 821BB790 | attente avant verrou (BlockOnResource) | `(pRes, u64, code, …, adr, taille, Flags)` | Flags & 0x1010 (READONLY/NOOVERWRITE) choisit Fence/ReadFence → BlockOnFence | 3 | — | probable |
| 821BA820 | fin de verrou : vidage cache CPU | `(pRes, adr, adrMip)` | boucle sur la plage + `hwsync` | 4 (tous les Unlock) | — | probable |
| 821BAA88 | **AddRef** | `(pRes)` → refs | lwarx +4 +1 ; surface enfant → AddRef parent | 29 / 24 | GuestResource::AddRef | sûr |
| 821BBAD8 | **Release** | `(pRes)` → refs | lwarx +4 −1 ; à 0 : Release du parent si surface enfant, puis 821BB548 | 166 / 73 | — | sûr |
| 821BB548 | **DestructResource** | `(pRes)` | aiguillage sur le type : BlockOnFence(Fence), `XMemFree(+32&~FFF, 0xB180…)`, `XMemFree(+48&~FFF)`, VB/IB, EDRAM ; enfin `XMemFree(en-tête, 0x2480…)` | 1 (Release) | sub_82BE6230 | sûr |
| 821AA088 | Texture GetSurfaceLevel | `(pTex, Level)` → surface 48 o. | AddRef(parent), +24=parent, +28=niveau<<28 | 5 | — | sûr |
| 821AA160 / 821AA228 | GetCubeMapSurface / GetVolumeLevel | `(pTex, Face, Level)` / `(pTex, Level)` | même motif ; appelés par D3DX seulement | 4 / 4 | — | probable |
| 821AA218 → 821A9210 | Texture GetLevelDesc | `(pTex, Level, pDesc)` | | 15 | — | probable |
| 821AA530 | Surface GetDesc | `(pSurf, pDesc)` | parent → GetLevelDesc, sinon +40/+36 | 3 | GetSurfaceDesc sub_82BE96F0 | sûr |
| 821AA078 | GetLevelCount | `(pTex)` | ((+44>>6)&15)+1 | 2 | — | sûr |
| 821BA790 | D3DResource GetType | `(pRes)` → D3DRTYPE | voir §2 | 10 | — | sûr |

### XGRAPHICS (tuilage, boutisme)

| Adresse | Fonction | Preuve | Confiance |
|---|---|---|---|
| 8220E6D0 → 8220DF30 | XGTileSurface `(pDst, W, H, pPoint, pSrc, RowPitch, pRect, TexelPitch)` | jumeau de 8220E300 ; W=r4, H=r5, pSrc=r7 (rect par défaut {0,0,W,H}) | probable |
| 8220E6D8 → 8220E300 | XGUntileSurface `(pDst, RowPitch, pPoint, pSrc, W, H, pRect, TexelPitch)` | W=r7, H=r8, pSrc=r6 | probable |
| 8220DC90 → 8220D370 / 8220DCC8 → 8220D800 | XGTileVolume / XGUntileVolume (10 arguments) | même dualité | probable |
| 8220DD00 | copie interne XG (memcpy aligné) | | sûr |
| 8220CDC0, 8220CDC8, 8220D2E8, 8220D310 | lecture de disposition (XGGetTextureDesc / XGGetTextureLayout…) depuis l'en-tête (+32/+48) | lisent le fetch, appellent GetLevelDesc | hypothèse sur les noms |
| 8220E6E0, 8220F160 (+ 8220F1A8, 8220F1F0 jamais appelées) | famille XGEndianSwap* (lwbrx/sthbrx, codes taille<<16\|nombre) | appelée par D3DX 821D5140 | hypothèse |
| — | XGUntileTextureLevel / XGTileTextureLevel | non trouvées (pas liées) | hypothèse |

### D3DX

| Adresse | Fonction | Appelants |
|---|---|---|
| 821CFD90 | D3DXCreateTextureFromFileEx `(pDev, path, W, H, Mip, Usage, Format, Pool, Filter, MipFilter, ColorKey, pInfo, pPal, ppTex)` (lit le fichier via 821D5630) | 82109AC0 (×5 : `game:\media\textures\{effects,maps}\%d.dds`, `{meshes,particles,elements}\%d.png`), 823D61A0 |
| 821CFD20 | D3DXCreateTextureFromFileInMemoryEx (type 3, Depth 1) | 8210E8E8, 82119278 (jeu), 822C58D0 (XUI), 823D7518 |
| 821CF548 | cœur : création + chargement (type 3 ou 17) | les deux précédents |
| 821CDC08 / 821CE2E0 | D3DXLoadSurfaceFromMemory / D3DXLoadVolumeFromMemory | 821CF548, 821CEED0/821CEFF0 |
| 821CF120 | génération des mips (D3DXFilterTexture) | 821CF548 |

`IsBusy` / `BlockUntilNotBusy` : **pas de fonction autonome** (en ligne ou non liées) ; l'attente passe par 821BB790 → 821BD620 (BlockOnFence, déjà crocheté dans game_log.cpp). D3DXFillTexture / MakePictureData : pas d'équivalent identifié.

## 4. Comment le jeu remplit les textures (où détuiler)

1. **D3DX (chemin principal des contenus)** : `D3DXCreateTextureFromFile[InMemory]Ex` → CreateTexture 821AA2D8 → GetSurfaceLevel → D3DXLoadSurfaceFromMemory 821CDC08 :
   LockRect surface (821AA5C8) → conversion → **XGTileSurface** (8220E6D0) dans la mémoire verrouillée → UnlockRect (821A8710) → mips par 821CF120 (même chemin).
   Les données finales sont donc **tuilées, au format Xenos, en mémoire invitée** ; le fetch constant (+28) les décrit entièrement.
2. **CreateTexture + LockRect + écriture + UnlockRect** : XUI (822CF818, 822ED1F8), images vidéo XUI (822DB878, 3 plans par image). Écriture directe au pas rendu par LockRect.
3. **Cibles de rendu** (82107338, 82111488, 821153F8, 82119760, 821C8C40) : CreateTexture + CreateSurface, remplies par Resolve (part « cibles »).
4. **Mémoire fournie + XGSetTextureHeader** : non utilisé pour du contenu (2 appels XUI de calcul de taille). Pas de paquets XPR.
5. **Tampons** : CreateVertexBuffer/IndexBuffer → Lock → memcpy → Unlock (ex. 8210AAA4..8210AAD0), données gros-boutistes.

**Conséquence** : le point unique et sûr est **UnlockRect** (821A86F8 texture, 821A8710 surface ; tous deux passent par 821BA820)
plus la fin de 821CF548 (mips) : à ces points la mémoire invitée est complète ; décoder par le fetch constant avec le décodeur
existant (`gpu/texture_decode`, tuilage géré) et téléverser dans l'image Vulkan associée à l'en-tête. Inutile d'intercepter XGTileSurface.
Tampons : copier + inverser les octets à UnlockVertexBuffer / UnlockIndexBuffer (821BAC30 / 821BACF0).

**Différence avec UnleashedRecomp** : Unleashed remplace les objets invités par des `GuestTexture/GuestBuffer` hôtes et un
LockRect linéaire. Ici ce n'est pas transposable tel quel : D3DX, XG (8220CDC0…) et le Resolve (821B7968, copie +28) lisent
directement les champs du fetch (+32/+48) et écrivent tuilé. Conseillé : appeler l'original partout (en-tête et mémoire invitée
intacts) et tenir une table hôte `adresse d'en-tête → ressource Vulkan + sale`, marquée à Unlock, libérée dans DestructResource 821BB548
(crochet : libérer l'objet hôte puis appeler `__imp__sub_821BB548`).

Modèle de crochet (cf. runtime/src/game_log.cpp) :
```cpp
PPC_FUNC_IMPL(__imp__sub_821A86F8);
PPC_FUNC(sub_821A86F8) { const uint32_t tex = ctx.r3.u32; __imp__sub_821A86F8(ctx, base); /* marquer tex sale */ }
```
Attention : plusieurs cibles sont atteintes par `b` (appel terminal) depuis d'autres fonctions (821AA140 → 821A99D0, 821BAC30 → 821BA820) ;
crocheter la fonction de plus haut niveau voulue, ou la générique (821A99D0 / 821BA820) pour tout attraper.
