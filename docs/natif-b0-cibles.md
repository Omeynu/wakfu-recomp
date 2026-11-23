# Natif B0 : présentation et cibles (D3D9 Xbox 360, XDK 2.0.20353)

Analyse statique seule (text.dis, image.bin, ppc_func_mapping.cpp). Aucun lancement.
Convention d'appel : r3 = device (sauf CreateSurface), un flottant **occupe aussi un emplacement GPR**
(ex. Clear : Z en f1 « mange » r8, Stencil arrive en r9 ; au-delà de r10 → pile de l'appelant, sp+84/92/100…).
Bibliothèque D3D liée : environ 0x821A9000–0x821D8000 (tables .pdata en 0x820E3A00…).

Confiance : **sûr** = preuves concordantes (registres/paquets, champs, appelants) ; **probable** = forme +
signature cohérentes ; **hypothèse** = nom déduit du contexte seulement.

## 1. Tableau

| Fonction (XDK) | Adresse | Signature probable | Appels (jeu) | Confiance |
|---|---|---|---|---|
| Direct3D_CreateDevice | `sub_821BC588` | (r3 Adapter, r4 DevType, r5 hWnd, r6 Behavior, r7 D3DPRESENT_PARAMETERS*, r8 ppDevice) → HRESULT | 1 (`82186BC8`) | sûr |
| init device + swap chain | `sub_821C9148` (r3 dev, r4 pp) → `sub_821C8C40` (crée front/back/depth) | interne | 1 | probable |
| D3DDevice_Release | `sub_821BC060` (réf. dev+60, détruit via `821C9480`) | (dev) → refcount | 9 | sûr |
| D3DDevice_AddRef | `sub_821BC038` | (dev) | 7 | sûr |
| Direct3D_GetDevice (global) | `sub_821BAB00` | (r3 ?, r4 ppDevice) ; lit `**(0x82000560)` (titre) / `0x82000564` | 1 | probable |
| **D3DDevice_Present** | `sub_821B7FF0` | (dev) | 5 | sûr |
| **D3DDevice_Swap** (seul appelant de VdSwap) | `sub_821B7968` | (r3 dev, r4 D3DBaseTexture* pFrontBuffer, r5 D3DVIDEO_SCALER_PARAMETERS* ou 0) | 1 jeu (`821073F8`) + Present + fil `821CC7D8` | sûr |
| fin d'image du jeu (Resolve/EndTiling + Swap) | `sub_821073F8` (code jeu) | (this) ; this+0 = dev, +88 = texture front, +80/+84 = RT/DS à remettre | 4 | sûr |
| attente VBlank/synchro avant Swap | `sub_821B7538` (r4=flags) ; `sub_821B7960` = (dev, 0) | (dev, flags) | 2 | hypothèse (nom) |
| programmation scan-out (flip) | `sub_821B7700` (reg 0x1844), `sub_821B7780` (regs 0x1964/0x1973) | internes à Swap | — | probable |
| fil de ré-affichage (Guide/suspension) | `sub_821CC7D8` | thread ; re-Swap de dev+15120 | 0 (thread) | hypothèse |
| **D3DDevice_SetRenderTarget** | `sub_821B57F8` ; alias jeu `sub_821B5E20` (= `b 821B57F8`) | (dev, r4 index 0..3, r5 D3DSurface*) | 10 (via alias) + 6 internes | sûr |
| **D3DDevice_SetDepthStencilSurface** | `sub_821B5B48` | (dev, r4 D3DSurface*) | 14 appels / 11 fonctions | sûr |
| GetRenderTarget / GetDepthStencilSurface | `sub_821B50F0` (dev, idx) / `sub_821B5138` (dev) | renvoie + AddRef | 4 / 4 | probable |
| **D3DDevice_Clear** | `sub_821B7090` | (dev, r4 Count, r5 D3DRECT*, r6 Flags, r7 D3DCOLOR, f1 Z, r9 Stencil) | 9 | sûr |
| **ClearF** (cœur ; = « Clear » d'UnleashedRecomp) | `sub_821B6F68` | (dev, r4 Flags, r5 D3DRECT* ou 0, r6 D3DVECTOR4* couleur, f1 Z, r8 Stencil) | via Clear (2) + BeginTiling (1) | sûr |
| **D3DDevice_Resolve** | `sub_821BA740` → cœur `sub_821B95F0` | (dev, r4 Flags, r5 D3DRECT* src, r6 D3DBaseTexture* dst, r7 D3DPOINT*, r8 Level, r9 Slice, r10 D3DVECTOR4* ClearColor, f1 ClearZ, [sp+92] ClearStencil, [sp+100] D3DRESOLVE_PARAMETERS*) | 16 appels / 8 fonctions | sûr |
| D3DDevice_BeginTiling | `sub_821B8398` | (dev, r4 Flags, r5 Count, r6 D3DRECT* tiles, r7 D3DVECTOR4* ClearColor, f1 ClearZ, r9 ClearStencil) | 2 | probable |
| D3DDevice_EndTiling | `sub_821B8708` | (dev, r4 ResolveFlags, r5 D3DRECT*, r6 D3DBaseTexture* dst, r7 D3DVECTOR4*, f1 ClearZ, r8 ClearStencil?, r9 pParams?) | 2 | probable |
| **D3DDevice_SetViewport** | `sub_821B56C8` (D3DVIEWPORT9* : X,Y,W,H en DWORD + MinZ,MaxZ) → `sub_821B5460` (SetViewportF : f1..f6) | (dev, r4 D3DVIEWPORT9*) | **0 appel jeu** (internes : SetRenderTarget, tiling) | sûr (forme) |
| D3DDevice_GetViewport | `sub_821B4E68` | (dev, r4 D3DVIEWPORT9* out) | 2 | sûr |
| viewport par défaut (sur changement de RT) | `sub_821B5768` | (dev, r4 surface) | internes | probable |
| **D3DDevice_SetScissorRect** | `sub_821B4D70` | (dev, r4 RECT*) | **0 appel jeu** (Present, tiling, viewport) | sûr (forme) |
| RS SCISSORTESTENABLE (table SetRenderState) | `sub_821B56B8` (pointeur en 0x826343A4) | (dev, r4 BOOL) | via table | probable |
| **D3DDevice_CreateSurface** (CreateRenderTarget **et** CreateDepthStencilSurface inlinés dessus) | `sub_821AA3F8` | (r3 Width, r4 Height, r5 D3DFORMAT, r6 MultiSample, r7 D3DSURFACE_PARAMETERS* ou 0) → D3DSurface* (en-tête 48 o) | 12 appels / 7 fonctions | sûr |
| allocateur EDRAM (tuiles) | `sub_821BCCF0` (+ libération `sub_821BCCA0`) | interne à CreateSurface quand pParams = 0 (limite 2048 tuiles) | — | probable |
| KickOff (soumission du segment courant) | `sub_821BE678` | (dev) → nouveau pointeur d'écriture | 106 internes | sûr |
| réservation N mots (« MakeSpace ») | `sub_821BE8B0` | (dev, r4 nb_dwords) → ptr | 14 internes | probable |
| écriture CP_RB_WPTR (0x7FC80714) | `sub_821BDB00` | interne (appelé par `821BE0D0`) | — | sûr |
| InsertFence (cœur, dans KickOff) | `sub_821BDF90` | écrit dev+11036 en mémoire (EVENT_WRITE_SHD 0xC0025800), puis dev+11036 += 2 | 2 internes | probable |
| **BlockOnFence** | `sub_821BD620` | (dev, r4 fence, r5 flags, r6, r7) | internes seulement | sûr (déjà connu) |
| BlockUntilIdle | `sub_821BE928` | (dev) | 9 internes | probable |
| InsertCallback (public) | `sub_821BE9A0` → cœur `sub_821BE280` | (dev, r4 Type, r5 pCallback, r6 Context) | 1 jeu (`822ED1F8`) + EndTiling | probable |
| rappel D3D (côté interruption) | `sub_821B72E0` | référencé par `lis/addi` dans `821B7538` | — | déjà connu |
| GetBackBuffer / GetFrontBuffer | aucune fonction ; champs dev+15108 / dev+15104 | — | **jamais lus par le jeu** | sûr (absence) |
| StretchRect | n'existe pas en D3D Xbox (le jeu passe par Resolve) | — | — | sûr (absence) |

## 2. Structure du device (taille 0x5F80 = 24448, alignée 128 ; CreateDevice `li r3,24448`)

Disposition **différente** de `GuestDevice` d'UnleashedRecomp (viewport à 0x3260 ici contre 0x3168).

| Offset | Contenu | Preuve |
|---|---|---|
| +0..+47 | drapeaux « dirty » (qwords ; +16 = bloc 0x2000/0x2100/0x2200, +24, +32) | `ld/or/std 16(r3)` dans tous les Set* |
| +48 / +52 / +56 | pointeur d'écriture du tampon de commandes / fin dure / limite douce | `lwz 48; lwz 56; cmplw; bl 821BE678` partout |
| +60 | compteur de références du device | AddRef/Release (lwarx) |
| +10368 | ombre registres bloc 0x2000 (16 regs : RB_SURFACE_INFO, +10372 RB_COLOR_INFO, +10376 RB_DEPTH_INFO, +10380.. COLOR1..3_INFO) | SetRT écrit surf+28 en (2593+n)*4 ; flush `li r5,8192 ; addi r6,r31,10368` |
| +10432 | bloc 0x2080 (3 regs) : +10436 PA_SC_WINDOW_SCISSOR_TL, +10440 _BR | SetScissorRect |
| +10444 / +10528 / +10548 / +10596 | blocs 0x2100 (viewport PA_CL_VPORT_*), 0x2180, 0x2200 (+10560 = 0x2203, surf+32 du DS), 0x2280 | flush `821B68E8` |
| +11024 | pointeur bloc de contrôle anneau (écritures fence/rptr) | Swap, KickOff |
| +11028 | adresse mémoire de retour GPU (attentes WAIT_REG_MEM) | `821B7538` |
| +11036 | compteur de barrières courant (pair, +2 par KickOff) | `821BDF90` |
| +12160 | scissor activé | `821B56B8` |
| +12688..+12700 | RenderTarget[0..3] | SetRT `addi r11,r4,3172 ; slwi 2` |
| +12704 | DepthStencil | SetDS `stw r4,12704(r3)` |
| +12896..+12923 | viewport en flottants X,Y,W,H,MinZ,MaxZ + 1 mot | GetViewport, Present (copie 28 o) |
| +12924..+12939 | rectangle scissor | SetScissorRect, Present (copie 16 o) |
| +12972..+13000+ | état tiling (surface, RT sauvegardés +12976.., nb tuiles +12996, rects +13000) | Begin/EndTiling |
| +13916 | D3DVIDEO_SCALER_PARAMETERS courant (28 o) | Swap compare r5 |
| +15100 / +15104 / +15108 | auto-depth / **front buffer (texture)** / **back buffer (surface)** | Present, `821C8C40`, `821BC258` |
| +15120 | copie de l'en-tête de la dernière texture affichée (52 o) | Swap `addi r3,r31,15120 ; li r5,52` |
| +15216 | barrière de l'image précédente (régulation : Swap fait BlockOnFence dessus) | Swap `lwz r4,15216 ; bl 821BD620` |
| +16840 | compteur de Swap | Swap `+1` |
| +24264 | BehaviorFlags | CreateDevice `stw r30,24264` |

Surface (en-tête 48 o) : +0 Common (type), +4 refcount, +28 valeur RB_SURFACE/COLOR/DEPTH_INFO (base EDRAM + format),
+32 2e mot registre, +36 taille : `((h-1)<<18) | ((w-1)<<3)…` (lu par ClearF).

## 3. Preuves concises

- **Swap `821B7968`** : seul `bl 0x82626FAC` (VdSwap) à `821B7C28` ; aussi VdGetSystemCommandBuffer (`821B7B94`) et
  VdPersistDisplay (`821B7F0C`). Copie 24 o depuis r4+28 (constante de fetch de la texture front) et 52 o vers dev+15120 ;
  compare r5 sur 28 o à dev+13916 ; BlockOnFence(dev+15216) puis dev+15216 = fence courante (limite 1 image d'avance).
- **Present `821B7FF0`** : sauvegarde viewport/scissor, `SetRenderTarget(dev,0,dev[15108])`, `821B7538(dev,0)`,
  `Resolve(dev,0,0,dev[15104],0,0,0,&clr,f1,0,0)`, restaure (`821B5748`, `821B4D70`), puis `Swap(dev,dev[15104],dev+13916)`.
  Même rôle que `Video::Present` d'UnleashedRecomp (`sub_82BDA8C0`).
- **Chemin réel du jeu** (`821073F8`) : `Resolve(dev, 0x300 = CLEARRENDERTARGET|CLEARDEPTHSTENCIL, 0, this[88], …)` ou
  `EndTiling(…)` si this+68 > 0, puis `821B7960` + `Swap(dev, this[88], 0)`, puis `SetRenderTarget(dev,0,this[80])`,
  `SetDepthStencilSurface(dev,this[84])`, Release des deux. Le jeu ne lit jamais le back buffer du device.
- **CreateDevice** : appel `82186BC8` avec pp = 1280×720, `0x18280186` (A8R8G8B8), BackBufferCount 1, SwapEffect 1,
  EnableAutoDepthStencil 1, `0x2D200196` (D24S8), PresentationInterval 1 ; ppDevice = this+40. Alloue 24448 o (align 128),
  DevType 2 → chemin à part (probablement D3DDEVTYPE_COMMAND_BUFFER). Remonte vers `821BE9F8` (VdInitializeRingBuffer,
  VdEnableRingBufferRPtrWriteBack).
- **SetRenderTarget** : `stwx r5` en dev+(3172+idx)*4, surf+28 en ombre 0x2001 (idx 0) / 0x2003+ (idx>0), bit dirty
  `0x20107+n`. **SetDepthStencilSurface** : dev+12704, surf+28 → 0x2002 (dev+10376), surf+32 → dev+10560, ajuste
  RB_SURFACE_INFO (dev+10368) selon le tiling. Les deux appellent `821B5768` (viewport plein écran) si aucun RT.
- **Clear** : convertit D3DCOLOR → vecteur (VMX) puis boucle `ClearF` par rectangle. ClearF → `821B68E8` (vidage complet
  des blocs de registres 0x2000/0x2080/0x2100/0x2180/0x2200/0x2280/0x2300) puis dessin de quad. Forme = `Clear(device,
  flags, unused, color*, z)` d'UnleashedRecomp (`sub_82BFE4C8`) → **crocheter ClearF**.
- **Resolve** : enveloppe 80 o qui insère deux 0 (r6,r7) et décale vers `821B95F0` ; le cœur écrit RB_COPY_CONTROL
  (`li r10,8984` = 0x2318) et 0x231B (COPY_DEST_INFO).
- **CreateSurface** : alloue en-tête 48 o (`821A1148`), `821A9E08` remplit l'en-tête ; si pParams = 0 → `821BCCF0`
  (tuiles EDRAM, refus si > 2048). Appels jeu : `82109274` (D24S8, pParams = {Base calculé, HiZ}), `82109980`, etc.
  Forme = `CreateSurface(width,height,format,multiSample)` d'UnleashedRecomp (`sub_82BE95B8`).
- **KickOff `821BE678`** : appelé quand ptr (+48) > limite (+56) ; soumet le segment (`821BE4F0` → `821BE0D0` →
  `821BDB00` qui écrit 0x7FC80714) et peut faire BlockOnFence(dev+11036−2).
- **InsertCallback** `821BE280` émet WAIT_REG_MEM (`0xC0043C00`) + interruption ; `821B7538` l'utilise avec
  pCallback = `0x821B72E0` (`lis r11,-32229 ; addi r6,r11,29408`).

## 4. Inlinés / non appelés par le jeu (à savoir pour le natif)

- **SetViewport, SetScissorRect : aucun appel du jeu.** Le viewport vient de `821B5768` (plein RT à chaque
  SetRenderTarget/SetDepthStencilSurface) ; le scissor passe par SetRenderState(SCISSORTESTENABLE) = `821B56B8`
  (table de fonctions RS copiée dans le device). Crocheter `821B5460` (SetViewportF) et `821B4D70` couvre tous les cas,
  car les appels internes de D3D passent aussi par les symboles `sub_X` remplacés.
- **GetBackBuffer / GetFrontBuffer / InsertFence / BlockOnFence / KickOff** : jamais appelés par le jeu (tout est interne
  à D3D). Le jeu a son propre front buffer (texture this+88) et ses RT créés par CreateSurface.
- **StretchRect** : absent (Xbox → Resolve).
- Les autres Set* (états, constantes) écrivent directement l'ombre registres + bits dirty (+0..+47) ; le vidage se fait
  dans `821B68E8` / `821BBF88` au draw/clear. Si Clear/Resolve sont réalisés en natif, ces fonctions restent à neutraliser
  ou à doubler pour ne plus émettre de PM4.

## 5. Trous

- Noms exacts de `821B7538` (VBlank/synchro ?), `821B7700`/`821B7780` (flip/scaler), `821CC7D8` (fil).
- EndTiling : rôle de r8 (= 0x82000D00 dans `821073F8`, pointeur et non valeur de stencil ?) non élucidé.
- Valeurs exactes des bits Flags de Clear/Resolve non vérifiées sur un XDK (0x300 déduit de la doc publique).
- Pas d'InsertFence public : à confirmer qu'aucun appel indirect (`bctrl`) n'atteint ces fonctions.
