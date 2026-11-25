// Processus invité (pas « process.h » : nom pris par l'en-tête Windows inclus par SDL3) : module XEX, threads (contexte, PCR, KTHREAD, TLS).
#pragma once
#include <cstdint>
#include "guest.h"

namespace wk {

struct Module {
    uint32_t xexHeader = 0;      // copie de l'en-tête XEX en mémoire invitée
    uint32_t xexHeaderSize = 0;
    uint32_t ldrEntry = 0;       // X_LDR_DATA_TABLE_ENTRY
    uint32_t handleCell = 0;     // cellule pointée par la variable XexExecutableModuleHandle
};
extern Module g_module;

// Valeur d'un en-tête optionnel XEX (clé & 0xFF : 0 = valeur, 1 = adresse de la valeur, sinon adresse des données).
uint32_t XexHeaderField(uint32_t key, bool* found = nullptr);

struct GuestThread {
    uint32_t pcr = 0, kthread = 0, stackBase = 0, stackSize = 0, threadId = 0;
    uint32_t tls[64] = {};
    int suspendCount = 0;        // protégé par le verrou noyau (sync.h)
    bool resumeCredit = false;   // NtResumeThread reçu avant l'auto-suspension (réveil anticipé), idem
};
GuestThread* CurrentThread();
GuestThread* FindThreadByKthread(uint32_t kthread);
// Alloue pile, PCR et KTHREAD (en-tête dispatcher de type thread, non signalé).
GuestThread* CreateGuestThread(uint32_t stackSize);
// Lie le thread hôte courant au thread invité et prépare le contexte (r1, r13, FPSCR).
void AttachThread(GuestThread* t, PPCContext& ctx);

// Lance une fonction invitée (adresse) avec le contexte donné ; false si l'adresse est inconnue.
bool CallGuest(uint32_t address, PPCContext& ctx);

// Diagnostic : après WAKFU_DUMP_AFTER secondes, pile hôte de chaque thread invité.
void StartWatchdog();

}  // namespace wk
