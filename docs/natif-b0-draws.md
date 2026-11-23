# D3D natif — états et draws (analyse statique)

Analyse **statique** du D3D9 Xbox 360 lié dans `default.xex` (XDK 2.0.20353, sans symboles) : désassemblage
`work/game/patched/text.dis`, données `image.bin` (base 0x82000000). Rien n'a été exécuté.
Confiance : **sûr** = forme + champs + paquets concordent ; **probable** = forme concordante, un détail non vérifié ;
**hypothèse** = rôle déduit des registres seulement.

Convention : `dev` = r3 (pointeur `D3DDevice`), décalages en hexadécimal depuis `dev`. Registres Xenos entre crochets.

## 1. Fonctions

| Fonction | Adresse | Signature (r3, r4, ...) | Appels `bl` | Confiance |
|---|---|---|---|---|
| DrawVertices (≈ DrawPrimitive) | `821B1F28` | (dev, primType, startVertex, vertexCount) | 6 | sûr |
| DrawIndexedVertices (≈ DrawIndexedPrimitive) | `821B2340` | (dev, primType, baseVertexIndex, startIndex, indexCount) | 8 (5 fonctions) | sûr |
| DrawVerticesUP (≈ DrawPrimitiveUP) | `821B1900` | (dev, primType, vertexCount, pVertexData, stride) | 6 | sûr |
| DrawIndexedVerticesUP | `821B1E88` | (dev, prim, minVertexIndex, numVertices, indexCount, pIndexData, indexFormat, pVertexData, stride=[sp+0x54]) | 1 | probable |
| BeginVertices | `821B1440` | (dev, primType, vertexCount, stride) → r3 = pointeur d'écriture | 2 (DrawVerticesUP, XUI `822CF798`) | sûr |
| EndVertices | `821B1430` | (dev) : `dev+0x30 = dev+0x3594` | 1 (`822CF7E0`) ; ailleurs inline | sûr |
| BeginIndexedVertices | `821B1948` | (dev, prim, -minIndex, numVertices, indexCount, indexFormat, stride, &pVB, &pIB [sp+0x54]) | 1 | probable |
| Vidage des états sales (autonome) | `821B9390` | (dev) | 1 (`821B95F0`, hors draws) | sûr |
| Vidage shaders + liaison VS/déclaration | `821C05C8` | (dev, dirty[2]) → dirty[2] mis à jour | 5 (les 4 draws + 821B9390) | sûr |
| SetTexture | `821AA658` | (dev, sampler, pTexture, masque64 = 1<<(31-sampler)) | 54 | sûr |
| SetSamplerState (table) | `dev+0x1D4+state` → setters `821B4AC0`… | (dev, sampler, valeur) | 65 sites indirects env. | sûr |
| SetRenderState (table) | `dev+0x40+state` → setters `821B31D0`… | (dev, valeur) | indirects + directs (cf. §3) | sûr |
| SetVertexShader | `821AC6D0` | (dev, pVS) | 16 | sûr |
| SetPixelShader | `821AC510` | (dev, pPS) | 17 | sûr |
| SetVertexShaderConstantF | `821AC168` | (dev, startReg, pData, vec4Count, masque64) | 34 (+ écritures inline) | sûr |
| SetPixelShaderConstantF | `821AC240` | (dev, startReg, pData, vec4Count, masque64) | 12 (+ inline) | sûr |
| SetVertexShaderConstantB / PS B | `821AC318` / `821AC378` | (dev, startReg, pBOOL, count) | 3 / 3 | sûr |
| SetVertexShaderConstantI / PS I | `821AC3D8` / `821AC430` | (dev, startReg, pInt4, count) | 3 / 3 | sûr |
| SetStreamSource | `821B4EB8` | (dev, stream, pVB, offset, stride, masque64) | 24 | sûr |
| SetIndices | `821B5060` | (dev, pIB) | 20 | sûr |
| SetVertexDeclaration | `821AC8A0` | (dev, pDecl) | 19 | sûr |
| SetFVF | `821AE198` | (dev, fvf) : décl. intégrée à `dev+0x3098`, via `821AA7E8` | 1 | probable |
| CreateVertexDeclaration | `821AC9A8` (init `821AC8B8`) | (pElements) → pDecl | 11 | sûr |
| CreateVertexShader | `821AE0A8` (init `821ADFB0`) | (pFunction) → pVS | 6 | sûr |
| CreatePixelShader | `821ADEC0` (init `821AC4D8`) | (pFunction) → pPS | 7 | sûr |
| Initialisation des états (tables + valeurs par défaut) | `821C89D8` | (dev) | 2 (`821C9148`, `821C93B0`) | sûr |

