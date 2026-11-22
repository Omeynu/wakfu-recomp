// Processeur de commandes PM4 (jalons 2-3) : exécute ce qui fait avancer le CPU (écritures mémoire,
// barrières, registres scratch, interruptions), suit l'état Xenos (registres, shaders chargés, draws,
// resolves) et en tire des statistiques par image. Aucun rendu : c'est le jalon 4.
// Mémoire vue par le GPU : adresses physiques -> invitées en 0xA0000000+ (cf. PhysToGuest).
#include "gpu_cp.h"
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include "../trace.h"
#include "../process.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "../gpu/renderer.h"
#endif

namespace wk {

namespace {
constexpr uint32_t kMmio = 0x7FC80000;
uint32_t g_regs[0x10000];
}  // namespace
// Génération des registres de constantes (0x4000-0x490F) : le rendu ne recopie les constantes que si elle a changé.
uint64_t g_cpConstGen = 1;
namespace {
std::map<uint32_t, uint64_t> g_opStats;
uint64_t g_draws = 0;
std::mutex g_statsMutex;

// Registres Xenos utilisés (indices de registre)
constexpr uint32_t REG_RB_SURFACE_INFO = 0x2000, REG_RB_COLOR_INFO = 0x2001, REG_RB_MODECONTROL = 0x2208,
                   REG_RB_COPY_DEST_BASE = 0x2319, REG_RB_COPY_DEST_PITCH = 0x231A, REG_RB_COPY_DEST_INFO = 0x231B;

// Microcode -> hachage FNV-1a 64 bits ; catalogue des shaders du jeu par hachage de microcode
uint64_t Fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
std::map<uint64_t, std::string> g_catalog;

struct Frame {
    uint32_t draws = 0, resolves = 0, indices = 0, shaderLoads = 0;
    std::map<uint32_t, uint32_t> prims;          // type de primitive -> nombre
    std::set<uint64_t> shaders, unknownShaders, xexShaders;
    std::set<uint32_t> resolveDests;
} g_frame;
std::set<uint64_t> g_allShaders, g_allUnknown, g_allXex;
std::map<uint32_t, uint64_t> g_unknownOps;
uint64_t g_totalResolves = 0;
CpResolve g_lastResolve;

CpShaderHook g_shaderHook;
uint32_t g_lastShaderType = 0;
uint64_t g_currentShader[2] = {0, 0};   // dernier microcode chargé : [0] sommets, [1] pixels

void LoadShader(uint32_t guestAddr, uint32_t sizeDwords) {
    const uint64_t h = Fnv(Host(guestAddr), sizeDwords * 4);
    g_currentShader[g_lastShaderType & 1] = h;
    if (g_shaderHook) g_shaderHook(g_lastShaderType, h, Host(guestAddr), sizeDwords);
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_frame.shaderLoads++;
    g_frame.shaders.insert(h);
    g_allShaders.insert(h);
    auto it = g_catalog.find(h);
    if (it == g_catalog.end()) { g_frame.unknownShaders.insert(h); g_allUnknown.insert(h); }
    else if (it->second.rfind("xex:", 0) == 0) { g_frame.xexShaders.insert(h); g_allXex.insert(h); }
}

void Draw(uint32_t initiator, uint32_t indexBase = 0, uint32_t indexInfo = 0, bool dma = false) {
    const uint32_t prim = initiator & 0x3F, count = initiator >> 16;
    const uint32_t mode = g_regs[REG_RB_MODECONTROL] & 7;
    bool resolve = false;
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        if (mode == 6) {                          // edram_mode = copie : c'est un resolve
            g_frame.resolves++;
            g_totalResolves++;
            g_lastResolve = {g_regs[REG_RB_COPY_DEST_BASE], g_regs[REG_RB_COPY_DEST_PITCH], g_regs[REG_RB_COPY_DEST_INFO],
                             g_regs[REG_RB_SURFACE_INFO], g_regs[REG_RB_COLOR_INFO]};
            g_frame.resolveDests.insert(g_regs[REG_RB_COPY_DEST_BASE]);
            resolve = true;
        } else {
            g_frame.draws++;
            g_frame.indices += count;
            g_frame.prims[prim]++;
        }
    }
#ifdef WAKFU_HAS_SHADERGEN
    if (gpu::RendererActive()) {
        if (resolve) gpu::RendererResolve(g_regs);
        else if (mode == 4 || mode == 0) gpu::RendererDraw(g_regs, g_currentShader[0], g_currentShader[1], {initiator, indexBase, indexInfo, dma});
    }
#else
    (void)indexBase; (void)indexInfo; (void)dma; (void)resolve;
#endif
}

uint32_t PhysToGuest(uint32_t p) { return p < 0x20000000 ? 0xA0000000u | p : p; }

// Mise en ordre des octets selon le mode demandé (2 bits bas de l'adresse ou champ dédié)
uint32_t GpuSwap(uint32_t v, uint32_t endian) {
    switch (endian & 3) {
        case 1: return ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF);   // 8 dans 16
        case 2: return __builtin_bswap32(v);                                   // 8 dans 32
        case 3: return (v << 16) | (v >> 16);                                   // 16 dans 32
        default: return v;
    }
}
// Le GPU voit la mémoire en petit-boutiste ; St32/Ld32 manipulent la vue (gros-boutiste) du CPU.
// Mode 2 (8 dans 32) : la valeur arrive telle que le CPU la lit.
// Registre de départ d'un chargement de constantes (type dans les bits 16-23) : ALU, fetch, booléens, boucles,
// registres (cf. Xenia). 0 si type inconnu.
uint32_t ConstantRegister(uint32_t info) {
    static const uint32_t kBase[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
    const uint32_t type = (info >> 16) & 0xFF;
    return type < 5 ? kBase[type] + (info & 0x7FF) : 0;
}
// Le rendu est asynchrone : toute écriture que le CPU peut observer (barrière, compteur, interruption) n'a lieu
// qu'à la fin réelle du travail GPU qui la précède dans le flux, comme sur la console où le GPU exécute dans
// l'ordre. Sans travail GPU en cours, elle est immédiate.
void Visible(std::function<void()> op) {
#ifdef WAKFU_HAS_SHADERGEN
    if (gpu::RendererActive() && gpu::RendererDefer(op)) return;
#endif
    op();
}
// Attente du CP non satisfaite : termine d'abord le travail GPU et les écritures différées qu'elle peut attendre.
void SyncGpuWrites() {
#ifdef WAKFU_HAS_SHADERGEN
    if (gpu::RendererActive()) gpu::RendererSyncCpu();
#endif
}
void MemWrite(uint32_t addrWithEndian, uint32_t value) {
    Visible([addrWithEndian, value] { St32(PhysToGuest(addrWithEndian & ~3u), __builtin_bswap32(GpuSwap(value, addrWithEndian & 3))); });
}

void WriteRegister(uint32_t reg, uint32_t value) {
    reg &= 0xFFFF;
    if (reg >= 0x4000 && reg < 0x4910 && g_regs[reg] != value) g_cpConstGen++;   // constantes ALU, de lecture, bool, boucle
    g_regs[reg] = value;
    St32(kMmio + reg * 4, value);                       // miroir lisible par le CPU
    // Registres scratch 0x578-0x57F recopiés en mémoire si SCRATCH_UMSK (0x1DC) l'autorise
    if (reg >= 0x578 && reg <= 0x57F) {
        const uint32_t n = reg - 0x578;
        if ((g_regs[0x1DC] >> n) & 1) {
            const uint32_t addr = PhysToGuest(g_regs[0x1DD]) + n * 4;
            // Tout de suite, comme les interruptions : rappels D3D (InsertCallback : scratch0 = 4, interruption, attente de
            // scratch0 = 0 posé par le gestionnaire). Différés à la fin réelle du GPU, ils coûtaient une vidange par image
            // (32 -> 44 img/s). Les barrières (MEM_WRITE, EVENT_WRITE) restent différées : le jeu y attend la fin du GPU.
            St32(addr, value);
        }
    }
}

// Condition des paquets WAIT_REG_MEM / COND_WRITE (fonction : bits 0-2)
bool Compare(uint32_t func, uint32_t v, uint32_t ref) {
    switch (func & 7) {
        case 0: return false; case 1: return v < ref; case 2: return v <= ref; case 3: return v == ref;
        case 4: return v != ref; case 5: return v >= ref; case 6: return v > ref; default: return true;
    }
}
uint32_t PollValue(uint32_t info, uint32_t addr) {
    // bit 4 : lecture mémoire (adresse + mode d'octets), sinon registre
    return (info & 0x10) ? GpuSwap(__builtin_bswap32(Ld32(PhysToGuest(addr & ~3u))), addr & 3) : g_regs[addr & 0xFFFF];
}

struct Reader {
    uint32_t base, sizeDwords, pos;   // pos en mots, modulo la taille (anneau) ou linéaire (IB)
    bool ring;
    uint32_t Read() {
        uint32_t v = Ld32(base + 4 * (ring ? pos % sizeDwords : pos));
        pos++;
        return v;
    }
};

CpCallbacks g_cb;
// Prédication par zones (« bins ») : un paquet type 3 marqué (bit 0) ne s'exécute que si
// bin_select & bin_mask != 0 (cf. Xenia). D3D rejoue ainsi un même segment pour chaque zone d'écran.
uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;
uint32_t g_swapCounter = 0;

void Execute(Reader& r, uint32_t endPos, int depth);

void ExecuteType3(Reader& r, uint32_t op, uint32_t count, int depth) {
    g_opStats[op]++;
    const uint32_t start = r.pos;
    switch (op) {
        case 0x3F: case 0x37: {   // INDIRECT_BUFFER(_PFD) : adresse, taille en mots
            const uint32_t addr = r.Read(), size = r.Read() & 0xFFFFF;
            if (depth < 4) {
                Reader ib{PhysToGuest(addr), size, 0, false};
                Execute(ib, size, depth + 1);
            }
            break;
        }
        case 0x58: {              // EVENT_WRITE_SHD : initiateur, adresse, valeur
            const uint32_t init = r.Read(), addr = r.Read(), value = r.Read();
            MemWrite(addr, (init >> 31) ? g_swapCounter : value);
            break;
        }
        case 0x3D: {              // MEM_WRITE : adresse puis valeurs
            uint32_t addr = r.Read();
            for (uint32_t i = 1; i < count; i++, addr += 4) MemWrite(addr, r.Read());
            break;
        }
        case 0x3E: {              // REG_TO_MEM : registre, adresse
            const uint32_t reg = r.Read(), addr = r.Read();
            MemWrite(addr, g_regs[reg & 0xFFFF]);
            break;
        }
        case 0x21: {              // REG_RMW : registre, masque ET, valeur OU
            const uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
            WriteRegister(info & 0x1FFF, (g_regs[info & 0x1FFF] & andMask) | orMask);
            break;
        }
        case 0x3C: case 0x45: {   // WAIT_REG_MEM / COND_WRITE : info, adresse, référence, masque, ...
            const uint32_t info = r.Read(), addr = r.Read(), ref = r.Read(), mask = r.Read();
            bool ok = Compare(info, PollValue(info, addr) & mask, ref);
            const bool traced = !ok;
            if (!ok) trace::Mark('W', addr);
            if (!ok) { SyncGpuWrites(); ok = Compare(info, PollValue(info, addr) & mask, ref); if (ok) trace::Mark('F', addr); }
            // attend le CPU ou une interruption (10 s max) : d'abord en cédant la main (réveil rapide), puis par pas de 100 µs
            for (int i = 0; !ok && i < 100000; i++) {
                if (i < 200) std::this_thread::yield();
                else std::this_thread::sleep_for(std::chrono::microseconds(100));
                ok = Compare(info, PollValue(info, addr) & mask, ref);
            }
            if (traced) trace::Mark('w');
            if (!ok) fprintf(stderr, "[gpu] attente %s non satisfaite : info %08X adresse %08X réf %08X masque %08X valeur %08X\n",
                             op == 0x3C ? "WAIT_REG_MEM" : "COND_WRITE", info, addr, ref, mask, PollValue(info, addr));
            if (op == 0x45) {
                const uint32_t writeAddr = r.Read(), value = r.Read();
                if (ok) { if (info & 0x100) MemWrite(writeAddr, value); else WriteRegister(writeAddr, value); }
            }
            break;
        }
        case kCpSwapOpcode: {     // marqueur de swap du runtime : signature, image, constante de texture (6 mots)
            if (r.Read() != kCpSwapMagic) break;
            const uint32_t frame = r.Read();
            uint32_t fetch[6];
            for (auto& v : fetch) v = r.Read();
            trace::Mark('S', frame);
            if (g_cb.swap) g_cb.swap(frame, fetch);
            break;
        }
        case 0x54: {              // INTERRUPT : masque des CPU à interrompre
            const uint32_t mask = r.Read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)   // tout de suite (cf. registres scratch, WriteRegister)
                if (((mask >> cpu) & 1) && g_cb.interrupt) g_cb.interrupt(1, cpu);
            break;
        }
        case 0x22: {              // DRAW_INDX : info de requête, VGT_DRAW_INITIATOR, [base, taille des indices]
            r.Read();
            const uint32_t init = r.Read();
            const bool dma = ((init >> 6) & 3) == 0 && count >= 4;
            const uint32_t ib = dma ? r.Read() : 0, isz = dma ? r.Read() : 0;
            g_draws++;
            Draw(init, ib, isz, dma);
            break;
        }
        case 0x36: {              // DRAW_INDX_2 : VGT_DRAW_INITIATOR puis indices en ligne
            const uint32_t init = r.Read();
            g_draws++;
            Draw(init);
            break;
        }
        case 0x27: {              // IM_LOAD : adresse|type, début<<16|taille (mots)
            const uint32_t addrType = r.Read(), startSize = r.Read();
            g_lastShaderType = addrType & 3;      // 0 sommets, 1 pixels
            LoadShader(PhysToGuest(addrType & ~3u), startSize & 0xFFFF);
            break;
        }
        case 0x2B: {              // IM_LOAD_IMMEDIATE : type, début<<16|taille, microcode en ligne
            g_lastShaderType = r.Read() & 3;
            const uint32_t startSize = r.Read();
            const uint32_t n = startSize & 0xFFFF;
            if (r.ring) {         // microcode à cheval possible : copie linéaire
                std::vector<uint8_t> tmp(n * 4);
                for (uint32_t i = 0; i < n; i++) { uint32_t v = __builtin_bswap32(r.Read()); memcpy(&tmp[4 * i], &v, 4); }
                const uint64_t h = Fnv(tmp.data(), tmp.size());
                g_currentShader[g_lastShaderType & 1] = h;
                if (g_shaderHook) g_shaderHook(g_lastShaderType, h, tmp.data(), n);
                std::lock_guard<std::mutex> lock(g_statsMutex);
                g_frame.shaderLoads++;
                g_frame.shaders.insert(h);
                g_allShaders.insert(h);
                auto it = g_catalog.find(h);
                if (it == g_catalog.end()) { g_frame.unknownShaders.insert(h); g_allUnknown.insert(h); }
                else if (it->second.rfind("xex:", 0) == 0) { g_frame.xexShaders.insert(h); g_allXex.insert(h); }
            } else {
                LoadShader(r.base + 4 * r.pos, n);
            }
            break;
        }
        case 0x2D: case 0x55: {   // SET_CONSTANT(2) : type|index puis valeurs
            const uint32_t info = r.Read();
            uint32_t idx = ConstantRegister(info);
            if (idx) for (uint32_t i = 1; i < count; i++) WriteRegister(idx++, r.Read());
            break;
        }
        case 0x2F: {              // LOAD_ALU_CONSTANT : adresse physique, type|index, taille (mots) lue en mémoire
            const uint32_t addr = r.Read() & 0x1FFFFFFFu, info = r.Read(), size = r.Read() & 0xFFF;
            uint32_t idx = ConstantRegister(info);
            if (idx) for (uint32_t i = 0; i < size; i++) WriteRegister(idx++, Ld32(PhysToGuest(addr + 4 * i)));
            break;
        }
        case 0x56: {              // SET_SHADER_CONSTANTS : registre de départ (absolu) puis valeurs
            uint32_t idx = r.Read() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++) WriteRegister(idx++, r.Read());
            break;
        }
        case 0x50: case 0x51: {   // SET_BIN_MASK / SET_BIN_SELECT (64 bits : haut, bas)
            const uint64_t hi = r.Read(), lo = r.Read();
            (op == 0x50 ? g_binMask : g_binSelect) = (hi << 32) | lo;
            break;
        }
        case 0x60: case 0x61: case 0x62: case 0x63: {   // SET_BIN_MASK_LO/HI, SET_BIN_SELECT_LO/HI
            const uint64_t v = r.Read();
            uint64_t& d = op < 0x62 ? g_binMask : g_binSelect;
            d = (op & 1) ? (d & 0xFFFFFFFFull) | (v << 32) : (d & ~0xFFFFFFFFull) | v;
            break;
        }
        // Paquets reconnus sans effet hors rendu : NOP, ME_INIT, WAIT_FOR_IDLE, EVENT_WRITE(_EXT/_CFL/_ZPD),
        // INVALIDATE_STATE, CONTEXT_UPDATE, VIZ_QUERY, EVENT_WRITE_ZPD (0x5B)
        case 0x10: case 0x48: case 0x26: case 0x46: case 0x5A: case 0x59: case 0x3B: case 0x5B:
        case 0x5E: case 0x23: case 0x4A: case 0x4B: case 0x25: case 0x2E: case 0x5C: case 0x5D: case 0x52: case 0x53:
            break;
        default: {
            std::lock_guard<std::mutex> lock(g_statsMutex);
            g_unknownOps[op]++;
            break;
        }
    }
    r.pos = start + count;        // saute les mots non lus
}

