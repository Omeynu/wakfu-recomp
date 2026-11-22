// Couche plateforme : ce qui diffère entre Linux et Windows.
#pragma once
#include <string>
#include <cstddef>
#include <cstdint>

namespace wk::platform {

// Initialisation : sorties en mode binaire (journaux identiques sous Linux et Windows).
void Init();
// Windows : journal (stderr) vers ce fichier puis console fermée ; ailleurs : sans effet.
void DetachConsole(const std::string& journal);

// Réserve et rend accessible (lecture/écriture) une zone de `size` octets ; nullptr si échec.
// Windows : la totalité est engagée (comptée dans la mémoire virtuelle du système).
void* ReserveGuestMemory(uint64_t size);

// Surveillance des écritures dans la mémoire physique invitée (512 Mo, vues 0xA0000000 et 0xC0000000), par pages
// de 4 Ko : WatchProtect passe une plage en lecture seule ; la première écriture (code invité ou hôte) lève une faute
// qui rend la page inscriptible et augmente son numéro d'écriture. WatchGen : plus grand numéro de la plage.
// WatchTouch : numéro augmenté sans faute ; `writable` : plage rendue inscriptible (écriture système à venir, ex. lecture
// de fichier) ; sans : écriture du GPU (resolve), protection inchangée.
void WatchInit(uint8_t* guestBase);
void WatchProtect(uint32_t physStart, uint32_t physEnd);
uint64_t WatchGen(uint32_t physStart, uint32_t physEnd);
void WatchTouch(uint32_t physStart, uint32_t physEnd, bool writable);
// Faute à `addr` due à la surveillance : page rendue inscriptible, vrai (sinon : vraie faute).
bool WatchFault(uintptr_t addr);
uint64_t WatchFaults();
// Numéro global : change à chaque écriture surveillée ou WatchTouch (si inchangé, aucune plage n'a changé).
uint64_t WatchSerial();   // fautes traitées depuis le lancement (statistiques)

// Plantage : `onCrash(signal ou code d'exception, adresse fautive)` puis fin du processus.
using CrashCallback = void (*)(int code, uintptr_t faultAddress);
void InstallCrashHandler(CrashCallback onCrash);
// Pile d'appels hôte sur stderr (noms de fonctions sous Linux, adresses sous Windows).
void PrintBacktrace();

// Fichiers : descripteur entier, lecture/écriture à une position (sans déplacer de curseur partagé).
int OpenFile(const char* path, bool write, bool create, bool truncate);
int64_t ReadAt(int fd, void* buf, uint64_t size, uint64_t offset);
int64_t WriteAt(int fd, const void* buf, uint64_t size, uint64_t offset);
bool Truncate(int fd, uint64_t size);
void CloseFile(int fd);

// Diagnostic : demande la pile de chaque thread enregistré (Linux seulement ; sans effet sous Windows).
void RegisterCurrentThreadForDump(unsigned id);
void DumpAllThreadsAfter(int seconds);

}  // namespace wk::platform