Aides du vidage : `821BF7A0` constantes ALU (type 0 vers 0x4000/0x4400), `821BF408` bloc de registres d'après un
masque (type 0), `821BF648` constantes de lecture (fetch) 0x4800, `821BEDC0` copie + réserve, `821BE678` réserve de
place dans le tampon (KickOff, voir l'agent présentation), `821BF4C8` probable plans de découpe utilisateur (6 bits, 6×vec4
depuis `dev+0x2820`, WAIT_REG_MEM avant), `821BBF88` probable invalidation des caches (type 0 [0x05C8 WAIT_UNTIL], [0x0E00],
[0x0E40]), `821BF070` hypothèse : rendu en tuiles (SET_BIN_MASK/SELECT 0x60-0x63, SET_CONSTANT, `dev+0x3190..`).
`821C0340` émet LOAD_ALU_CONSTANT (0x2F : constantes littérales du shader), `821C01F0` IM_LOAD_IMMEDIATE, `821C05C8` IM_LOAD
(0x27 : microcode par adresse).

## 2. Disposition du device (`D3DDevice`)

Identique à `GuestDevice` d'UnleashedRecomp (`gpu/video.h`) jusqu'à 0x27A0 ; seuls les champs plus loin diffèrent.

| Décalage | Contenu | Preuve |
|---|---|---|
| 0x00 | dirty[0] (u64) : constantes ALU VS, 1 bit = 4 vec4 | vidage → `821BF7A0(…, 0x4000, dev+0x780)` |
| 0x08 | dirty[1] : constantes ALU PS | → 0x4400 depuis `dev+0x1780` |
| 0x10 | dirty[2] : bits 17-20 shaders/déclaration (→ `821C05C8`) ; bits 0-11 [0x2200-0x220B] depuis 0x2934 ; bits 12-16 [0x2180-0x2184] depuis 0x2920 ; bits 21-41 [0x2100-0x2114] depuis 0x28CC ; bits 42-57 [0x2000-0x200F] depuis 0x2880 | vidage dans chaque draw |
| 0x18 | dirty[3] : bits 0-31 constantes de lecture 31…0 (bit 31-n = emplacement n) ; bits 34-54 [0x2280-0x2294] depuis 0x2964 | `821BF648`, SetTexture |
| 0x20 | dirty[4] : bits 38-45 [0x2380-0x2387] depuis 0x2A50 ; 49-54 plans de découpe ; 56 bool/boucle [0x4900-0x4927] depuis 0x2780 ; 0-37 [0x2300-0x2325] depuis 0x29B8 ; 62-63 `821BBF88` | idem |
| 0x28 | masque de tuiles (ET avec dirty[2] → `821BF070`) | hypothèse |
| 0x30 / 0x38 | tampon de commandes : position d'écriture / limite | tous les émetteurs |
| 0x40 | 101 setters SetRenderState (`dev+0x40+D3DRS`) | `821C89D8` |
| 0x1D4 | 20 setters SetSamplerState (`dev+0x1D4+D3DSAMP`) | idem |
| 0x224 / 0x3B8 | getters RS / SS | idem |
| 0x480 | 32 constantes de lecture × 24 o (texture n = `0x480+24n` ; flux s = constante de lecture de sommets 95-s, `0x778-8s`) | SetTexture, SetStreamSource |
| 0x780 / 0x1780 | constantes float VS / PS (256 vec4 chacune, gros-boutiste) | constantes F, vidage |
| 0x2780 / 0x2790 | bool VS / PS (4 mots chacun) | `821AC318`/`821AC378` |
| 0x27A0 / 0x27E0 | entiers (boucle) VS / PS | `821AC3D8`/`821AC430` |
| 0x2820 | plans de découpe (6 vec4) | `821BF4C8` |
| 0x2880-0x2A5F | **ombre des registres Xenos** (cf. dirty) : 0x2934 RB_DEPTHCONTROL, 0x2938 RB_BLENDCONTROL0, 0x293C RB_COLORCONTROL, 0x2944 PA_CL_CLIP_CNTL, 0x2948 PA_SU_SC_MODE_CNTL, 0x294C PA_CL_VTE_CNTL, 0x28DC RB_COLOR_MASK, 0x28E0-EC RB_BLEND_RGBA, 0x2900 RB_STENCILREFMASK, 0x28FC idem face arrière, 0x2904 RB_ALPHA_REF, 0x292C/0x2930 SQ_WRAPPING_0/1, 0x2964… PA_SU_POINT_SIZE/LINE_CNTL, 0x29C0 PA_SU_VTX_CNTL, 0x29C4-D0 PA_CL_GB_*, 0x2A00 PA_SC_AA_MASK, 0x2A50-5C PA_SU_POLY_OFFSET | setters RS (§3) |
| 0x2B3C (11068) | octet ; bit 0 = rendu en tuiles actif (les draws ajoutent SET_BIN_MASK 0x60, 0x5A, EVENT 0x46) | draws |
| 0x2F58 | déclaration courante (Unleashed : 0x2E2C) | SetVertexDeclaration, `821C05C8` |
| 0x2F68 | 16 octets : strides attendus par flux (déduits de la liaison VS/déclaration) | `821C05C8` |
| 0x3098 | déclaration intégrée pour SetFVF (en-tête type 5) | `821C89D8`, `821AE198` |
| 0x3188 / 0x318C | FVF / index buffer courant | SetFVF, SetIndices |
| 0x31A4+4s / 0x31E8+s | VB du flux s / stride (un octet, en mots de 4 o : 3 = 12 o) | SetStreamSource |
| 0x31F8+4n | texture de l'échantillonneur n (26 échantillonneurs) | SetTexture |
| 0x328C / 0x3290 | pixel shader / vertex shader courant | SetPS / SetVS |
| 0x3594 | position d'écriture après Begin*Vertices (EndVertices la recopie à 0x30) | 821B1430 |
| 0x2B1C / 0x2B20, 0x35E8 | marquage des ressources libérées (barrière) : table `821BC7D8` | Set* qui remplacent une ressource |