void Execute(Reader& r, uint32_t endPos, int depth) {
    while (r.pos < endPos) {
        const uint32_t h = r.Read();
        switch (h >> 30) {
            case 0: {             // écriture de registres consécutifs (ou du même)
                const uint32_t count = ((h >> 16) & 0x3FFF) + 1, reg = h & 0x7FFF, one = (h >> 15) & 1;
                for (uint32_t i = 0; i < count; i++) WriteRegister(one ? reg : reg + i, r.Read());
                break;
            }
            case 1: {             // deux registres
                const uint32_t r1 = h & 0x7FF, r2 = (h >> 11) & 0x7FF;
                WriteRegister(r1, r.Read());
                WriteRegister(r2, r.Read());
                break;
            }
            case 2: break;        // NOP
            case 3: {
                const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
                if ((h & 1) && !(g_binSelect & g_binMask)) { r.pos += count; break; }   // prédiqué, zone non retenue
                ExecuteType3(r, (h >> 8) & 0x7F, count, depth);
                break;
            }
        }
    }
}
}  // namespace

void CpSetCallbacks(const CpCallbacks& cb) { g_cb = cb; }
void CpSetShaderHook(CpShaderHook hook) { g_shaderHook = std::move(hook); }
void CpSetSwapCounter(uint32_t n) { g_swapCounter = n; }

