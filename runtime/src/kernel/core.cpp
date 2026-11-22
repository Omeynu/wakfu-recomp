// Imports noyau/XAM de base : processus, module XEX, mémoire, TLS, temps, configuration, chaînes.
#include "../quit.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include "../process.h"
#include "stub.h"
#include "time_fields.h"

using namespace wk;

// ---------- Processus, privilèges ----------
WK_IMPORT(KeGetCurrentProcessType) { WK_TRACE(KeGetCurrentProcessType); Ret(ctx, 1); }  // processus titre
WK_IMPORT(KeSetCurrentProcessType) { WK_TRACE(KeSetCurrentProcessType); Ret(ctx, 0); }
WK_IMPORT(XexCheckExecutablePrivilege) {
    WK_TRACE(XexCheckExecutablePrivilege);
    // Privilèges = bits de SYSTEM_FLAGS (clé 0x00030000) ; privilège n -> bit n.
    const uint32_t priv = ctx.r3.u32, flags = XexHeaderField(0x00030000);
    Ret(ctx, priv < 32 ? (flags >> priv) & 1 : 0);
}
WK_IMPORT(ExRegisterTitleTerminateNotification) { WK_TRACE(ExRegisterTitleTerminateNotification); }
WK_IMPORT(KeEnterCriticalRegion) { WK_TRACE(KeEnterCriticalRegion); }
WK_IMPORT(KeLeaveCriticalRegion) { WK_TRACE(KeLeaveCriticalRegion); }
WK_IMPORT(KeBugCheck) {
    WK_TRACE(KeBugCheck);
    fprintf(stderr, "[KeBugCheck] code %08X\n", ctx.r3.u32);
    DumpRecentCalls();
    wk::Quit(wk::kExitGameError, "erreur fatale du jeu (KeBugCheck)", false);
}
WK_IMPORT(KeBugCheckEx) {
    WK_TRACE(KeBugCheckEx);
    fprintf(stderr, "[KeBugCheckEx] code %08X %08X %08X %08X %08X\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    DumpRecentCalls();
    wk::Quit(wk::kExitGameError, "erreur fatale du jeu (KeBugCheckEx)", false);
}
WK_IMPORT(DbgPrint) {
    WK_TRACE(DbgPrint);
    fprintf(stderr, "[DbgPrint] %s\n", ReadAnsi(ctx.r3.u32).c_str());   // format brut (args non substitués)
    Ret(ctx, 0);
}

// ---------- Module XEX ----------
WK_IMPORT(XexGetModuleHandle) {
    WK_TRACE(XexGetModuleHandle);
    const std::string name = ReadAnsi(ctx.r3.u32);
    if (ctx.r3.u32 == 0 || name == "default.xex") {
        St32(ctx.r4.u32, g_module.ldrEntry);
        Ret(ctx, STATUS_SUCCESS);
    } else {
        fprintf(stderr, "[XexGetModuleHandle] module inconnu : %s\n", name.c_str());
        St32(ctx.r4.u32, 0);
        Ret(ctx, STATUS_NOT_FOUND);
    }
}
WK_IMPORT(RtlImageXexHeaderField) {
    WK_TRACE(RtlImageXexHeaderField);
    bool found = false;
    uint32_t v = XexHeaderField(ctx.r4.u32, &found);
    fprintf(stderr, "[RtlImageXexHeaderField] clé %08X -> %s %08X\n", ctx.r4.u32, found ? "" : "absente", v);
    Ret(ctx, v);
}
WK_IMPORT(XexGetModuleSection) {
    WK_TRACE(XexGetModuleSection);
    // RESOURCE_INFO (0x2FF) : taille, puis entrées {nom[8], adresse, taille}.
    const std::string want = ReadAnsi(ctx.r4.u32, 8);
    bool found = false;
    const uint32_t ri = XexHeaderField(0x000002FF, &found);
    if (found) {
        const uint32_t n = (Ld32(ri) - 4) / 16;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t e = ri + 4 + 16 * i;
            if (ReadAnsi(e, 8) == want) {
                St32(ctx.r5.u32, Ld32(e + 8));
                St32(ctx.r6.u32, Ld32(e + 12));
                Ret(ctx, STATUS_SUCCESS);
                return;
            }
        }
    }
    fprintf(stderr, "[XexGetModuleSection] section absente : %s\n", want.c_str());
    Ret(ctx, STATUS_NOT_FOUND);
}

