// Couche plateforme : ce qui diffère entre Linux et Windows.
#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <chrono>

namespace wk::platform {

// Initialisation : sorties en mode binaire (journaux identiques sous Linux et Windows).
void Init();
// Windows : journal (stderr) vers ce fichier puis console fermée ; ailleurs : sans effet.
void DetachConsole(const std::string& journal);

// Réserve et rend accessible (lecture/écriture) une zone de `size` octets ; nullptr si échec.
// Windows : la totalité est engagée (comptée dans la mémoire virtuelle du système).
void* ReserveGuestMemory(uint64_t size);

// Erreur bloquante montrée au joueur : boîte de message sous Windows (le journal la reçoit aussi) ; ailleurs, sans effet.
void ShowError(const std::string& title, const std::string& message);

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

// Lance un outil (args[0] = chemin), sorties jetées, et attend sa fin : code de sortie, -1 si lancement impossible ou
// arrêt anormal. `libDir` (Linux, peut être nul) : LD_LIBRARY_PATH de l'outil. Windows : priorité basse, sans fenêtre.
int RunTool(const std::vector<std::string>& args, const char* libDir);

// Dossier de l'exécutable (Linux /proc/self/exe, Windows GetModuleFileNameW) ; vide si inconnu ou non représentable.
std::string ExeDir();

// Temps processeur du thread appelant, en ms (statistiques).
double ThreadCpuMs();

// Diagnostic : demande la pile de chaque thread enregistré (Linux seulement ; sans effet sous Windows).
void RegisterCurrentThreadForDump(unsigned id);
void DumpAllThreadsAfter(int seconds);

// Attend jusqu'à l'échéance t. Sous Windows : minuteur haute résolution (≈ 0,5 ms) au lieu de la résolution par
// défaut du système (15,6 ms), trop grossière pour la synchro verticale à 16,7 ms.
void SleepUntil(std::chrono::steady_clock::time_point t);

}  // namespace wk::platform
