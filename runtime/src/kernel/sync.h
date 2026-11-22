// Synchronisation noyau : un verrou global + une variable de condition.
// Tous les objets attendables sont des en-têtes « dispatcher » en mémoire invitée :
//   +0 type (u8) : 0 événement notification, 1 événement synchro, 2 mutant, 5 sémaphore, 6 thread
//   +4 SignalState (s32) ; sémaphore : +0x10 limite.
#pragma once
#include <condition_variable>
#include <mutex>
#include "../guest.h"

namespace wk {

extern std::mutex g_kernelLock;
extern std::condition_variable g_kernelCv;

enum class HandleKind { Object, File, Listener, Enumerator };
struct HandleEntry { HandleKind kind; uint32_t object; void* host; };

uint32_t NewHandle(HandleKind kind, uint32_t object, void* host = nullptr);
bool LookupHandle(uint32_t h, HandleEntry* out);       // gère 0xFFFFFFFE (thread courant)
bool CloseHandle(uint32_t h, HandleEntry* out);

// Signale un objet (verrou pris) puis réveille les attentes.
void SignalLocked(uint32_t object, int32_t state);
// Attente sur des objets invités. waitAll : tous ; sinon le premier. timeoutPtr : LARGE_INTEGER invité
// (0 = infini ; négatif = relatif en 100 ns ; positif = absolu). Retour : index ou STATUS_TIMEOUT.
uint32_t WaitObjects(const uint32_t* objects, uint32_t count, bool waitAll, uint32_t timeoutPtr);

uint32_t NewDispatcher(uint8_t type, int32_t signal, uint32_t extra = 0);

}  // namespace wk
