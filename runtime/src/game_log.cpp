// Crochets sur des fonctions du jeu : journal de débogage (WAKFU_GAMELOG=1) et saut des vidéos XUI (tests).
// Journal : sub_82169360 est vide dans la version commerciale ; on affiche la chaîne formatée (%s, %d, %x, %u, %c, %f).
#include <cstdio>
#include <cstring>
#include <set>
#include <cstdlib>
#include "config.h"
#include "guest.h"
#include "kernel/sync.h"
#include "trace.h"

// D3D BlockOnFence (thread du jeu) : trace seulement
PPC_FUNC_IMPL(__imp__sub_821BD620);
PPC_FUNC(sub_821BD620) {
    wk::trace::Mark('B', ctx.r4.u32);
    __imp__sub_821BD620(ctx, base);
    wk::trace::Mark('b');
}

static const bool g_gameLog = getenv("WAKFU_GAMELOG") && *getenv("WAKFU_GAMELOG") == '1';

PPC_FUNC(sub_82169360) {
    if (!g_gameLog) return;
    const std::string f = wk::ReadAnsi(ctx.r3.u32, 256);
    // Arguments variables (ABI PPC64 : r4..r10, flottants passés aussi dans les registres entiers).
    const uint64_t args[7] = {ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
    std::string s;
    size_t k = 0;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) { s += f[i]; continue; }
        size_t j = i + 1;
        while (j < f.size() && strchr("-+ #0123456789.lh", f[j])) j++;
        if (j >= f.size()) break;
        const char c = f[j];
        const std::string spec = f.substr(i, j - i) + c;
        char buf[288];
        if (c == '%') { s += '%'; i = j; continue; }
        const uint64_t a = k < 7 ? args[k++] : 0;
        if (c == 's') snprintf(buf, sizeof buf, "%s", a ? wk::ReadAnsi(uint32_t(a), 256).c_str() : "(nul)");
        else if (c == 'f' || c == 'g' || c == 'e') { double d; memcpy(&d, &a, 8); snprintf(buf, sizeof buf, "%g", d); }
        else if (c == 'x' || c == 'X') snprintf(buf, sizeof buf, "%x", uint32_t(a));
        else if (c == 'u') snprintf(buf, sizeof buf, "%u", uint32_t(a));
        else if (c == 'c') snprintf(buf, sizeof buf, "%c", char(a));
        else snprintf(buf, sizeof buf, "%d", int32_t(a));
        s += buf;
        i = j;
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    fprintf(stderr, "[jeu] %s\n", s.c_str());
}

// WAKFU_SKIP_VIDEOS=1 (tests) : les vidéos intégrées à XUI (logos, fond de l'écran-titre) se terminent tout de suite.
// sub_822EA7C8 décode l'image suivante pour l'élément vidéo XUI ; 0x16660026 = fin de la vidéo. (Les gels d'avant
// venaient du minuteur XUI d'intervalle non nul jamais déclenché : TickCount de KeTimeStampBundle figé, cf. variables.cpp.)
PPC_FUNC_IMPL(__imp__sub_822EA7C8);
PPC_FUNC(sub_822EA7C8) {
    static const bool skip = getenv("WAKFU_SKIP_VIDEOS") && *getenv("WAKFU_SKIP_VIDEOS") == '1';
    if (skip) { ctx.r3.u64 = 0x16660026; return; }
    // Option passer_logos : les deux premiers éléments vidéo XUI du lancement sont les logos (le troisième est le fond
    // de l'écran-titre) ; ils se terminent tout de suite.
    static std::set<uint32_t> logos;
    if (wk::config::g.skipLogos && (logos.count(ctx.r3.u32) || (logos.size() < 2 && logos.insert(ctx.r3.u32).second))) {
        static std::set<uint32_t> told;
        if (told.insert(ctx.r3.u32).second) fprintf(stderr, "[config] logo sauté (élément vidéo %08X)\n", ctx.r3.u32);
        ctx.r3.u64 = 0x16660026;
        return;
    }
    __imp__sub_822EA7C8(ctx, base);
}

// Lecteur vidéo XUI (objet de vtable 0x82054688, 4 threads créés suspendus par sub_822F9CD0) : 8 événements
// dans l'objet, ev0..ev3 (+84/+100/+116/+132, synchro) pour les données, ev4..ev7 (notification) = thread au repos.
// Les attentes sur ev0..ev3 (vtable +164..+176) sont bornées à 10 ms : ev3 a deux consommateurs (thread principal
// dans sub_822FA4B8, thread 4 sub_822FB608) et un réveil pris par l'un manque à l'autre, qui attendait sans fin
// pendant que le producteur, file pleine, attendait lui aussi. Tous les appelants revérifient leur condition après
// l'attente et ignorent le code de retour : un réveil sans signal ne fait qu'un tour de boucle de plus.
static void BoundedPlayerWait(PPCContext& ctx, uint32_t eventOffset) {
    static const uint32_t timeout = [] {
        const uint32_t cell = wk::Alloc(wk::Heap::Kernel, 8);
        wk::St64(cell, uint64_t(-100000));   // relatif, 100 ns : 10 ms
        return cell;
    }();
    const uint32_t event = ctx.r3.u32 + eventOffset;
    wk::WaitObjects(&event, 1, false, timeout);
    ctx.r3.u64 = 0;
}
PPC_FUNC(sub_822F9208) { BoundedPlayerWait(ctx, 84); }
PPC_FUNC(sub_822F9220) { BoundedPlayerWait(ctx, 100); }
PPC_FUNC(sub_822F9238) { BoundedPlayerWait(ctx, 116); }
PPC_FUNC(sub_822F9250) { BoundedPlayerWait(ctx, 132); }