Objets : VB/IB `+0x18` adresse physique, `+0x1C` taille ; bit 31 de `IB+0` = index 32 bits. Texture `+0x1C..+0x33` =
GPUTEXTURE_FETCH_CONSTANT (6 mots) fusionné avec l'état d'échantillonneur du device. En-tête Common : type 5 décl.,
6 VS, 7 PS (mot 0 | 0x00100000).

## 3. SetRenderState / SetSamplerState : tables

Table statique RS en `0x82634148` : 101 entrées `{get, set, défaut}` de 12 octets (état D3DRS = index×4) ; table SS en
`0x82634608`, 20 entrées. `821C89D8` copie set → `dev+0x40+i*4`, get → `dev+0x224+i*4`, puis appelle chaque setter avec la
valeur par défaut (de même pour les 26 échantillonneurs, puis `SetTexture(dev, n, NULL)`). Les états 0-36 pointent sur
un setter vide `821D9C18`. Appel inline par le jeu : `lwz rX,0x40+state(dev) ; mtctr ; bctrl` (r4 = valeur) ; certains
setters sont aussi appelés directement (`bl`).

Setters (champ de l'ombre écrit, puis `dirty[k] |= bit`) — les noms suivent l'énumération D3DRS de la 360 (ZENABLE = 40)
et sont confirmés par les défauts (ZFUNC 3, CULLMODE 6, BLENDFACTOR/STENCILMASK 0xFFFFFFFF, POINTSIZE_MAX 64.0,
COLORWRITEENABLE 0xF, PRIMITIVERESETINDEX 0xFFFF…) et par les registres écrits :

| D3DRS | Setter | Champ | | D3DRS | Setter | Champ |
|---|---|---|---|---|---|---|
| 40 ZENABLE | 821B31D0 | 0x2934 | | 172 CLIPPLANEENABLE | 821B3550 | 0x2944 |
| 44 ZFUNC | 821B3240 | 0x2934 | | 176/180/188 POINTSIZE(_MIN/_MAX) | 821B3880/38D8/3928 | 0x2964-0x296A |
| 48 ZWRITEENABLE | 821B3210 | 0x2934 | | 184 POINTSPRITEENABLE | 821B3870 | 0x2F9C |
| 52 FILLMODE | 821B2AD8 | 0x2948 | | 192 MULTISAMPLEANTIALIAS | 821B3720 | 0x2948 |
| 56 CULLMODE | 821B2AA8 | 0x2948 | | 196 MULTISAMPLEMASK | 821B3750 | 0x2A00 |
| 60 ALPHABLENDENABLE | 821B2B40 | 0x2938, 0x2958-60 | | 200 SCISSORTESTENABLE | 821B56B8 | 0x2F80 |
| 64 SEPARATEALPHABLENDENABLE | 821B2ED0 | idem | | 204 SLOPESCALEDEPTHBIAS | 821B35B0 | 0x2948, 0x2A50/58 |
| 68 BLENDFACTOR | 821B2FF8 | 0x28E0-0x28EC | | 208 DEPTHBIAS | 821B3678 | 0x2948, 0x2A54/5C |
| 72 SRCBLEND | 821B2C60 | 0x2938… | | 212-224 COLORWRITEENABLE0-3 | 821B3770/37B0/37F0/3830 | 0x28DC |
| 76 DESTBLEND | 821B2CF0 | 0x2938… | | 228-236 TESSELLATION* | 821B3FF8/3F88/3FC0 | 0x2978-0x2980 |
| 80 BLENDOP | 821B2BD0 | 0x2938… | | 240-300 WRAP0-15 | 821B3978 … 821B3BB8 | 0x292C / 0x2930 |
| 84/88/92 *BLENDALPHA / BLENDOPALPHA | 821B2DF0/2E60/2D80 | 0x2938… | | 304 VIEWPORTENABLE | 821B3CD8 | 0x2944, 0x294C |
| 96 ALPHATESTENABLE | 821B2B08 | 0x293C | | 308-320 HIGHPRECISIONBLENDENABLE0-3 | 821B3D28/3DC0/3E58/3EF0 | 0x2884… |
| 100 ALPHAREF | 821B2F68 | 0x2904 | | 324 HALFPIXELOFFSET | 821B4030 | 0x29C0 |
| 104 ALPHAFUNC | 821B2FC8 | 0x293C | | 328/332 PRIMITIVERESET(ENABLE/INDEX) | 821B4068/4098 | 0x2948 / 0x28D8 |
| 108 STENCILENABLE | 821B3278 | 0x2934 | | 336/340 ALPHATOMASK* | 821B40C0/40F0 | 0x293C |
| 112-128 TWOSIDED/STENCIL FAIL/ZFAIL/PASS/FUNC | 821B32B8/3320/3358/3390/32F0 | 0x2934 | | 344-356 GUARDBAND/DISCARDBAND X/Y | 821B4118/4148/4178/41A0 | 0x29C4-0x29D0 |
| 132/136/140 STENCILREF/MASK/WRITEMASK | 821B3490/34B0/34D0 | octets 0x2903/02/01 | | 360-372 HISTENCIL* | 821B4268/4298/42C8/42F8 | 0x2940 |
| 144-156 CCW_STENCIL FAIL/ZFAIL/PASS/FUNC | 821B33F0/3428/3460/33C0 | 0x2934 | | 376 PRESENTINTERVAL | 821B4318 | 0x3630 |
| 160-168 CCW_STENCILREF/MASK/WRITEMASK | 821B34F0/3510/3530 | octets 0x28FF/FE/FD | | 384/388 HIZENABLE/HIZWRITEENABLE | 821B41D0/4200 | 0x2F7C |
| | | | | 392 LASTPIXEL / 396 LINEWIDTH / 400 BUFFER2FRAMES | 821B30F8/3130/31A0 | 0x29B8 / 0x296E / 0x5EC8 |

SetSamplerState (dev, sampler, valeur), état = index×4 : ADDRESSU 821B4AC0, ADDRESSV 821B4B10, ADDRESSW 821B4B60,
BORDERCOLOR 821B4A50, MAGFILTER 821B44F0, MINFILTER 821B4348, MIPFILTER 821B4690, MIPMAPLODBIAS 821B48B0, MAXMIPLEVEL
821B4950, MAXANISOTROPY 821B4790, MAGFILTERZ 821B45F0, MINFILTERZ 821B4448, SEPARATEZFILTERENABLE 821B46E8, MINMIPLEVEL
821B49D0, TRILINEARTHRESHOLD 821B4BB0, ANISOTROPYBIAS 821B4808, H/VGRADIENTEXPBIAS 821B4C08/4C60, WHITEBORDERCOLORW
821B4CB8, POINTBORDERENABLE 821B4D10. Tous modifient la constante de lecture `dev+0x480+24·sampler` et `dirty[3]`
(noms des états ≥ 40 : hypothèse fondée sur l'ordre de l'énumération ; ADDRESS*/MAG/MIN/MIPFILTER : probable).

## 4. Preuves clés

- **Draws** : seuls `821B1440/1948/1F28/2340` émettent DRAW_INDX (type 3, opcode 0x22). DrawVertices écrit en type 0
  [0x2102 VGT_INDX_OFFSET] = startVertex, puis `0xC0012201`, mot de requête, `VGT_DRAW_INITIATOR = count<<16 | 0x80
  (index automatiques) | prim`. DrawIndexedVertices : `0xC0032201`, adresse = `IB+0x18 + startIndex·2` (·4 et bit 11 si
  IB 32 bits), taille ; [0x2102] = baseVertexIndex. Découpage au-delà de 65535 par la table `0x82000798` (8 o par
  primitive). Exemple d'appel du jeu (`82101470`-`82101520`) : SetVertexShaderConstantF(dev,16,p,4,masque),
  écriture **inline** `stfs → dev+0x1780` + `dirty[1] |= …`, SetPixelShaderConstantF(dev,24,p,2), SetTexture(dev,0,t,
  0x80000000), DrawVertices(dev, 6 = TRISTRIP, 0, 4).
- **Vidage** : il est **inline en tête de chaque draw** (même code que `821B9390`, qui n'est appelé que par `821B95F0`) :
  `ld dirty[0..4]`, pour chaque mot non nul une aide, puis `std 0`. Les constantes et l'ombre des registres partent en
  paquets type 0 copiés depuis le device ; pas de SET_CONSTANT pour les draws.
- **SetTexture** fusionne les 6 mots de lecture de la texture avec les bits d'échantillonneur de `dev+0x480+24n`, fixe
  `dirty[3] |= r6`, libère l'ancienne texture sous barrière ; forme identique au hook `SetTexture` d'Unleashed, plus un
  4e argument (masque 64 bits précalculé par l'appelant).
- **SetStreamSource** écrit la constante de lecture de sommets 95-s (adresse | 3, taille) ; signature Unleashed +
  masque. **SetIndices** stocke seulement le pointeur (`dev+0x318C`).
- **SetVS / SetPS** stockent le shader, posent dirty[2] bit 19 (VS) / bits 17+20 (PS), et recopient dans l'ombre
  `dev+0x480+…` les constantes littérales du shader (table à `VS+0x368+[+0x14]`, `PS+0x28+[+0x3C]`).
  `821C05C8` lie VS et déclaration (entrées de 416 o dans l'objet VS, microcode corrigé des vertex fetch) puis IM_LOAD.
- **Format du microcode (CreateVertexShader/PixelShader)** : `pFunction` = conteneur `.xvu/.xpu` : mot 0 = drapeaux
  (`0x102A1101` VS / `0x102A1100` PS, bit 0 = VS), mot 1 = taille de l'en-tête (le microcode commence à
  `pFunction + mot1`), mot 2 = taille du microcode. L'en-tête est copié dans l'objet (VS : 0x368 o + en-tête ; PS :
  0x28 o + en-tête), le microcode dans une allocation physique (`821A1148`, attributs 0xB5800000). Crochet optionnel
  avant création : pointeurs `0x8266F460` (PS) / `0x8266F464` (VS). C'est le format qu'Unleashed reçoit
  (`CreateVertexShader(const be<uint32_t>* function)`) et que XenosRecomp lit.
- **CreateVertexDeclaration** : compte les D3DVERTEXELEMENT9 (12 o, fin = Stream 0xFF), alloue `56 + 12n`, en-tête
  `0x00100005`, n à +0x18, flux max à +0x1C, éléments recopiés à +0x34.

## 5. Pour les crochets natifs

- Draws : crochet `PPC_FUNC` sur les 4 fonctions de draw + `821B1440`/`821B1948`/`821B1430` (pour UP, le jeu écrit les
  sommets entre Begin et End ; DrawVerticesUP/IndexedUP sont des enveloppes de Begin + memcpy + End inline).
- Constantes : le jeu les écrit **aussi en ligne** dans `dev+0x780/0x1780` ; il faut donc les lire dans le device au
  moment du draw, d'après dirty[0]/[1] (comme Unleashed), pas seulement intercepter SetVertexShaderConstantF.
- États de rendu : soit remplacer les entrées `dev+0x40…`/`dev+0x1D4…` après `821C89D8` (méthode d'Unleashed dans son
  CreateDevice), en sachant que quelques setters sont aussi appelés par `bl` direct (crochet sur l'adresse du setter
  alors), soit décoder l'ombre des registres (§2) au draw, ce que le traducteur PM4 actuel sait déjà faire.
- Les draws internes (DRAW_INDX_2 dans `821ADD90`, `821B5E28`, `821BAD00`, `821BAE58`, `821C3C70`, `821CAB50`) relèvent
  de clear/resolve (autre agent).
