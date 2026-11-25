// Option [entrees] icones : icônes de boutons PlayStation dessinées par le runtime (aucune image tierce), substituées
// au chargement des textures d'icônes Xbox du jeu (images PNG du paquet XUI, reconnues à l'empreinte de leurs texels).
#pragma once
#include <cstdint>
#include "natif/textures.h"

namespace wk::natif {

// Style voulu (fil de la fenêtre) : vrai = PlayStation. Vaut pour les textures chargées ensuite.
void ChoisirIcones(bool playstation);
// Fil natif, après conversion (texels BGRA du format k_8_8_8_8, niveau l à dst + debut[l]) : en mode PlayStation,
// remplace une icône reconnue (tous les niveaux). Vrai si remplacée.
bool RemplacerIcone(const Disposition& d, uint8_t* dst, const uint32_t debut[]);
// Dessin seul (tests) : icône `forme` (0 croix, 1 rond, 2 carré, 3 triangle, 4 L1, 5 R1, 6 L2, 7 R2, 8 Share)
// dans le rectangle [x0, x1[ x [y0, y1[ d'une image BGRA w x h.
void DessinerIcone(int forme, uint8_t* bgra, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1);

}  // namespace wk::natif
