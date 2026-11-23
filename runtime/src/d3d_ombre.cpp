// Crochets sur les fonctions D3D du jeu (inventaire docs/natif.md) : rendu natif (natif/device.h) ; WAKFU_OMBRE=1
// compte aussi les appels par image et journalise les appels rares (détail des 3 premières images et des
// resolves/créations des 600 premières : WAKFU_OMBRE=2). shortcut: compteurs non atomiques (D3D appelé par un seul
// thread de rendu), sinon std::atomic.
#include <cstdio>
#include <cstdlib>
#include "guest.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "natif/device.h"
#include "natif/interfaces.h"
#include "natif/tampons.h"
#include "natif/textures.h"
#endif

namespace {
const int g_ombre = getenv("WAKFU_OMBRE") ? atoi(getenv("WAKFU_OMBRE")) : 0;
uint32_t g_frame = 0;

enum Fn { kDraw, kDrawIdx, kDrawUP, kDrawIdxUP, kClear, kResolve, kSetRT, kSetDS, kSetTex, kSetStream, kSetIdx, kSetDecl,
          kSetVS, kSetPS, kCreateTex, kCreateSurf, kCreateVB, kCreateIB, kUnlockTex, kUnlockSurf, kUnlockVB, kUnlockIB,
          kD3dxMem, kD3dxFile, kCount };
const char* const kName[kCount] = {"DrawVertices", "DrawIndexedVertices", "DrawVerticesUP", "DrawIndexedVerticesUP",
    "Clear", "Resolve", "SetRenderTarget", "SetDepthStencil", "SetTexture", "SetStreamSource", "SetIndices",
    "SetVertexDeclaration", "SetVertexShader", "SetPixelShader", "CreateTexture", "CreateSurface", "CreateVertexBuffer",
    "CreateIndexBuffer", "UnlockRect(tex)", "UnlockRect(surf)", "Unlock(VB)", "Unlock(IB)", "D3DXTexFromMemory",
    "D3DXTexFromFile"};
uint32_t g_calls[kCount];
uint64_t g_verts, g_indices;   // somme des sommets (non indexés, UP) et des indices de l'image

bool Detail(Fn f) {
    if (g_ombre < 2) return false;
    if (g_frame < 3) return true;
    return g_frame < 600 && (f == kResolve || f == kClear || f == kSetRT || f == kSetDS || f >= kCreateTex);
}

void Note(Fn f, const PPCContext& ctx) {
    if (f >= kCount) return;   // crochet sans compteur (créations de shaders)
    g_calls[f]++;
    if (f == kDraw) g_verts += ctx.r6.u32;
    else if (f == kDrawUP) g_verts += ctx.r5.u32;
    else if (f == kDrawIdx) g_indices += ctx.r7.u32;
    else if (f == kDrawIdxUP) g_indices += ctx.r7.u32;
    if (!Detail(f)) return;
    if (f == kResolve) {   // destination : adresse de base du fetch constant de la texture (+32)
        const uint32_t tex = ctx.r6.u32, dst = tex ? wk::Ld32(tex + 32) & 0xFFFFF000u : 0;
        fprintf(stderr, "[ombre %u] Resolve flags %08X rect %08X dest %08X (base %08X) niveau %u\n", g_frame, ctx.r4.u32,
                ctx.r5.u32, tex, dst, ctx.r8.u32);
        return;
    }
    fprintf(stderr, "[ombre %u] %s %08X %08X %08X %08X %08X\n", g_frame, kName[f], ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
            ctx.r7.u32, ctx.r8.u32);
}

void EndFrame() {
    if (g_frame % 60 == 0 || g_frame < 3) {
        char line[1024];
        int n = snprintf(line, sizeof line, "[ombre %u] sommets %llu, indices %llu ;", g_frame,
                         (unsigned long long)g_verts, (unsigned long long)g_indices);
        for (int i = 0; i < kCount && n < int(sizeof line) - 48; i++)
            if (g_calls[i]) n += snprintf(line + n, sizeof line - n, " %s %u", kName[i], g_calls[i]);
        fprintf(stderr, "%s\n", line);
    }
    for (uint32_t& c : g_calls) c = 0;
    g_verts = g_indices = 0;
    g_frame++;
}
}  // namespace