// ---------- Mémoire ----------
// Réservations virtuelles (base -> taille) : nécessaires pour MEM_RELEASE et pour rendre des pages à zéro.
namespace {
std::mutex g_vmMutex;
std::map<uint32_t, uint32_t> g_reservations;
}
WK_IMPORT(NtAllocateVirtualMemory) {
    WK_TRACE(NtAllocateVirtualMemory);
    // Signature Xbox (sans ZeroBits) : (PVOID* base, SIZE_T* taille, type, protection)
    // type : MEM_COMMIT 0x1000, MEM_RESERVE 0x2000, MEM_LARGE_PAGES 0x20000000 (64 Kio), MEM_HEAP 0x40000000,
    // MEM_16MB_PAGES 0x80000000 (constantes de Xenia)
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    const uint32_t want = Ld32(basePtr), size = (Ld32(sizePtr) + 0xFFF) & ~0xFFFu;
    if (want) {   // engagement dans une réservation : pages déjà présentes (et à zéro si rendues avant)
        St32(basePtr, want & ~0xFFFu);
        St32(sizePtr, size);
        Ret(ctx, STATUS_SUCCESS);
        return;
    }
    const uint32_t align = (type & 0x80000000) && !(type & 0x20000000) ? 0x1000000 : 0x10000;
    const uint32_t a = Alloc(Heap::Virtual, size, align);
    if (!a) { Ret(ctx, STATUS_NO_MEMORY); return; }
    {
        std::lock_guard<std::mutex> lock(g_vmMutex);
        g_reservations[a] = size;
    }
    St32(basePtr, a);
    St32(sizePtr, size);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtFreeVirtualMemory) {
    WK_TRACE(NtFreeVirtualMemory);
    // (PVOID* base, SIZE_T* taille, type) : MEM_DECOMMIT 0x4000 (pages rendues -> à zéro), MEM_RELEASE 0x8000
    // (réservation entière). L'espace d'adressage n'est pas réutilisé (tas à pointeur croissant).
    const uint32_t vbase = Ld32(ctx.r3.u32) & ~0xFFFu, type = ctx.r5.u32;
    uint32_t size = (Ld32(ctx.r4.u32) + 0xFFF) & ~0xFFFu;
    std::lock_guard<std::mutex> lock(g_vmMutex);
    auto it = g_reservations.upper_bound(vbase);
    if (it == g_reservations.begin()) { Ret(ctx, STATUS_INVALID_PARAMETER); return; }
    --it;
    const uint32_t resBase = it->first, resEnd = it->first + it->second;
    if (vbase >= resEnd) { Ret(ctx, STATUS_INVALID_PARAMETER); return; }
    if ((type & 0x8000) || size == 0) { size = resEnd - vbase; }
    if (vbase + size > resEnd) size = resEnd - vbase;
    memset(Host(vbase), 0, size);
    if (type & 0x8000) g_reservations.erase(resBase);
    St32(ctx.r4.u32, size);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(MmAllocatePhysicalMemoryEx) {
    WK_TRACE(MmAllocatePhysicalMemoryEx);
    // (flags, size, protect, min, max, alignment)
    uint32_t align = ctx.r8.u32 < 0x1000 ? 0x1000 : ctx.r8.u32;
    Ret(ctx, Alloc(Heap::Physical, ctx.r4.u32, align));
}
WK_IMPORT(MmFreePhysicalMemory) { WK_TRACE(MmFreePhysicalMemory); }
WK_IMPORT(MmGetPhysicalAddress) { WK_TRACE(MmGetPhysicalAddress); Ret(ctx, ctx.r3.u32 & 0x1FFFFFFF); }
// (inconnu, adresse physique, taille, protection) -> adresse invitée de la vue physique (0xA0000000 + adresse)
WK_IMPORT(MmMapIoSpace) {
    WK_TRACE(MmMapIoSpace);
    const uint32_t p = ctx.r4.u32;
    Ret(ctx, p < 0x20000000 ? 0xA0000000u | p : p);
}
WK_IMPORT(MmQueryAddressProtect) { WK_TRACE(MmQueryAddressProtect); Ret(ctx, 0x04); }  // PAGE_READWRITE
WK_IMPORT(XamAlloc) {
    WK_TRACE(XamAlloc);
    // (flags, size, void** out)
    const uint32_t a = Alloc(Heap::Virtual, ctx.r4.u32);
    St32(ctx.r5.u32, a);
    Ret(ctx, a ? 0 : 0x8007000E);
}
WK_IMPORT(XamFree) { WK_TRACE(XamFree); Ret(ctx, 0); }

// ---------- TLS ----------
namespace { std::mutex g_tlsMutex; uint64_t g_tlsUsed = 0; }
WK_IMPORT(KeTlsAlloc) {
    WK_TRACE(KeTlsAlloc);
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    for (uint32_t i = 0; i < 64; i++)
        if (!(g_tlsUsed >> i & 1)) { g_tlsUsed |= 1ull << i; Ret(ctx, i); return; }
    Ret(ctx, 0xFFFFFFFF);
}
WK_IMPORT(KeTlsFree) {
    WK_TRACE(KeTlsFree);
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    if (ctx.r3.u32 < 64) g_tlsUsed &= ~(1ull << ctx.r3.u32);
    Ret(ctx, 1);
}
WK_IMPORT(KeTlsGetValue) {
    WK_TRACE(KeTlsGetValue);
    Ret(ctx, ctx.r3.u32 < 64 ? CurrentThread()->tls[ctx.r3.u32] : 0);
}
WK_IMPORT(KeTlsSetValue) {
    WK_TRACE(KeTlsSetValue);
    if (ctx.r3.u32 >= 64) { Ret(ctx, 0); return; }
    CurrentThread()->tls[ctx.r3.u32] = ctx.r4.u32;
    Ret(ctx, 1);
}

// ---------- Temps ----------
// Base de temps du Xenon (mftb, QueryPerformanceCounter du XDK) : 50 MHz depuis le démarrage, jamais nulle (le XDK
// relit mftb si la partie basse vaut 0). XenonRecomp émettait __rdtsc() brut (≈ 2,9 GHz) : temps de jeu ≈ 58x trop
// rapide, script de la zone 1 bloqué sur WaitTime.
uint64_t WkQueryTimeBase() {
    static const auto start = std::chrono::steady_clock::now();
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) / 20 + 1;
}
WK_IMPORT(KeQueryPerformanceFrequency) { WK_TRACE(KeQueryPerformanceFrequency); Ret(ctx, 50000000); }
WK_IMPORT(KeQuerySystemTime) {
    WK_TRACE(KeQuerySystemTime);
    using namespace std::chrono;
    // FILETIME : centaines de ns depuis 1601
    const uint64_t t = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ull;
    St64(ctx.r3.u32, t);
}

