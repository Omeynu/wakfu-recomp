// Journal des appels d'imports, stubs génériques et déclaration des implémentations.
#pragma once
#include "../guest.h"

namespace wk {

// Stub générique : journalise (nom, r3-r6) et retourne 0.
void StubCall(const char* name, uint32_t thunk, PPCContext& ctx);
// À appeler en tête de chaque import implémenté (journal si WAKFU_TRACE=1, anneau des derniers appels).
void Trace(const char* name, PPCContext& ctx);
// Derniers imports appelés (affichés en cas de plantage).
void DumpRecentCalls();

}  // namespace wk

#define WK_IMPORT(Name) PPC_FUNC(__imp__##Name)
#define WK_TRACE(Name) ::wk::Trace(#Name, ctx)