uint32_t CpRegister(uint32_t index) { return g_regs[index & 0xFFFF]; }

uint32_t CpProcessRing(uint32_t ringBase, uint32_t sizeDwords, uint32_t rptr, uint32_t wptr) {
    Reader r{ringBase, sizeDwords, rptr, true};
    // positions non modulo pour la boucle : wptr « déroulé » si l'anneau a bouclé
    uint32_t end = wptr >= rptr ? wptr : wptr + sizeDwords;
    Execute(r, end, 0);
    return end % sizeDwords;
}

struct Precompile { uint32_t type; uint64_t hash; std::vector<uint8_t> ucode; };
std::vector<Precompile> g_precompile;

void CpPrecompileCatalog(CpShaderHook hook) {
    if (!hook || g_precompile.empty()) return;
    std::thread([hook, list = std::move(g_precompile)] {
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& p : list) hook(p.type, p.hash, p.ucode.data(), uint32_t(p.ucode.size() / 4));
        fprintf(stderr, "[shaders] %zu shaders du jeu précompilés en %.1f s\n", list.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }).detach();
}

void CpLoadShaderCatalog(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) { fprintf(stderr, "[gpu] catalogue de shaders introuvable : %s\n", dir.c_str()); return; }
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 5 || (n.compare(n.size() - 4, 4, ".xpu") && n.compare(n.size() - 4, 4, ".xvu"))) continue;
        std::ifstream f(dir + "/" + n, std::ios::binary);
        std::vector<uint8_t> b{std::istreambuf_iterator<char>(f), {}};
        auto be = [&](size_t o) { return o + 4 <= b.size() ? uint32_t(b[o]) << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3] : 0u; };
        // ShaderContainer : +4 virtualSize, +0x18 shaderOffset ; Shader : +0 physicalOffset, +4 taille (octets)
        const uint32_t virt = be(4), sh = be(0x18), phys = be(sh), size = be(sh + 4);
        if (virt + phys + size > b.size() || !size) continue;
        const uint64_t h = Fnv(&b[virt + phys], size);
        g_catalog[h] = n;
        if (size % 4 == 0)
            g_precompile.push_back({n.compare(n.size() - 4, 4, ".xpu") == 0 ? 1u : 0u, h,
                                    std::vector<uint8_t>(b.begin() + virt + phys, b.begin() + virt + phys + size)});
    }
    closedir(d);
    fprintf(stderr, "[gpu] catalogue : %zu shaders du jeu\n", g_catalog.size());
}