#define OMBRE(addr, fn)                                \
    PPC_FUNC_IMPL(__imp__sub_##addr);                  \
    PPC_FUNC(sub_##addr) {                             \
        if (g_ombre) Note(fn, ctx);                    \
        __imp__sub_##addr(ctx, base);                  \
    }

#ifdef WAKFU_HAS_SHADERGEN
using wk::natif::Instantane;
// Draws, effacements et resolves : rendus par le natif seul, l'original n'est plus appelé (il n'écrivait que du PM4 de
// rendu, ignoré par le CP) ; les bits dirty des constantes sont vidés par natif::SynchroConstantes.
static void NatifDraw(const PPCContext& ctx, uint32_t type) {
    auto d = std::make_unique<Instantane>();
    d->type = type;
    d->prim = ctx.r4.u32;
    uint32_t sommetsUP = 0, indicesUP = 0;
    switch (type) {
        case 0: d->debut = ctx.r5.u32; d->nombre = ctx.r6.u32; break;
        case 1: d->baseSommet = int32_t(ctx.r5.u32); d->debut = ctx.r6.u32; d->nombre = ctx.r7.u32; break;
        case 2: d->nombre = d->sommetsUP = ctx.r5.u32; sommetsUP = ctx.r6.u32; d->pasUP = ctx.r7.u32; break;
        default:   // (dev, prim, minIndex, nbSommets, nbIndices, pIndices, formatIndices, pSommets, stride [sp+0x54])
            d->baseSommet = int32_t(ctx.r5.u32); d->sommetsUP = ctx.r6.u32; d->nombre = ctx.r7.u32;
            indicesUP = ctx.r8.u32; d->formatIndicesUP = ctx.r9.u32; sommetsUP = ctx.r10.u32;
            d->pasUP = wk::Ld32(ctx.r1.u32 + 0x54);
    }
    wk::natif::Dessiner(ctx.r3.u32, std::move(d), sommetsUP, indicesUP);
}
#define NATIF_SEUL(addr, fn, natif)                    \
    PPC_FUNC_IMPL(__imp__sub_##addr);                  \
    PPC_FUNC(sub_##addr) {                             \
        if (g_ombre) Note(fn, ctx);                    \
        natif;                                         \
    }
#define NATIF_AVANT(addr, fn, natif)                   \
    PPC_FUNC_IMPL(__imp__sub_##addr);                  \
    PPC_FUNC(sub_##addr) {                             \
        if (g_ombre) Note(fn, ctx);                    \
        natif;                                         \
        __imp__sub_##addr(ctx, base);                  \
    }
NATIF_SEUL(821B1F28, kDraw, NatifDraw(ctx, 0))
NATIF_SEUL(821B2340, kDrawIdx, NatifDraw(ctx, 1))
NATIF_SEUL(821B1900, kDrawUP, NatifDraw(ctx, 2))
NATIF_SEUL(821B1E88, kDrawIdxUP, NatifDraw(ctx, 3))
// XUI (822CF798 : objet, prim, nombre, pas, &pointeur ; device en objet+12) appelle BeginVertices sans passer par
// DrawVerticesUP et écrit les sommets avant EndVertices (822CF7E0) : état relevé au Begin, sommets lus au End.
static std::unique_ptr<Instantane> g_xui;
static uint32_t g_xuiSommets;
PPC_FUNC_IMPL(__imp__sub_822CF798);
PPC_FUNC(sub_822CF798) {
    const uint32_t sortie = ctx.r7.u32;
    g_xui = std::make_unique<Instantane>();
    g_xui->type = 2;
    g_xui->prim = ctx.r4.u32;
    g_xui->nombre = g_xui->sommetsUP = ctx.r5.u32;
    g_xui->pasUP = ctx.r6.u32;
    wk::natif::Relever(wk::Ld32(ctx.r3.u32 + 12), g_xui.get());
    __imp__sub_822CF798(ctx, base);
    g_xuiSommets = ctx.r3.u32 == 0 ? wk::Ld32(sortie) : 0;   // r3 : HRESULT
}
PPC_FUNC_IMPL(__imp__sub_822CF7E0);
PPC_FUNC(sub_822CF7E0) {
    if (g_xui && g_xuiSommets) wk::natif::Dessiner(0, std::move(g_xui), g_xuiSommets, 0);
    g_xui.reset();
    __imp__sub_822CF7E0(ctx, base);
}
// Resolve (dev, flags, rect, texture, point, niveau, tranche, couleur, f1 Z, [sp+92] stencil)
NATIF_SEUL(821BA740, kResolve, wk::natif::Resoudre(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                                     ctx.r10.u32, float(ctx.f1.f64), wk::Ld32(ctx.r1.u32 + 92)))
NATIF_AVANT(821B57F8, kSetRT, wk::natif::CibleCouleur(ctx.r4.u32, ctx.r5.u32))   // l'alias 821B5E20 du jeu y saute (b)
NATIF_AVANT(821B5B48, kSetDS, wk::natif::CibleProfondeur(ctx.r4.u32))
NATIF_AVANT(821AC6D0, kSetVS, wk::natif::ShaderVu(ctx.r4.u32, false))
NATIF_AVANT(821AC510, kSetPS, wk::natif::ShaderVu(ctx.r4.u32, true))
// ClearF (dev, flags, rect, couleur vec4*, f1 Z, r8 stencil) : cœur de Clear (par rectangle) et de BeginTiling
PPC_FUNC_IMPL(__imp__sub_821B6F68);
PPC_FUNC(sub_821B6F68) { wk::natif::Effacer(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r8.u32); }
// Vidage autonome des états sales (cœur du Resolve) : constantes à relever avant qu'il ne remette dirty à zéro
PPC_FUNC_IMPL(__imp__sub_821B9390);
PPC_FUNC(sub_821B9390) {
    wk::natif::SynchroConstantes(ctx.r3.u32);
    __imp__sub_821B9390(ctx, base);
}
// Créations : image native d'après les arguments, une fois l'en-tête rendu (r3)
PPC_FUNC_IMPL(__imp__sub_821AA2D8);
PPC_FUNC(sub_821AA2D8) {
    const uint32_t usage = ctx.r7.u32;
    if (g_ombre) Note(kCreateTex, ctx);
    __imp__sub_821AA2D8(ctx, base);
    wk::natif::CreationTexture(ctx.r3.u32, usage);
}
PPC_FUNC_IMPL(__imp__sub_821AA3F8);
PPC_FUNC(sub_821AA3F8) {
    const uint32_t w = ctx.r3.u32, h = ctx.r4.u32, fmt = ctx.r5.u32, ms = ctx.r6.u32;
    if (g_ombre) Note(kCreateSurf, ctx);
    __imp__sub_821AA3F8(ctx, base);
    wk::natif::CreationSurface(ctx.r3.u32, w, h, fmt, ms);
}
#else
OMBRE(821B1F28, kDraw)
OMBRE(821B2340, kDrawIdx)
OMBRE(821B1900, kDrawUP)
OMBRE(821B1E88, kDrawIdxUP)
OMBRE(821BA740, kResolve)
OMBRE(821B57F8, kSetRT)        // l'alias 821B5E20 du jeu y saute (b)
OMBRE(821B5B48, kSetDS)
OMBRE(821AC6D0, kSetVS)
OMBRE(821AC510, kSetPS)
OMBRE(821AA2D8, kCreateTex)
OMBRE(821AA3F8, kCreateSurf)
#endif
OMBRE(821B7090, kClear)
OMBRE(821AA658, kSetTex)
OMBRE(821B4EB8, kSetStream)
OMBRE(821B5060, kSetIdx)
OMBRE(821AC8A0, kSetDecl)
OMBRE(821BAB68, kCreateVB)
OMBRE(821BAC40, kCreateIB)
// UnlockRect (pTex) / (pSurf, parent en +24) : génération de la texture native (N1) après l'original
PPC_FUNC_IMPL(__imp__sub_821A86F8);
PPC_FUNC(sub_821A86F8) {
    [[maybe_unused]] const uint32_t tex = ctx.r3.u32;
    if (g_ombre) Note(kUnlockTex, ctx);
    __imp__sub_821A86F8(ctx, base);
#ifdef WAKFU_HAS_SHADERGEN
    wk::natif::TextureEcrite(tex);
#endif
}
PPC_FUNC_IMPL(__imp__sub_821A8710);
PPC_FUNC(sub_821A8710) {
    [[maybe_unused]] const uint32_t surf = ctx.r3.u32;
    if (g_ombre) Note(kUnlockSurf, ctx);
    __imp__sub_821A8710(ctx, base);
#ifdef WAKFU_HAS_SHADERGEN
    if (surf) wk::natif::TextureEcrite(wk::Ld32(surf + 24));
#endif
}
#ifdef WAKFU_HAS_SHADERGEN
// B3 : tampons natifs à reconvertir après Unlock, libérés à la destruction de la ressource
#define NATIF_APRES(addr, fn, natif)                   \
    PPC_FUNC_IMPL(__imp__sub_##addr);                  \
    PPC_FUNC(sub_##addr) {                             \
        const uint32_t r3 = ctx.r3.u32;                \
        if (g_ombre) Note(fn, ctx);                    \
        __imp__sub_##addr(ctx, base);                  \
        natif;                                         \
    }
NATIF_APRES(821BAC30, kUnlockVB, natif::OnUnlock(r3))
NATIF_APRES(821BACF0, kUnlockIB, natif::OnUnlock(r3))
// CreateVertexShader / CreatePixelShader (pFunction) -> objet : empreinte du conteneur (shaders précompilés)
NATIF_APRES(821AE0A8, kCount, wk::natif::ShaderCree(ctx.r3.u32, r3))
NATIF_APRES(821ADEC0, kCount, wk::natif::ShaderCree(ctx.r3.u32, r3))
PPC_FUNC_IMPL(__imp__sub_821BB548);
PPC_FUNC(sub_821BB548) {   // DestructResource (pRes)
    natif::OnDestroy(ctx.r3.u32);
    wk::natif::TextureDetruite(ctx.r3.u32);
    __imp__sub_821BB548(ctx, base);
}
#else
OMBRE(821BAC30, kUnlockVB)
OMBRE(821BACF0, kUnlockIB)
#endif
OMBRE(821CFD20, kD3dxMem)
OMBRE(821CFD90, kD3dxFile)

// Swap (seul appelant de VdSwap) : fin d'image côté jeu
PPC_FUNC_IMPL(__imp__sub_821B7968);
PPC_FUNC(sub_821B7968) {
    if (g_ombre) EndFrame();
#ifdef WAKFU_HAS_SHADERGEN
    wk::natif::Echange(ctx.r4.u32);   // r4 : texture affichée (destination du dernier resolve)
#endif
    __imp__sub_821B7968(ctx, base);
}
