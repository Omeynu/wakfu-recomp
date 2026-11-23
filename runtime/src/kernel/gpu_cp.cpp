// Processeur de commandes PM4 : exécute ce qui fait avancer le CPU (écritures mémoire, barrières, registres scratch,
// interruptions, marqueur de swap) ; draws, shaders et constantes sont ignorés (rendu natif, natif/device.cpp).
// Mémoire vue par le GPU : adresses physiques -> invitées en 0xA0000000+ (cf. PhysToGuest).
#include "gpu_cp.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "../trace.h"
#include "../process.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "../natif/device.h"
#endif

namespace wk {

namespace {
constexpr uint32_t kMmio = 0x7FC80000;
uint32_t g_regs[0x10000];

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
// Toute écriture que le CPU peut observer (barrière, compteur, interruption) n'a lieu qu'à la fin réelle du travail
// natif envoyé avant elle (le jeu réécrit sommets et constantes après ses barrières) ; sans rendu, tout de suite.
void Visible(std::function<void()> op) {
#ifdef WAKFU_HAS_SHADERGEN
    natif::Rappel(std::move(op));
#else
    op();
#endif
}
void MemWrite(uint32_t addrWithEndian, uint32_t value) {
    Visible([addrWithEndian, value] { St32(PhysToGuest(addrWithEndian & ~3u), __builtin_bswap32(GpuSwap(value, addrWithEndian & 3))); });
}

void WriteRegister(uint32_t reg, uint32_t value) {
    reg &= 0xFFFF;
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
        uint32_t v = Ld32(base + 4 * (ring && pos >= sizeDwords ? pos % sizeDwords : pos));
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
        case kCpSwapOpcode: {     // marqueur de swap du runtime : signature, image
            if (r.Read() != kCpSwapMagic) break;
            const uint32_t frame = r.Read();
            trace::Mark('S', frame);
            if (g_cb.swap) g_cb.swap(frame);
            break;
        }
        case 0x54: {              // INTERRUPT : masque des CPU à interrompre
            const uint32_t mask = r.Read();
            for (uint32_t cpu = 0; cpu < 6; cpu++)   // tout de suite (cf. registres scratch, WriteRegister)
                if (((mask >> cpu) & 1) && g_cb.interrupt) g_cb.interrupt(1, cpu);
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
        // INVALIDATE_STATE, CONTEXT_UPDATE, VIZ_QUERY, EVENT_WRITE_ZPD (0x5B) ; rendu (fait par le natif) : DRAW_INDX(_2),
        // IM_LOAD(_IMMEDIATE), SET_CONSTANT(2), LOAD_ALU_CONSTANT, SET_SHADER_CONSTANTS
        case 0x22: case 0x36: case 0x27: case 0x2B: case 0x2D: case 0x55: case 0x2F: case 0x56:
        case 0x10: case 0x48: case 0x26: case 0x46: case 0x5A: case 0x59: case 0x3B: case 0x5B:
        case 0x5E: case 0x23: case 0x4A: case 0x4B: case 0x25: case 0x2E: case 0x5C: case 0x5D: case 0x52: case 0x53:
            break;
        default: {                // fil du CP seul
            static bool vus[128];
            if (!vus[op]) fprintf(stderr, "[gpu] paquet type 3 inconnu : %02X\n", op);
            vus[op] = true;
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
void CpSetSwapCounter(uint32_t n) { g_swapCounter = n; }

uint32_t CpRegister(uint32_t index) { return g_regs[index & 0xFFFF]; }

uint32_t CpProcessRing(uint32_t ringBase, uint32_t sizeDwords, uint32_t rptr, uint32_t wptr) {
    Reader r{ringBase, sizeDwords, rptr, true};
    // positions non modulo pour la boucle : wptr « déroulé » si l'anneau a bouclé
    uint32_t end = wptr >= rptr ? wptr : wptr + sizeDwords;
    Execute(r, end, 0);
    return end % sizeDwords;
}

}  // namespace wk