// TIME_FIELDS invité : 8 entiers 16 bits gros-boutistes (année, mois, jour, heure, minute, seconde, ms, jour de semaine).
WK_IMPORT(RtlTimeToTimeFields) {
    WK_TRACE(RtlTimeToTimeFields);
    const TimeFields f = FileTimeToFields(Ld64(ctx.r3.u32));
    const int16_t v[8] = {f.year, f.month, f.day, f.hour, f.minute, f.second, f.milliseconds, f.weekday};
    for (int i = 0; i < 8; i++) St16(ctx.r4.u32 + 2 * i, uint16_t(v[i]));
}
WK_IMPORT(RtlTimeFieldsToTime) {
    WK_TRACE(RtlTimeFieldsToTime);
    int16_t v[8];
    for (int i = 0; i < 8; i++) v[i] = int16_t(Ld16(ctx.r3.u32 + 2 * i));
    uint64_t t = 0;
    const bool ok = FieldsToFileTime({v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]}, &t);
    if (ok) St64(ctx.r4.u32, t);
    Ret(ctx, ok ? 1 : 0);
}

// ---------- Configuration, système ----------
WK_IMPORT(ExGetXConfigSetting) {
    WK_TRACE(ExGetXConfigSetting);
    // (catégorie, réglage, tampon, taille, taille_requise*) ; réglages inconnus -> journal + échec
    const uint32_t cat = ctx.r3.u32, set = ctx.r4.u32, buf = ctx.r5.u32, size = ctx.r6.u32, req = ctx.r7.u32;
    fprintf(stderr, "[ExGetXConfigSetting] catégorie %u réglage %u taille %u\n", cat, set, size);
    uint32_t value = 0, len = 0;
    if (cat == 3 && set == 9) { value = 1; len = 4; }        // langue : anglais
    else if (cat == 3 && set == 10) { value = 0x00040000; len = 4; }  // drapeaux vidéo : 16:9
    else if (cat == 3) { value = 0; len = 4; }
    if (!len) { Ret(ctx, STATUS_OBJECT_NAME_NOT_FOUND); return; }
    if (buf && size >= len) St32(buf, value);
    if (req) St16(req, uint16_t(len));
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(XGetLanguage) { WK_TRACE(XGetLanguage); Ret(ctx, 2); }        // XC_LANGUAGE_FRENCH ? voir journal
WK_IMPORT(XGetAVPack) { WK_TRACE(XGetAVPack); Ret(ctx, 6); }            // HDMI
WK_IMPORT(XGetGameRegion) { WK_TRACE(XGetGameRegion); Ret(ctx, 0x00FF); }
WK_IMPORT(XamGetSystemVersion) { WK_TRACE(XamGetSystemVersion); Ret(ctx, 0x20004F81); }   // 2.0.20353

// ---------- Chaînes ----------
WK_IMPORT(RtlInitAnsiString) {
    WK_TRACE(RtlInitAnsiString);
    const uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    const uint16_t n = uint16_t(ReadAnsi(s, 0xFFFE).size());
    St16(d, n);
    St16(d + 2, s ? n + 1 : 0);
    St32(d + 4, s);
}
WK_IMPORT(RtlInitUnicodeString) {
    WK_TRACE(RtlInitUnicodeString);
    const uint32_t d = ctx.r3.u32, s = ctx.r4.u32;
    uint16_t n = 0;
    if (s) while (Ld16(s + 2 * n) && n < 0x7FFE) n++;
    St16(d, uint16_t(2 * n));
    St16(d + 2, s ? uint16_t(2 * n + 2) : 0);
    St32(d + 4, s);
}
WK_IMPORT(RtlFillMemoryUlong) {
    WK_TRACE(RtlFillMemoryUlong);
    for (uint32_t i = 0; i + 4 <= ctx.r4.u32; i += 4) St32(ctx.r3.u32 + i, ctx.r5.u32);
}
WK_IMPORT(RtlCompareMemoryUlong) {
    WK_TRACE(RtlCompareMemoryUlong);
    uint32_t i = 0;
    while (i + 4 <= ctx.r4.u32 && Ld32(ctx.r3.u32 + i) == ctx.r5.u32) i += 4;
    Ret(ctx, i);
}
WK_IMPORT(RtlNtStatusToDosError) {
    WK_TRACE(RtlNtStatusToDosError);
    const uint32_t s = ctx.r3.u32;
    Ret(ctx, s == 0 ? 0 : s == STATUS_OBJECT_NAME_NOT_FOUND ? 2 : s == STATUS_NO_MEMORY ? 8 : 31);
}

WK_IMPORT(MmQueryStatistics) {
    WK_TRACE(MmQueryStatistics);
    // MM_STATISTICS : +0 longueur (fournie), puis compteurs de pages (4 Kio) ; valeurs plausibles (512 Mo)
    const uint32_t s = ctx.r3.u32, len = Ld32(s);
    if (len < 0x40 || len > 0x100) { Ret(ctx, STATUS_INVALID_PARAMETER); return; }
    for (uint32_t i = 4; i < len; i += 4) St32(s + i, 0);
    St32(s + 0x04, 0x20000);       // pages physiques totales
    St32(s + 0x08, 0x2000);        // pages du noyau
    St32(s + 0x0C, 0x8000);        // pages disponibles pour le titre
    St32(s + 0x10, 0x30000000);    // mémoire virtuelle totale du titre (octets)
    St32(s + 0x14, 0x08000000);    // mémoire virtuelle réservée du titre (octets)
    St32(s + 0x18, 0x10000);       // pages physiques du titre
    Ret(ctx, STATUS_SUCCESS);
}
