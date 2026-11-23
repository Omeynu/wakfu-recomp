// Verrou D3D générique des textures : le CPU va écrire la ressource (génération de la texture native, N1). Compté :
// verrou d'une destination de resolve natif, dont le contenu n'est pas recopié en mémoire invitée (relecture CPU
// d'un resolve, aucune en jeu à N6 ; à suivre si elle apparaît).
#include <cstdio>
#include "guest.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "natif/device.h"
#include "natif/textures.h"
#endif

// LockRect générique (texture, surface) : (pBaseTex, ArrayIndex, Face, Level, D3DLOCKED_RECT*, pRect, Flags)
PPC_FUNC_IMPL(__imp__sub_821A99D0);
PPC_FUNC(sub_821A99D0) {
    [[maybe_unused]] const uint32_t tex = ctx.r3.u32;
    __imp__sub_821A99D0(ctx, base);
#ifdef WAKFU_HAS_SHADERGEN
    static uint32_t relus = 0;   // fil du jeu seul
    if (wk::natif::DestinationResolve(tex) && ++relus <= 5)
        fprintf(stderr, "[verrou] relecture CPU d'un resolve du GPU (texture %08X), %u\n", tex, relus);
    wk::natif::TextureEcrite(tex);
#endif
}