void CpLoadShaderCatalogFromImage(uint32_t base, uint32_t size) {
    // Conteneurs intégrés au XEX (XUI, vidéo, D3DX) : mêmes critères que XenosRecomp.
    size_t n = 0;
    for (uint32_t o = 0; o + 0x24 <= size; o += 4) {
        const uint32_t a = base + o, flags = Ld32(a), virt = Ld32(a + 4), phys = Ld32(a + 8);
        if ((flags & 0xFFFFFF00u) != 0x102A1100u || Ld32(a + 0x1C) || Ld32(a + 0x20) || virt + phys > size - o) continue;
        const uint32_t sh = a + Ld32(a + 0x18), poff = Ld32(sh), len = Ld32(sh + 4);
        if (virt + poff + len <= virt + phys && len) {
            g_catalog.emplace(Fnv(Host(a + virt + poff), len), "xex:" + std::to_string(o));
            n++;
        }
        o += virt + phys - 4;
    }
    fprintf(stderr, "[gpu] catalogue : %zu shaders intégrés au XEX\n", n);
}

CpFrameStats CpEndFrame() {
    std::lock_guard<std::mutex> lock(g_statsMutex);
    CpFrameStats st;
    st.draws = g_frame.draws; st.resolves = g_frame.resolves; st.indices = g_frame.indices;
    st.shaderLoads = g_frame.shaderLoads; st.shaders = uint32_t(g_frame.shaders.size());
    st.unknownShaders = uint32_t(g_frame.unknownShaders.size());
    for (auto& [p, n] : g_frame.prims) st.prims += (st.prims.empty() ? "" : " ") + std::to_string(p) + ":" + std::to_string(n);
    st.totalShaders = uint32_t(g_allShaders.size()); st.totalUnknownShaders = uint32_t(g_allUnknown.size());
    st.totalXexShaders = uint32_t(g_allXex.size());
    st.totalResolves = g_totalResolves; st.lastResolve = g_lastResolve;
    for (auto& [op, n] : g_unknownOps) st.unknownOps += (st.unknownOps.empty() ? "" : " ") + std::to_string(op) + ":" + std::to_string(n);
    g_frame = Frame{};
    return st;
}

void CpDumpStats() {
    fprintf(stderr, "[gpu] draws : %llu ; paquets type 3 :", (unsigned long long)g_draws);
    for (auto& [op, n] : g_opStats) fprintf(stderr, " %02X:%llu", op, (unsigned long long)n);
    fprintf(stderr, "\n");
}

}  // namespace wk
