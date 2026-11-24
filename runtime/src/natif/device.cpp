// Cœur du rendu natif (voir device.h). Fil du jeu : lecture de l'état du device invité, envoi de commandes.
// Fil « natif » : images Vulkan des cibles, passes de rendu dynamiques, effacements, resolves, pipelines,
// présentation, capture. shortcut: toutes les images natives restent en VK_IMAGE_LAYOUT_GENERAL avec des barrières
// globales entre opérations (simple, un peu plus lent) ; à affiner si le GPU devient la limite (N8).
#include "natif/device.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "config.h"
#include "kernel/window.h"
#include "gpu/vk.h"
#include "guest.h"
#include "natif/etats.h"
#include "natif/interfaces.h"
#include "natif/tampons.h"
#include "platform.h"
#include "quit.h"
#include "png.h"
#include "trace.h"

uint64_t WkBouclesNs();          // kernel/core.cpp, fil courant
uint64_t WkLecturesHorloge();
namespace wk { uint32_t SwapsEmis(); extern std::string g_dataDir; }   // kernel/gpu.cpp : nombre de VdSwap émis par le jeu

namespace wk::natif {
namespace {
using namespace wk::vk;

// Contexte Vulkan (InitialiserVulkan, fil du CP) ; nul : pas de rendu (sans fenêtre ni WAKFU_RENDER=1)
std::atomic<vk::Context*> g_vk{nullptr};

// Captures : WAKFU_CAPTURE="60,300,900" (numéros de VdSwap) ou "toutes:N" ; dossier WAKFU_CAPTURE_DIR (captures/).
// Vrai si l'image n est demandée, *chemin = <dossier>/image_<n>.png (dossier créé).
bool CaptureVoulue(uint32_t n, std::string* chemin) {
    static const std::string spec = getenv("WAKFU_CAPTURE") ? getenv("WAKFU_CAPTURE") : "";
    if (spec.empty()) return false;
    static const std::set<uint32_t> images = [] {
        std::set<uint32_t> f;
        for (size_t i = 0; i < spec.size();) {
            size_t j = spec.find(',', i);
            if (j == std::string::npos) j = spec.size();
            f.insert(uint32_t(atoi(spec.substr(i, j - i).c_str())));
            i = j + 1;
        }
        return f;
    }();
    if (spec.rfind("toutes:", 0) == 0) { const int k = atoi(spec.c_str() + 7); if (k <= 0 || n % k) return false; }
    else if (!images.count(n)) return false;
    static const std::string dossier = getenv("WAKFU_CAPTURE_DIR") ? getenv("WAKFU_CAPTURE_DIR") : "captures";
    std::error_code ec;
    std::filesystem::create_directories(dossier, ec);
    *chemin = dossier + "/image_" + std::to_string(n) + ".png";
    return true;
}

enum class Op { Surface, Texture, Cible, Profondeur, Effacer, Resoudre, Dessiner, Echange, Rappel };
struct Commande {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;   // selon op (voir les fonctions publiques)
    uint32_t w = 0, h = 0, format = 0;            // description d'une texture (Texture, Resoudre)
    int32_t rect[4]{};
    bool avecRect = false;
    float couleur[4]{};
    float z = 0;
    std::unique_ptr<Instantane> draw;
    std::function<void()> rappel;   // Rappel
};

// --- File (fil du jeu -> fil natif) ---
std::mutex g_mutex;
std::condition_variable g_cv, g_cvEchange;
std::deque<Commande> g_file;
int g_echangesEnAttente = 0;
uint64_t g_echangesEnvoyes = 0, g_echangesFaits = 0;   // Vider
// Présentation calée sur le swap du CP (vblank synthétique), comme le swap matériel, et non sur l'appel Swap du jeu
// (durée de travail variable). Le fil natif n'y bloque pas : les rappels (barrières du jeu) passent pendant l'attente.
uint32_t g_swapCp = 0;   // dernier swap atteint par le CP (g_mutex)
struct { bool actif = false; VkImage image = VK_NULL_HANDLE; uint32_t w = 0, h = 0, swap = 0; } g_aPresenter;   // fil natif
bool PretAPresenter() { return g_aPresenter.actif && int32_t(g_swapCp - g_aPresenter.swap) >= 0; }   // sous g_mutex
std::once_flag g_lance;
void Boucle();
// Journal du fil du jeu (retours des joueurs : chute de cadence quand les draws augmentent). Fil du jeu seul.
using Horloge = std::chrono::steady_clock;

double Ms(Horloge::time_point a, Horloge::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
struct {
    Horloge::time_point debut{};
    double cpu0 = 0, dessinerMs = 0, attenteMs = 0, maxMs = 0;
    uint64_t boucles0 = 0, lectures0 = 0;
    uint32_t draws = 0, images = 0, lentes = 0;
} g_jeu;

void Envoyer(Commande&& c) {
    std::call_once(g_lance, [] { std::thread(Boucle).detach(); });
    const bool rendu = c.op == Op::Effacer || c.op == Op::Resoudre || c.op == Op::Dessiner || c.op == Op::Echange;
    if (rendu && !g_vk.load()) return;   // sans contexte Vulkan : pas de rendu
    std::unique_lock<std::mutex> l(g_mutex);
    if (!g_vk.load() && (rendu || c.op == Op::Rappel)) {   // fil natif arrêté entre-temps (Boucle) : rien en vol
        if (c.op == Op::Rappel) c.rappel();
        return;
    }
    if (c.op == Op::Echange) {
        g_echangesEnAttente++;
        g_echangesEnvoyes++;
        g_file.push_back(std::move(c));
        g_cv.notify_one();
        // au plus 2 images d'avance (mémoire de la file bornée)
        const auto t0 = Horloge::now();
        g_cvEchange.wait(l, [] { return g_echangesEnAttente <= 2; });
        g_jeu.attenteMs += Ms(t0, Horloge::now());
        return;
    }
    // le fil natif n'attend que sur une file vide : un réveil par paquet, pas par commande
    const bool vide = g_file.empty();
    g_file.push_back(std::move(c));
    if (vide) g_cv.notify_one();
}

float F32(uint32_t a) { return std::bit_cast<float>(Ld32(a)); }

// --- Constantes (fil du jeu) ---
float g_constantes[2048];
bool g_constantesChangees[2] = {true, true};   // VS, PS
std::shared_ptr<const std::array<float, 1024>> g_instantaneConstantes[2];
uint64_t g_serieConstantes[2] = {};

// Description d'une texture d'après son en-tête (fetch constant en +28 : format mot 1 bits 0-5, taille 2D mot 2)
// Clé des textures natives : adresse physique des données (mot 1 de la constante de lecture, bits 12-28), commune aux
// objets qui partagent la mémoire ; l'en-tête porte l'adresse virtuelle (A9B98000), le device la physique (09B98000).
uint32_t Base(uint32_t mot1) { return mot1 & 0x1FFFF000u; }
uint32_t BaseTexture(uint32_t tex) { return Base(Ld32(tex + 32)); }
std::unordered_set<uint32_t> g_basesResolues;   // fil du jeu : destinations de resolve (DestinationResolve)
void DescriptionTexture(uint32_t tex, Commande& c) {
    const uint32_t m1 = Ld32(tex + 32), m2 = Ld32(tex + 36);
    c.format = m1 & 0x3F;
    c.w = (m2 & 0x1FFF) + 1;
    c.h = ((m2 >> 13) & 0x1FFF) + 1;
}

// ====================== Fil natif ======================
struct ImageNative {
    vk::Image img;
    VkSampleCountFlagBits ech = VK_SAMPLE_COUNT_1_BIT;
    uint32_t formatInvite = 0;
    VkImageView lecture = VK_NULL_HANDLE;   // vue échantillonnable (profondeur seule pour D/S, sinon img.view)
    bool resolue = false;                   // écrite par un resolve natif : servie aux draws natifs
    uint32_t echelle = 1;                   // pixels natifs par pixel invité (echelle_rendu)
};
// Causes des resolves non faits (ligne [natif N])
enum Cause { kSansSource, kMip, kCreation, kVide, kFormats, kProfMsaa, kCauses };
const char* const kNomsCauses[kCauses] = {"source absente", "mip>0", "création", "rect vide", "formats", "profondeur MSAA"};
struct Stats {
    uint32_t draws = 0, sansShader = 0, sansSommets = 0, primitive = 0, sansCible = 0, dessines = 0, effacements = 0,
             resolves = 0, resolvesNonFaits = 0, surfaces = 0, formatsInconnus = 0, texturesNatives = 0,
             fusions = 0, echanges = 0;   // resolves de bandes fusionnés, resolves+effacement faits par échange d'images
    uint32_t causes[kCauses]{};
    uint32_t shader[4]{}, sommets[3]{};   // causes (ShadersNatifs::echec, SommetsNatifs::echec)
    uint32_t pipeline = 0, liaison = 0, texturesSecours = 0, sansPasse = 0;
};

struct Natif {
    vk::Context* vk = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmds[2]{}, envois[2]{};   // envois : copies des tampons, soumises avant cmds
    VkFence fences[2]{};
    int slot = 0;
    bool enregistre = false;
    VkFormat d24s8 = VK_FORMAT_D24_UNORM_S8_UINT;
    std::unordered_map<uint32_t, ImageNative> surfaces, textures;
    std::vector<std::pair<uint32_t, ImageNative>> aDetruire;   // (image où la détruire, image)
    uint32_t rt[4]{}, ds = 0;
    bool passe = false;
    uint32_t passeRt[4]{}, passeDs = 0;
    bool passeSansDs = false;   // profondeur d'une autre échelle que la couleur (cible d'ombre agrandie) : non attachée
    VkExtent2D zone{};
    uint32_t passeEchelle = 1;   // échelle des cibles de la passe ouverte (viewport, scissor, effacements)
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    uint32_t image = 0;
    uint32_t echelle = 1;   // définition interne (config echelle_rendu) : cibles natives ×echelle, tailles invitées inchangées
    uint32_t ombres = 1;    // option ombres : cible de l'ombre (512x512 sans MSAA) ×ombres en plus de ×echelle
    uint32_t versionTextures = 0;
    // état dynamique (même liste pour tous les pipelines) et pipeline liés dans n.cmds : non réémis s'ils ne changent pas
    struct Dynamique {
        VkViewport v;
        VkRect2D r;
        float melange[4];
        uint32_t stencil[6];
    } dyn{};
    VkPipeline pipelineLie = VK_NULL_HANDLE;
    bool dynLie = false;   // change quand VueResolue peut rendre autre chose (VersionTextures)
    Stats stats;
    // Rappels (natif seul) : (numéro du lot à attendre, écriture) ; lots numérotés à la soumission
    std::deque<std::pair<uint64_t, std::function<void()>>> rappels;
    uint64_t soumissions = 0, fini = 0, idSlot[2]{};
    // Mesures : horodatages début/fin de chaque lot (2 par emplacement), cumuls sur 60 images
    VkQueryPool temps = VK_NULL_HANDLE;
    bool mesure[2]{};
    double gpuMs = 0;
    uint32_t lots = 0;
    double cpu0 = 0;   // ms (platform::ThreadCpuMs)
    double attenteGpuMs = 0, presentMs = 0;   // durées réelles (fences GPU ; présentation : acquire, envoi, fence)
    // Profil GPU : horodatage après chaque passe/resolve/effacement (déjà séparés par des barrières complètes : coût
    // négligeable), cumulé par catégorie pour la ligne [natif N] ; WAKFU_PROFIL_GPU=1 : par cible, 2 : par draw
    VkQueryPool prof = VK_NULL_HANDLE;
    std::vector<std::string> profNoms[2];
    std::unordered_map<std::string, std::pair<double, uint32_t>> profCumul;   // nom -> (ms, nombre) sur 240 images
    // Répartition CPU (Basculer) : images n % 60 >= 50 seulement
    bool chrono = false;
    Poste poste = kAutres;
    std::chrono::steady_clock::time_point t;
    double postes[kPostes]{};   // ms cumulées sur les images mesurées
};
Natif n;
constexpr uint32_t kProfMax = 4096;   // horodatages par lot
int g_profDetail = 0;   // WAKFU_PROFIL_GPU : 1 noms détaillés, 2 en plus un horodatage par draw
uint32_t g_msaa = 4;    // [video] multi_echantillonnage : échantillons retenus pour ce GPU, 0 = désactivé (Initialiser)

// Image mesurée : sans WAKFU_PROFIL_GPU, 10 images sur 60 seulement (horodatages ≈ 3 % du GPU). Un lot ne couvre
// qu'une image (soumis à l'échange).
bool Mesure() { return n.prof && (g_profDetail || n.image % 60 >= 50); }
const char* const kNomsPostes[kPostes] = {"autres", "file", "shaders", "sommets", "pipelines", "constantes", "textures",
                                           "descripteurs", "commandes", "passes", "resolves", "échange", "attente"};
// Écart entre deux horodatages en ms (bits valides de la file seulement)
double Ecart(uint64_t a, uint64_t b) { return double((b - a) & n.vk->timestampMask) * n.vk->timestampPeriod / 1e6; }

void Marque(std::string nom) {
    if (!Mesure() || n.profNoms[n.slot].size() >= kProfMax) return;
    vkCmdWriteTimestamp(n.cmds[n.slot], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, n.prof, n.slot * kProfMax + uint32_t(n.profNoms[n.slot].size()));
    n.profNoms[n.slot].push_back(std::move(nom));
}
std::string NomImage(const ImageNative* i) {
    if (!i) return "-";
    char b[64];
    snprintf(b, sizeof b, "%ux%u f%d x%d", i->img.width, i->img.height, int(i->img.format), int(i->ech));
    return b;
}

void Barriere(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}

VkCommandBuffer Cmd() {
    if (!n.enregistre) {
        const Poste p = Basculer(kAttente);
        const auto t0 = Horloge::now();
        vkWaitForFences(n.vk->device, 1, &n.fences[n.slot], VK_TRUE, UINT64_MAX);
        n.attenteGpuMs += Ms(t0, Horloge::now());
        Basculer(p);
        const uint32_t q = 2 * uint32_t(n.slot);
        uint64_t ts[2];
        if (n.mesure[n.slot] && vkGetQueryPoolResults(n.vk->device, n.temps, q, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            ts[1] != ts[0])
            n.gpuMs += Ecart(ts[0], ts[1]);
        n.mesure[n.slot] = false;
        if (n.prof && n.profNoms[n.slot].size() > 1) {
            auto& noms = n.profNoms[n.slot];
            std::vector<uint64_t> t(noms.size());
            if (vkGetQueryPoolResults(n.vk->device, n.prof, n.slot * kProfMax, uint32_t(t.size()), t.size() * 8, t.data(), 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                for (size_t i = 1; i < t.size(); i++) {
                    static const uint32_t seq = getenv("WAKFU_PROFIL_SEQ") ? atoi(getenv("WAKFU_PROFIL_SEQ")) : 0;
                    if (seq && n.image >= seq && n.image < seq + 3)
                        fprintf(stderr, "[profil seq %u] %.4f ms %s\n", n.image, Ecart(t[i - 1], t[i]), noms[i].c_str());
                    auto& c = n.profCumul[noms[i]];
                    c.first += Ecart(t[i - 1], t[i]);
                    c.second++;
                }
        }
        n.profNoms[n.slot].clear();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        for (VkCommandBuffer c : {n.cmds[n.slot], n.envois[n.slot]}) { vkResetCommandBuffer(c, 0); vkBeginCommandBuffer(c, &bi); }
        if (n.temps) {   // envois : soumis avant les draws du lot
            vkCmdResetQueryPool(n.envois[n.slot], n.temps, q, 2);
            vkCmdWriteTimestamp(n.envois[n.slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, n.temps, q);
        }
        if (Mesure()) vkCmdResetQueryPool(n.envois[n.slot], n.prof, n.slot * kProfMax, kProfMax);
        n.enregistre = true;
        n.dynLie = false;
        n.pipelineLie = VK_NULL_HANDLE;
        Marque("debut");
        NouveauLot(uint32_t(n.slot), n.image);
    }
    return n.cmds[n.slot];
}

// Termine et soumet le lot courant (envois puis draws) ; `attendre` : GPU au repos au retour.
void Soumettre(bool attendre) {
    VkCommandBuffer cmd = Cmd();
    VkCommandBuffer lot[2] = {n.envois[n.slot], cmd};
    Marque("fin de lot");
    if (n.temps) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, n.temps, 2 * uint32_t(n.slot) + 1);
    n.mesure[n.slot] = n.temps != VK_NULL_HANDLE;
    n.lots++;
    for (VkCommandBuffer c : lot) vkEndCommandBuffer(c);
    n.enregistre = false;
    n.fini = std::max(n.fini, n.idSlot[n.slot]);   // Cmd() a attendu la barrière de cet emplacement
    n.idSlot[n.slot] = ++n.soumissions;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 2;
    si.pCommandBuffers = lot;
    vkResetFences(n.vk->device, 1, &n.fences[n.slot]);
    {
        std::lock_guard<std::mutex> l(n.vk->queueLock);
        vkQueueSubmit(n.vk->queue, 1, &si, n.fences[n.slot]);
    }
    if (attendre) {
        const Poste p = Basculer(kAttente);
        const auto t0 = Horloge::now();
        vkWaitForFences(n.vk->device, 1, &n.fences[n.slot], VK_TRUE, UINT64_MAX);
        n.attenteGpuMs += Ms(t0, Horloge::now());
        Basculer(p);
    }
}

void FermerPasse();
// Rappels dont le lot est fini exécutés, dans l'ordre. `bloquer` : le premier est attendu (au plus 200 µs, l'appelant
// reboucle), son lot soumis s'il est encore en enregistrement (le jeu peut l'attendre avant d'envoyer la suite).
void Retirer(bool bloquer) {
    for (int s = 0; s < 2; s++)
        if (n.idSlot[s] > n.fini && vkGetFenceStatus(n.vk->device, n.fences[s]) == VK_SUCCESS) n.fini = std::max(n.fini, n.idSlot[s]);
    if (bloquer && !n.rappels.empty() && n.rappels.front().first > n.fini) {
        const uint64_t id = n.rappels.front().first;
        if (id > n.soumissions) { FermerPasse(); Soumettre(false); }
        const int s = n.idSlot[0] >= id && (n.idSlot[1] < id || n.idSlot[0] < n.idSlot[1]) ? 0 : 1;
        const Poste p = Basculer(kAttente);
        const auto t0 = Horloge::now();
        if (vkWaitForFences(n.vk->device, 1, &n.fences[s], VK_TRUE, 200000) == VK_SUCCESS) n.fini = std::max(n.fini, n.idSlot[s]);
        n.attenteGpuMs += Ms(t0, Horloge::now());
        Basculer(p);
    }
    while (!n.rappels.empty() && n.rappels.front().first <= n.fini) {
        n.rappels.front().second();
        n.rappels.pop_front();
    }
}

bool Profondeur(VkFormat f) { return vk::ImageAspect(f) != VK_IMAGE_ASPECT_COLOR_BIT; }

bool CreerImage(uint32_t w, uint32_t h, uint32_t formatInvite, VkSampleCountFlagBits ech, bool attache, ImageNative* out) {
    const VkFormat f = FormatCible(formatInvite, n.d24s8);
    if (f == VK_FORMAT_UNDEFINED || !w || !h || w > 8192 || h > 8192) {
        n.stats.formatsInconnus++;
        return false;
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // profondeur : attachable même en texture (destination d'un resolve MSAA, fait par une passe de rendu)
    if (attache || Profondeur(f)) u |= Profondeur(f) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ImageNative r;
    if (!n.vk->CreateImage(w, h, f, u, &r.img, ech)) {
        n.vk->DestroyImage(r.img);
        n.stats.formatsInconnus++;
        return false;
    }
    r.ech = ech;
    r.formatInvite = formatInvite;
    r.lecture = r.img.view;
    if (Profondeur(f)) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r.img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = f;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(n.vk->device, &vi, nullptr, &r.lecture) != VK_SUCCESS) r.lecture = VK_NULL_HANDLE;
    }
    vk::Context::Transition(Cmd(), r.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, vk::ImageAspect(f));
    *out = r;
    return true;
}

void Remplacer(std::unordered_map<uint32_t, ImageNative>& m, uint32_t cle, const ImageNative& img) {
    auto it = m.find(cle);
    if (it != m.end()) n.aDetruire.push_back({n.image + 2, it->second});   // encore utilisée par les lots en vol
    m[cle] = img;
    n.versionTextures++;
}

ImageNative* Trouver(std::unordered_map<uint32_t, ImageNative>& m, uint32_t cle) {
    auto it = cle ? m.find(cle) : m.end();
    return it == m.end() ? nullptr : &it->second;
}

void FermerPasse() {
    if (!n.passe) return;
    vkCmdEndRendering(Cmd());
    if (Mesure() && g_profDetail) {
        std::string nom = "passe";
        for (uint32_t r : n.passeRt) if (r) nom += " C[" + NomImage(Trouver(n.surfaces, r)) + "]";
        if (n.passeDs) nom += " D[" + NomImage(Trouver(n.surfaces, n.passeDs)) + "]";
        Marque(nom);
    } else if (Mesure()) {
        const ImageNative* c = Trouver(n.surfaces, n.passeDs);
        for (uint32_t r : n.passeRt) if (!c && r) c = Trouver(n.surfaces, r);
        Marque(c && c->ech != VK_SAMPLE_COUNT_1_BIT ? "passes MSAA" : "passes");
    }
    Barriere(Cmd());
    n.passe = false;
}

// Passe de rendu sur les cibles courantes (chargement et stockage : l'état des cibles persiste entre passes).
bool OuvrirPasse() {
    if (n.passe && !memcmp(n.passeRt, n.rt, sizeof n.rt) && n.passeDs == n.ds) return true;
    FermerPasse();
    VkRenderingAttachmentInfo couleur[4]{}, prof{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t nb = 0, w = UINT32_MAX, h = UINT32_MAX;
    for (int i = 0; i < 4; i++) {
        ImageNative* c = Trouver(n.surfaces, n.rt[i]);
        if (!c) { if (n.rt[i]) n.stats.sansCible++; continue; }
        couleur[nb] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        couleur[nb].imageView = c->img.view;
        couleur[nb].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        couleur[nb].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        couleur[nb].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        nb++;
        n.passeEchelle = c->echelle;
        w = std::min(w, c->img.width);
        h = std::min(h, c->img.height);
    }
    ImageNative* d = Trouver(n.surfaces, n.ds);
    // Zone 1 : la passe de l'ombre (512x512) garde la profondeur 1280x720 attachée sans s'en servir ; agrandie par
    // l'option ombres, elle limiterait la passe à la taille de la profondeur.
    n.passeSansDs = d && nb && d->echelle != n.passeEchelle;
    if (n.passeSansDs) d = nullptr;
    if (d) {
        prof.imageView = d->img.view;
        prof.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        prof.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        prof.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!nb) n.passeEchelle = d->echelle;
        w = std::min(w, d->img.width);
        h = std::min(h, d->img.height);
    } else if (n.ds && !n.passeSansDs) n.stats.sansCible++;
    if (!nb && !d) return false;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = nb;
    ri.pColorAttachments = couleur;
    ri.pDepthAttachment = d ? &prof : nullptr;
    ri.pStencilAttachment = d ? &prof : nullptr;
    vkCmdBeginRendering(Cmd(), &ri);
    memcpy(n.passeRt, n.rt, sizeof n.rt);
    n.passeDs = n.ds;
    n.zone = {w, h};
    n.passe = true;
    return true;
}

// Rectangle invité (x0, y0, x1, y1) en pixels natifs d'une cible d'échelle s, borné à `max`.
VkRect2D Rect(const Commande& c, VkExtent2D max, uint32_t s) {
    if (!c.avecRect) return {{0, 0}, max};
    int32_t g[4];
    memcpy(g, c.rect, sizeof g);
    AgrandirRect(g, s);
    const int32_t x0 = std::clamp(g[0], 0, int32_t(max.width)), y0 = std::clamp(g[1], 0, int32_t(max.height));
    const int32_t x1 = std::clamp(g[2], x0, int32_t(max.width)), y1 = std::clamp(g[3], y0, int32_t(max.height));
    return {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

// D3DCLEAR 360 : TARGET0-3 0x1-0x8, ZBUFFER 0x10, STENCIL 0x20
void FaireEffacer(const Commande& c) {
    if (!OuvrirPasse()) return;
    n.stats.effacements++;
    VkClearAttachment at[5];
    uint32_t nb = 0, idx = 0;
    for (int i = 0; i < 4; i++) {
        if (!Trouver(n.surfaces, n.rt[i])) continue;
        if (c.a & (1u << i)) {
            at[nb] = {VK_IMAGE_ASPECT_COLOR_BIT, idx, {}};
            memcpy(at[nb].clearValue.color.float32, c.couleur, 16);
            nb++;
        }
        idx++;
    }
    if (!n.passeSansDs && Trouver(n.surfaces, n.ds) && (c.a & 0x30)) {
        at[nb] = {VkImageAspectFlags((c.a & 0x10 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (c.a & 0x20 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, {}};
        at[nb].clearValue.depthStencil = {c.z, c.b & 0xFF};
        nb++;
    }
    VkClearRect r{Rect(c, n.zone, n.passeEchelle), 0, 1};
    if (nb && r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(Cmd(), nb, at, 1, &r);
}

// D3DRESOLVE 360 : bits 0-1 cible couleur, 0x4 profondeur, 0x100 / 0x200 effacer la cible / la profondeur après.
// Resolve non fait : compté par cause, les 12 premiers décrits une fois.
void NonFait(Cause k, const Commande& c, const ImageNative* src, const ImageNative* dst) {
    static int decrits = 0;
    n.stats.resolvesNonFaits++;
    n.stats.causes[k]++;
    if (decrits++ < 12)
        fprintf(stderr, "[natif] resolve non fait (%s) : flags %X, dest %08X %ux%u fmt %u niveau %u point %u,%u, rect %s%d,%d-%d,%d ; "
                "source %s %ux%u vk %d x%d, dest vk %d\n", kNomsCauses[k], c.a, c.b, c.w, c.h, c.format, c.d, c.e >> 16, c.e & 0xFFFF,
                c.avecRect ? "" : "(aucun) ", c.rect[0], c.rect[1], c.rect[2], c.rect[3], c.a & 4 ? "Z" : "RT",
                src ? src->img.width : 0, src ? src->img.height : 0, src ? int(src->img.format) : 0, src ? int(src->ech) : 0,
                dst ? int(dst->img.format) : 0);
}

// Effacement d'une cible après un resolve : passe vide en CLEAR limitée au rectangle du resolve (toute l'image sans
// rectangle), comme la console, à l'échelle de la cible.
void EffacerApresResolve(VkCommandBuffer cmd, const ImageNative& t, const Commande& c, const VkClearValue& v) {
    const VkImageAspectFlags aspect = vk::ImageAspect(t.img.format);
    const VkRect2D zone = Rect(c, {t.img.width, t.img.height}, t.echelle);
    // Image entière : contenu abandonné (UNDEFINED) puis passe CLEAR en disposition d'attache, que le pilote peut
    // effacer « vite » (métadonnées de compression seules) ; en GENERAL, l'effacement écrit chaque pixel.
    const bool entiere = !zone.offset.x && !zone.offset.y && zone.extent.width == t.img.width && zone.extent.height == t.img.height;
    const VkImageLayout attache = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                                      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (entiere) vk::Context::Transition(cmd, t.img.image, VK_IMAGE_LAYOUT_UNDEFINED, attache, aspect);
    VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    a.imageView = t.img.view;
    a.imageLayout = entiere ? attache : VK_IMAGE_LAYOUT_GENERAL;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.clearValue = v;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = zone;
    ri.layerCount = 1;
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) ri.colorAttachmentCount = 1, ri.pColorAttachments = &a;
    else {
        ri.pDepthAttachment = &a;
        if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &a;
    }
    if (!ri.renderArea.extent.width || !ri.renderArea.extent.height) return;
    vkCmdBeginRendering(cmd, &ri);
    vkCmdEndRendering(cmd);
    if (entiere) vk::Context::Transition(cmd, t.img.image, attache, VK_IMAGE_LAYOUT_GENERAL, aspect);
}

void FaireResoudre(const Commande& c) {
    FermerPasse();
    n.versionTextures++;   // vues résolues changées (VueResolue)
    n.stats.resolves++;
    const bool prof = c.a & 4;
    const uint32_t cleSrc = prof ? n.ds : n.rt[c.a & 3];
    ImageNative* src = Trouver(n.surfaces, cleSrc);
    if (!src) { NonFait(kSansSource, c, src, nullptr); return; }
    const uint32_t s = src->echelle;   // destination à l'échelle de la source
    // Tuilage (zone 1 : surface 4x de 320x720, 4 bandes résolues aux rectangles écran 0-320 ... 960-1280) : le natif
    // dessine une seule fois en coordonnées écran, la surface est agrandie au rectangle demandé (effet à l'image suivante).
    // Les autres surfaces attachées à la taille de la bande le sont aussi : la passe est limitée à la plus petite
    // (Grougal : cible 1024x1024 en 2 bandes de 1024x512, profondeur jamais résolue, moitié basse jamais dessinée).
    if (c.avecRect && (c.rect[2] * int32_t(s) > int32_t(src->img.width) || c.rect[3] * int32_t(s) > int32_t(src->img.height))) {
        const uint32_t w0 = src->img.width, h0 = src->img.height;
        const uint32_t w = std::max<uint32_t>(c.rect[2] * s, w0), h = std::max<uint32_t>(c.rect[3] * s, h0);
        for (uint32_t cle : {n.rt[0], n.rt[1], n.rt[2], n.rt[3], n.ds}) {
            const ImageNative* a = Trouver(n.surfaces, cle);
            ImageNative grande;
            if (a && a->img.width == w0 && a->img.height == h0 && a->echelle == s &&
                CreerImage(w, h, a->formatInvite, a->ech, true, &grande)) {
                grande.echelle = s;
                Remplacer(n.surfaces, cle, grande);
            }
        }
        src = Trouver(n.surfaces, cleSrc);
    }
    if (c.d) { NonFait(kMip, c, src, nullptr); return; }   // niveau de mip > 0 : à faire
    ImageNative* dst = Trouver(n.textures, c.b);
    if (!dst || dst->echelle != s || dst->img.width != c.w * s || dst->img.height != c.h * s || dst->formatInvite != c.format) {
        ImageNative nouvelle;
        if (!CreerImage(c.w * s, c.h * s, c.format, VK_SAMPLE_COUNT_1_BIT, true, &nouvelle)) { NonFait(kCreation, c, src, nullptr); return; }
        nouvelle.echelle = s;
        Remplacer(n.textures, c.b, nouvelle);
        dst = &n.textures[c.b];
    }
    VkCommandBuffer cmd = Cmd();
    Barriere(cmd);
    const VkRect2D r = Rect(c, {src->img.width, src->img.height}, s);
    // Resolve de toute la cible couleur suivi de son effacement (0x100) : la cible et la texture échangent leurs
    // images au lieu d'une copie, puis la (nouvelle) cible est effacée. Même résultat, une copie plein écran de moins.
    if ((c.a & 0x104) == 0x100 && src->ech == VK_SAMPLE_COUNT_1_BIT && !c.e && !r.offset.x && !r.offset.y &&
        r.extent.width == src->img.width && r.extent.height == src->img.height && dst->img.width == src->img.width &&
        dst->img.height == src->img.height && dst->img.format == src->img.format && dst->ech == VK_SAMPLE_COUNT_1_BIT &&
        !(g_aPresenter.actif && (g_aPresenter.image == dst->img.image || g_aPresenter.image == src->img.image))) {
        std::swap(src->img, dst->img);
        std::swap(src->lecture, dst->lecture);
        dst->resolue = true;
        n.stats.echanges++;
        if (n.prof) Marque(g_profDetail ? "resolve C echange " + NomImage(dst) : "resolves");
    } else {
        const int32_t dx = PointX(c.e, s), dy = PointY(c.e, s);   // point de destination (x << 16 | y)
        const uint32_t w = std::min<int64_t>(r.extent.width, std::max<int64_t>(0, int64_t(dst->img.width) - dx));
        const uint32_t h = std::min<int64_t>(r.extent.height, std::max<int64_t>(0, int64_t(dst->img.height) - dy));
        const VkImageAspectFlags aspect = vk::ImageAspect(src->img.format);
        const VkImageSubresourceLayers sub{aspect, 0, 0, 1};
        bool fait = false;
        if (w && h && src->img.format == dst->img.format) {
            if (src->ech != VK_SAMPLE_COUNT_1_BIT && prof && dx == r.offset.x && dy == r.offset.y) {
                // profondeur MSAA : vkCmdResolveImage ne la fait pas ; passe vide avec résolution échantillon 0 (cœur 1.2)
                VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                a.imageView = src->img.view;
                a.imageLayout = a.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
                a.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                a.resolveImageView = dst->img.view;
                a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {r.offset, {w, h}};
                ri.layerCount = 1;
                ri.pDepthAttachment = ri.pStencilAttachment = &a;
                vkCmdBeginRendering(cmd, &ri);
                vkCmdEndRendering(cmd);
                fait = true;
            } else if (src->ech != VK_SAMPLE_COUNT_1_BIT && !prof) {
                VkImageResolve rr{sub, {r.offset.x, r.offset.y, 0}, sub, {dx, dy, 0}, {w, h, 1}};
                vkCmdResolveImage(cmd, src->img.image, VK_IMAGE_LAYOUT_GENERAL, dst->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &rr);
                fait = true;
            } else if (src->ech == VK_SAMPLE_COUNT_1_BIT) {
                VkImageCopy cp{sub, {r.offset.x, r.offset.y, 0}, sub, {dx, dy, 0}, {w, h, 1}};
                vkCmdCopyImage(cmd, src->img.image, VK_IMAGE_LAYOUT_GENERAL, dst->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
                fait = true;
            }
        } else if (w && h && !prof && !Profondeur(dst->img.format) && src->ech == VK_SAMPLE_COUNT_1_BIT) {
            VkImageBlit b{sub, {{r.offset.x, r.offset.y, 0}, {r.offset.x + int32_t(w), r.offset.y + int32_t(h), 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + int32_t(w), dy + int32_t(h), 1}}};
            vkCmdBlitImage(cmd, src->img.image, VK_IMAGE_LAYOUT_GENERAL, dst->img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
            fait = true;
        }
        if (n.prof) Marque(g_profDetail ? std::string(prof ? "resolve Z " : "resolve C ") + NomImage(src) + " -> " + NomImage(dst) : "resolves");
        if (fait) dst->resolue = true;
        else NonFait(!w || !h ? kVide : prof && src->ech != VK_SAMPLE_COUNT_1_BIT ? kProfMsaa : kFormats, c, src, dst);
        Barriere(cmd);
    }
    // Effacements demandés avec le resolve (sur la cible source et/ou la profondeur courante)
    if (c.a & 0x100) {
        if (ImageNative* t = Trouver(n.surfaces, n.rt[c.a & 3])) {
            VkClearValue v{};
            memcpy(v.color.float32, c.couleur, 16);
            EffacerApresResolve(cmd, *t, c, v);
        }
    }
    if (c.a & 0x200) {
        if (ImageNative* t = Trouver(n.surfaces, n.ds)) {
            VkClearValue v{};
            v.depthStencil = {c.z, c.c & 0xFF};
            EffacerApresResolve(cmd, *t, c, v);
        }
    }
    if (c.a & 0x300) {
        if (Mesure()) Marque(g_profDetail ? "effacement apres resolve " + NomImage(Trouver(n.surfaces, n.rt[c.a & 3])) : "effacements");
        Barriere(cmd);
    }
}

uint64_t Hache(const void* p, size_t s, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < s; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    return h;
}

VkPipeline Pipeline(const Instantane& d, VkPrimitiveTopology topo, const ShadersNatifs& sh, const SommetsNatifs& so) {
    VkFormat fc[4];
    uint32_t nb = 0;
    VkSampleCountFlagBits ech = VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < 4; i++)
        if (ImageNative* c = Trouver(n.surfaces, n.passeRt[i])) { fc[nb++] = c->img.format; ech = c->ech; }
    ImageNative* ds = n.passeSansDs ? nullptr : Trouver(n.surfaces, n.passeDs);
    if (ds) ech = ds->ech;
    // les masques et la référence du stencil sont dynamiques : hors de la clé
    const uint32_t cleRegs[5] = {d.regs.depthControl, d.regs.blendControl, d.regs.colorControl, d.regs.modeControl, d.regs.colorMask};
    uint64_t k = Hache(cleRegs, sizeof cleRegs);
    k = Hache(fc, nb * sizeof(VkFormat), k);
    const uint64_t reste[5] = {sh.cle, so.cle, uint64_t(topo), uint64_t(ech), ds ? uint64_t(ds->img.format) : 0};
    k = Hache(reste, sizeof reste, k);
    auto it = n.pipelines.find(k);
    if (it != n.pipelines.end()) return it->second;

    const EtatPipeline e = TraduireEtat(d.regs, ds != nullptr);
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = sh.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = sh.ps; st[1].pName = "main";
    st[0].pSpecializationInfo = st[1].pSpecializationInfo = sh.specialisation;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = e.polygone;
    rs.cullMode = e.cull;
    rs.frontFace = e.face;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = ech;
    ms.alphaToCoverageEnable = e.alphaVersCouverture;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = nb;
    cb.pAttachments = e.blend;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 6;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = nb;
    ri.pColorAttachmentFormats = fc;
    if (ds) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = ds->img.format;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = 2;
    pci.pStages = st;
    pci.pVertexInputState = so.entree;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &e.profondeur;
    pci.pDynamicState = &dy;
    pci.layout = sh.disposition;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(n.vk->device, VK_NULL_HANDLE, 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return n.pipelines[k] = p;
}

void FaireDessiner(const Instantane& d) {
    n.stats.draws++;
    VkPrimitiveTopology topo;
    // shortcut: quads indexés depuis un IB invité non traduits (indices à relire côté GPU), absents des zones mesurées
    if (!Topologie(d.prim, &topo) || (d.prim == 13 && d.type == 1)) {
        static uint64_t vues = 0;   // journal une fois par type de primitive
        if (!(vues >> (d.prim & 63) & 1)) fprintf(stderr, "[natif %u] primitive %u non traduite (draw type %u)\n", n.image, d.prim, d.type);
        vues |= 1ull << (d.prim & 63);
        n.stats.primitive++;
        return;
    }
    ShadersNatifs sh;
    Basculer(kShaders);
    if (!ObtenirShaders(d, &sh)) { n.stats.sansShader++; n.stats.shader[sh.echec & 3]++; return; }
    Basculer(kPasses);
    if (!OuvrirPasse()) { n.stats.sansPasse++; return; }
    Basculer(kSommets);
    SommetsNatifs so;
    const uint32_t s = n.passeEchelle;
    so.cible = {n.zone.width / s, n.zone.height / s};   // sommets en pixels invités (VTE sans échelle)
    if (!ObtenirSommets(d, &so)) { n.stats.sansSommets++; n.stats.sommets[so.echec % 3]++; return; }
    Basculer(kPipeline);
    VkPipeline p = Pipeline(d, topo, sh, so);
    if (!p) { n.stats.pipeline++; return; }
    Basculer(kCommandes);
    VkCommandBuffer cmd = Cmd();
    if (p != n.pipelineLie) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    n.pipelineLie = p;
    // hauteur positive : les VS précompilés inversent déjà Y (-fvk-invert-y) ; même sens des faces qu'un viewport
    // négatif sans inversion (le sens est jugé dans le repère de l'image), cf. etats.h
    Natif::Dynamique y{};   // initialisé à zéro : comparé octet à octet
    float vp[6];
    memcpy(vp, d.viewport, sizeof vp);
    AgrandirViewport(vp, s);
    y.v = {vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]};
    Commande sc{Op::Dessiner};
    sc.avecRect = d.scissorActif;
    memcpy(sc.rect, d.scissor, sizeof sc.rect);
    y.r = Rect(sc, n.zone, s);
    memcpy(y.melange, d.constanteMelange, sizeof y.melange);
    const EtatPipeline e = TraduireEtat(d.regs, true);
    const auto& pf = e.profondeur;
    const uint32_t st[6] = {pf.front.reference, pf.back.reference, pf.front.compareMask, pf.back.compareMask,
                            pf.front.writeMask, pf.back.writeMask};
    memcpy(y.stencil, st, sizeof st);
    if (!n.dynLie || memcmp(&y.v, &n.dyn.v, sizeof y.v)) vkCmdSetViewport(cmd, 0, 1, &y.v);
    if (!n.dynLie || memcmp(&y.r, &n.dyn.r, sizeof y.r)) vkCmdSetScissor(cmd, 0, 1, &y.r);
    if (!n.dynLie || memcmp(y.melange, n.dyn.melange, sizeof y.melange)) vkCmdSetBlendConstants(cmd, y.melange);
    if (!n.dynLie || memcmp(y.stencil, n.dyn.stencil, sizeof y.stencil)) {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, st[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, st[1]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st[2]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, st[3]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, st[4]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, st[5]);
    }
    n.dyn = y;
    n.dynLie = true;
    const int lie = sh.lier ? sh.lier(cmd, sh.disposition, d) : 0;
    Basculer(kCommandes);
    if (lie == 2) { n.stats.liaison++; return; }
    if (lie == 1) n.stats.texturesSecours++;
    if (so.dessiner) so.dessiner(cmd, d);
    n.stats.dessines++;
    if (n.prof && g_profDetail == 2) {
        char b[160];
        snprintf(b, sizeof b, "dessin n %u t %u vs %08X ps %08X prim %u blend %08X z %08X cible %ux%u x%d", d.nombre, d.type, d.vs, d.ps, d.prim, d.regs.blendControl,
                 d.regs.depthControl, n.zone.width, n.zone.height, int(Trouver(n.surfaces, n.passeRt[0]) ? Trouver(n.surfaces, n.passeRt[0])->ech : 0));
        Marque(b);
    }
}

// Recopie RGBA d'une image couleur 4 octets (R8G8B8A8 / B8G8R8A8) ; fil natif, GPU au repos.
bool Relire(const vk::Image& img, std::vector<uint8_t>* rgba) {
    const bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM;
    if (img.format != VK_FORMAT_R8G8B8A8_UNORM && !bgra) return false;
    const VkDeviceSize taille = VkDeviceSize(img.width) * img.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = taille;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(n.vk->device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(n.vk->device, buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = n.vk->FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(n.vk->device, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(n.vk->device, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(n.vk->device, buf, mem, 0);
    VkCommandBuffer cmd = Cmd();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {img.width, img.height, 1};
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
    Soumettre(true);
    void* p;
    vkMapMemory(n.vk->device, mem, 0, taille, 0, &p);
    rgba->assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + taille);
    vkUnmapMemory(n.vk->device, mem);
    vkFreeMemory(n.vk->device, mem, nullptr);
    vkDestroyBuffer(n.vk->device, buf, nullptr);
    if (bgra) for (size_t i = 0; i < rgba->size(); i += 4) std::swap((*rgba)[i], (*rgba)[i + 2]);
    return true;
}

// Présente l'image en attente si le CP a atteint son swap ; `attendre` : jusqu'à 100 ms (avant de soumettre l'image
// suivante, qui peut réécrire la même texture ; le swap attendu ne dépend que du vblank, jamais du fil natif).
void Presenter(bool attendre) {
    if (!g_aPresenter.actif) return;
    {
        std::unique_lock<std::mutex> l(g_mutex);
        if (attendre) {
            const Poste p = Basculer(kAttente);
            g_cv.wait_for(l, std::chrono::milliseconds(100), PretAPresenter);
            Basculer(p);
        }
        else if (!PretAPresenter()) return;
    }
    g_aPresenter.actif = false;
    trace::Mark('N', g_aPresenter.swap);
    const auto t0 = Horloge::now();
    window::PresentNative(g_aPresenter.image, g_aPresenter.w, g_aPresenter.h);
    n.presentMs += Ms(t0, Horloge::now());
}

void FaireEchange(const Commande& c) {
    Presenter(true);
    FermerPasse();
    Soumettre(false);
    // image affichée ; WAKFU_CAPTURE : image présentée (numéro VdSwap = image native + 1)
    ImageNative* img = Trouver(n.textures, c.a);
    if (!img) img = Trouver(n.surfaces, n.rt[0]);
    if (img && img->ech == VK_SAMPLE_COUNT_1_BIT) {
        g_aPresenter = {true, img->img.image, img->img.width, img->img.height, c.b};
        Presenter(false);   // swap déjà atteint (jeu en retard) : tout de suite
    }
    else if (n.image < 3 || n.image % 600 == 0) fprintf(stderr, "[natif %u] pas d'image native à présenter\n", n.image);
    std::string chemin;
    const bool touche = window::CaptureDemandee();
    if (touche) {   // option touche_capture : <données>/captures/capture_<date>_<image>.png
        char date[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(date, sizeof date, "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
        std::error_code ec;
        std::filesystem::create_directories(g_dataDir + "/captures", ec);
        chemin = g_dataDir + "/captures/capture_" + date + "_" + std::to_string(n.image + 1) + ".png";
    }
    if (!chemin.empty() || CaptureVoulue(n.image + 1, &chemin)) {
        std::vector<uint8_t> rgba;
        const bool ok = img && img->ech == VK_SAMPLE_COUNT_1_BIT && Relire(img->img, &rgba) &&
                        WritePng(chemin, img->img.width, img->img.height, rgba);
        fprintf(stderr, "[capture] %s (image native présentée) : %s\n", chemin.c_str(), ok ? "écrite" : "échec");
        if (ok && touche) window::CaptureFaite();
    }
    n.slot ^= 1;
    for (size_t i = 0; i < n.aDetruire.size();) {
        if (n.aDetruire[i].first <= n.image) {
            ImageNative& d = n.aDetruire[i].second;
            if (d.lecture && d.lecture != d.img.view) vkDestroyImageView(n.vk->device, d.lecture, nullptr);
            n.vk->DestroyImage(d.img);
            n.aDetruire[i] = n.aDetruire.back();
            n.aDetruire.pop_back();
        } else i++;
    }
    const Stats& s = n.stats;
    std::string causes;
    for (int k = 0; k < kCauses; k++)
        if (s.causes[k]) causes += std::string(causes.empty() ? " ; non faits : " : ", ") + kNomsCauses[k] + " " + std::to_string(s.causes[k]);
    if (n.image % 60 == 0)
        fprintf(stderr, "[natif %u] draws %u, rendus %u (textures de secours %u) ; ignorés : shader non vu à sa création %u, "
                "SPIR-V absent %u, GPU sans les fonctions %u, déclaration absente %u, tampon indisponible %u, "
                "primitive %u, pipeline %u, liaison %u, sans cible %u ; effacements %u, resolves %u (non faits %u), "
                "cibles absentes %u, formats inconnus %u, surfaces %u ; textures natives %u ; resolves fusionnés %u, échangés %u%s\n",
                n.image, s.draws, s.dessines, s.texturesSecours, s.shader[1], s.shader[2], s.shader[3], s.sommets[1],
                s.sommets[2], s.primitive, s.pipeline, s.liaison,
                s.sansPasse, s.effacements,
                s.resolves, s.resolvesNonFaits, s.sansCible, s.formatsInconnus, s.surfaces, s.texturesNatives, s.fusions, s.echanges,
                causes.c_str());
    if (n.image % 60 == 0) {
        const double t = platform::ThreadCpuMs(), cpu = t - n.cpu0;
        std::string parts;   // répartition du GPU (catégories de Marque, 10 dernières images)
        if (n.prof && !g_profDetail) {
            for (const char* k : {"passes", "passes MSAA", "resolves", "effacements"}) {
                char b[48];
                snprintf(b, sizeof b, "%s%s %.2f", parts.empty() ? " : " : ", ", k, n.profCumul[k].first / 10);
                parts += b;
            }
            n.profCumul.clear();
        }
        std::string postes;   // répartition du fil natif (10 images mesurées, attentes à part)
        for (int k = 0; k < kPostes; k++) {
            char b[48];
            snprintf(b, sizeof b, "%s%s %.2f", postes.empty() ? " ; postes (ms/image, 10 dernières) : " : ", ", kNomsPostes[k], n.postes[k] / 10);
            postes += b;
            n.postes[k] = 0;
        }
        if (n.image) fprintf(stderr, "[natif %u] par image (60 dernières) : GPU %.2f ms (%.1f lots%s), CPU du fil natif %.2f ms, "
                             "attente des fences %.2f ms, présentation %.2f ms%s\n",
                             n.image, n.gpuMs / 60, n.lots / 60.0, parts.c_str(), cpu / 60, n.attenteGpuMs / 60, n.presentMs / 60, postes.c_str());
        n.attenteGpuMs = n.presentMs = 0;
        n.cpu0 = t;
        n.gpuMs = 0;
        if (n.prof && g_profDetail && n.image % 240 == 0 && n.image) {
            std::vector<std::pair<double, std::string>> v;
            for (auto& [nom, c] : n.profCumul) v.push_back({c.first, nom + " (" + std::to_string(c.second / 240.0).substr(0, 4) + "/image)"});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 25; i++) fprintf(stderr, "[profil gpu %u] %.3f ms/image : %s\n", n.image, v[i].first / 240, v[i].second.c_str());
            n.profCumul.clear();
        }
        n.lots = 0;
    }
    n.stats = Stats{};
    n.image++;
    n.chrono = n.image % 60 >= 50;
    if (n.chrono) n.t = std::chrono::steady_clock::now();
}

bool Initialiser() {
    while (!(n.vk = g_vk.load())) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = n.vk->queueFamily;
    if (vkCreateCommandPool(n.vk->device, &pci, nullptr, &n.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = n.pool;
    ai.commandBufferCount = 2;
    vkAllocateCommandBuffers(n.vk->device, &ai, n.cmds);
    vkAllocateCommandBuffers(n.vk->device, &ai, n.envois);
    if (!::natif::BuffersInit(n.vk)) fprintf(stderr, "[natif] tampons de sommets indisponibles\n");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (VkFence& f : n.fences) vkCreateFence(n.vk->device, &fci, nullptr, &f);
    if (n.vk->timestampPeriod > 0) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 4;
        if (vkCreateQueryPool(n.vk->device, &qi, nullptr, &n.temps) != VK_SUCCESS) n.temps = VK_NULL_HANDLE;
        qi.queryCount = 2 * kProfMax;
        const char* e = getenv("WAKFU_PROFIL_GPU");
        g_profDetail = e ? atoi(e) : 0;
        if (vkCreateQueryPool(n.vk->device, &qi, nullptr, &n.prof) != VK_SUCCESS) n.prof = VK_NULL_HANDLE;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(n.vk->physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) n.d24s8 = VK_FORMAT_D32_SFLOAT_S8_UINT;
    g_msaa = config::MsaaEchantillons(config::g.msaa, n.vk->deviceType);
    while (g_msaa > 1 && !(n.vk->sampleCounts & g_msaa)) g_msaa /= 2;   // plafond du GPU
    if (g_msaa == 1) g_msaa = 0;
    n.echelle = uint32_t(std::clamp(config::g.renderScale, 1, 4));
    n.ombres = config::g.ombres == "x4" ? 4 : config::g.ombres == "x2" ? 2 : 1;
    fprintf(stderr, "[natif] rendu natif sur %s (type %d) ; profondeur %s ; multi-échantillonnage x%u (%s ; GPU : 0x%X) ; définition interne x%u ; ombres x%u ; anisotrope x%g (GPU : x%g)\n",
            n.vk->deviceName.c_str(), n.vk->deviceType, n.d24s8 == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
            g_msaa, config::g.msaa.c_str(), n.vk->sampleCounts, n.echelle, n.ombres,
            config::g.anisotropy > 1 ? std::min(float(config::g.anisotropy), n.vk->maxAnisotropy) : 1.0f, n.vk->maxAnisotropy);
    return true;
}

// Resolves en bandes (zone 1 : 4 bandes de 320x720, couleur ou profondeur, à leur place dans la texture) : le natif
// dessine une seule fois en plein écran, les bandes qui se suivent sans autre commande sont fusionnées en un resolve
// (et un effacement) du rectangle réuni. Bandes disjointes : l'effacement d'une bande ne touche pas les suivantes.
Commande g_resolveEnAttente{Op::Cible};
bool g_resolveAttend = false;
bool Fusionner(Commande& p, const Commande& c) {
    if (!p.avecRect || !c.avecRect || p.a != c.a || p.b != c.b || p.c != c.c || p.d != c.d || p.w != c.w || p.h != c.h ||
        p.format != c.format || p.z != c.z || memcmp(p.couleur, c.couleur, sizeof p.couleur))
        return false;
    const int32_t pdx = int32_t(p.e >> 16) - p.rect[0], pdy = int32_t(p.e & 0xFFFF) - p.rect[1];
    const int32_t cdx = int32_t(c.e >> 16) - c.rect[0], cdy = int32_t(c.e & 0xFFFF) - c.rect[1];
    if (pdx != cdx || pdy != cdy) return false;   // même translation source -> destination
    const bool horiz = p.rect[1] == c.rect[1] && p.rect[3] == c.rect[3] && (p.rect[2] == c.rect[0] || c.rect[2] == p.rect[0]);
    const bool vert = p.rect[0] == c.rect[0] && p.rect[2] == c.rect[2] && (p.rect[3] == c.rect[1] || c.rect[3] == p.rect[1]);
    if (!horiz && !vert) return false;
    const int32_t x0 = std::min(p.rect[0], c.rect[0]), y0 = std::min(p.rect[1], c.rect[1]);
    if (x0 + pdx < 0 || y0 + pdy < 0) return false;
    p.rect[0] = x0;
    p.rect[1] = y0;
    p.rect[2] = std::max(p.rect[2], c.rect[2]);
    p.rect[3] = std::max(p.rect[3], c.rect[3]);
    p.e = uint32_t(x0 + pdx) << 16 | uint32_t(y0 + pdy);
    n.stats.fusions++;
    return true;
}
void ResolveEnAttente() {
    if (!g_resolveAttend) return;
    g_resolveAttend = false;
    Basculer(kResolves);
    FaireResoudre(g_resolveEnAttente);
    Basculer(kAutres);
}

void Traiter(Commande& c) {
    if (c.op == Op::Resoudre) {
        if (g_resolveAttend && Fusionner(g_resolveEnAttente, c)) return;
        ResolveEnAttente();
        g_resolveEnAttente = std::move(c);
        g_resolveAttend = true;
        return;
    }
    ResolveEnAttente();
    switch (c.op) {
        case Op::Surface: {
            n.stats.surfaces++;
            ImageNative img;
            // cible de l'ombre (zone 1 : masque 512x512, couleur sans MSAA) agrandie par l'option ombres
            const bool ombre = c.b == 512 && c.c == 512 && Echantillons(c.e) == VK_SAMPLE_COUNT_1_BIT &&
                               !Profondeur(FormatCible(c.d, n.d24s8));
            const uint32_t s = n.echelle * (ombre ? n.ombres : 1);
            if (CreerImage(c.b * s, c.c * s, c.d, g_msaa && Echantillons(c.e) != VK_SAMPLE_COUNT_1_BIT ? VkSampleCountFlagBits(g_msaa) : VK_SAMPLE_COUNT_1_BIT, true, &img)) {
                img.echelle = s;
                FermerPasse();   // l'ancienne image de ce pointeur peut être attachée
                Remplacer(n.surfaces, c.a, img);
            } else fprintf(stderr, "[natif] surface %08X %ux%u : format %08X non pris en charge\n", c.a, c.b, c.c, c.d);
            break;
        }
        case Op::Texture: {
            ImageNative* t = Trouver(n.textures, c.a);
            ImageNative img;
            const uint32_t s = n.echelle;   // refaite au resolve si la source a une autre échelle
            if ((!t || t->img.width != c.w * s || t->img.height != c.h * s || t->formatInvite != c.format) &&
                CreerImage(c.w * s, c.h * s, c.format, VK_SAMPLE_COUNT_1_BIT, true, &img)) {
                img.echelle = s;
                Remplacer(n.textures, c.a, img);
            }
            break;
        }
        case Op::Cible: n.rt[c.a & 3] = c.b; break;
        case Op::Profondeur: n.ds = c.a; break;
        case Op::Effacer: Basculer(kPasses); FaireEffacer(c); break;
        case Op::Dessiner: FaireDessiner(*c.draw); break;
        case Op::Echange: Basculer(kEchange); FaireEchange(c); break;
        case Op::Rappel: n.rappels.emplace_back(n.enregistre ? n.soumissions + 1 : n.soumissions, std::move(c.rappel)); break;
    }
}

void Boucle() {
    if (!Initialiser()) {
        {   // plus de rendu : barrières en file exécutées, échanges libérés (le jeu ne doit jamais rester bloqué)
            std::lock_guard<std::mutex> l(g_mutex);
            g_vk.store(nullptr);
            n.vk = nullptr;
            for (Commande& c : g_file) {
                if (c.op == Op::Rappel) c.rappel();
                if (c.op == Op::Echange) { g_echangesEnAttente--; g_echangesFaits++; }
            }
            g_file.clear();
        }
        g_cvEchange.notify_all();
        RenduImpossible("ressources Vulkan du rendu natif (pool de commandes, fences) non créées");
    }
    std::deque<Commande> lot;   // commandes prises d'un coup dans la file (un verrou par paquet)
    for (;;) {
        Commande c{Op::Cible};
        bool aTraiter = !lot.empty();
        if (!aTraiter) {
            std::unique_lock<std::mutex> l(g_mutex);
            // file vide, rappels en attente : leur lot est soumis et attendu (le jeu attend peut-être la barrière)
            while (g_file.empty() && !n.rappels.empty() && !PretAPresenter()) {
                // lot encore ouvert : 1 ms laissée à la suite de l'image avant de le soumettre seul
                if (n.rappels.front().first > n.soumissions &&
                    g_cv.wait_for(l, std::chrono::milliseconds(1), [] { return !g_file.empty() || PretAPresenter(); }))
                    break;
                l.unlock();
                Retirer(true);
                l.lock();
            }
            Basculer(kAttente);
            g_cv.wait(l, [] { return !g_file.empty() || PretAPresenter(); });
            Basculer(kFile);
            aTraiter = !g_file.empty();   // sinon réveil pour la présentation seule
            lot.swap(g_file);
        }
        if (aTraiter) { c = std::move(lot.front()); lot.pop_front(); }
        Basculer(kEchange);
        Presenter(false);
        Basculer(kAutres);
        if (!aTraiter) continue;
        Traiter(c);
        Basculer(kAutres);
        if (!n.rappels.empty()) Retirer(false);
        if (c.op == Op::Echange) {
            std::lock_guard<std::mutex> l(g_mutex);
            g_echangesEnAttente--;
            g_echangesFaits++;
            g_cvEchange.notify_all();
        }
        Basculer(kFile);   // destruction de la commande comprise
    }
}

}  // namespace

void RenduImpossible(const std::string& cause) {
    const std::string m = "Le rendu du jeu (Vulkan) ne peut pas démarrer : " + cause + ".";
    fprintf(stderr, "[natif] %s\n", m.c_str());
    platform::ShowError("Islands of Wakfu", m + "\n\nDétail : journal du jeu.");
    Quit(kExitRender, "rendu natif impossible");
}
bool DestinationResolve(uint32_t tex) { return tex && g_basesResolues.count(BaseTexture(tex)); }
bool InitialiserVulkan(std::string* erreur) {
    static vk::Context ctx;
    if (!ctx.Init(erreur)) return false;
    fprintf(stderr, "[rendu] Vulkan : %s\n", ctx.deviceName.c_str());
    g_vk.store(&ctx);
    return true;
}
void Vider() {
    std::unique_lock<std::mutex> l(g_mutex);
    const uint64_t cible = g_echangesEnvoyes;
    g_cvEchange.wait_for(l, std::chrono::seconds(10), [cible] { return g_echangesFaits >= cible; });
}
void Rappel(std::function<void()> op) {
    if (!g_vk.load()) { op(); return; }   // sans Vulkan, pas de fil natif : rien en vol
    Commande c{Op::Rappel};
    c.rappel = std::move(op);
    Envoyer(std::move(c));
}
uint32_t VersionTextures() { return n.versionTextures; }
VkImageView VueResolue(uint32_t mot1) {
    ImageNative* t = Trouver(n.textures, Base(mot1));
    if (!t || !t->resolue || !t->lecture) return VK_NULL_HANDLE;
    n.stats.texturesNatives++;
    return t->lecture;
}
VkCommandBuffer CmdEnvoi() { Cmd(); return n.envois[n.slot]; }
Poste Basculer(Poste p) {
    const Poste avant = n.poste;
    n.poste = p;
    if (n.chrono) {
        const auto t = std::chrono::steady_clock::now();
        n.postes[avant] += std::chrono::duration<double, std::milli>(t - n.t).count();
        n.t = t;
    }
    return avant;
}
vk::Context* Contexte() { return n.vk; }

void CreationSurface(uint32_t surf, uint32_t w, uint32_t h, uint32_t format, uint32_t ms) {
    if (!surf) return;
    Commande c{Op::Surface};
    c.a = surf; c.b = w; c.c = h; c.d = format; c.e = ms;
    Envoyer(std::move(c));
}
// D3DUSAGE_RENDERTARGET (1) : image native créée tout de suite ; les autres textures le sont au premier resolve.
void CreationTexture(uint32_t tex, uint32_t usage) {
    if (!tex || !(usage & 1)) return;
    Commande c{Op::Texture};
    c.a = BaseTexture(tex);
    DescriptionTexture(tex, c);
    Envoyer(std::move(c));
}
void CibleCouleur(uint32_t index, uint32_t surf) {
    Commande c{Op::Cible};
    c.a = index; c.b = surf;
    Envoyer(std::move(c));
}
void CibleProfondeur(uint32_t surf) {
    Commande c{Op::Profondeur};
    c.a = surf;
    Envoyer(std::move(c));
}

// dirty[0] / dirty[1] : 1 bit = 4 vec4, bit de poids fort = registres 0-3 (comme GuestDevice d'Unleashed) ; vidés ici
// comme le faisait le vidage des draws d'origine (plus appelés), sinon chaque draw recopierait tout ce qui a changé
void SynchroConstantes(uint32_t dev) {
    for (int s = 0; s < 2; s++) {
        uint64_t m = Ld64(dev + 8 * s);
        if (m) St64(dev + 8 * s, 0);
        const uint32_t base = dev + (s ? 0x1780 : 0x780);
        while (m) {
            const int bloc = std::countl_zero(m);
            m &= ~(1ull << (63 - bloc));
            for (int i = 0; i < 16; i++) g_constantes[s * 1024 + bloc * 16 + i] = F32(base + bloc * 64 + i * 4);
            g_constantesChangees[s] = true;
        }
    }
}

void Effacer(uint32_t flags, uint32_t rect, uint32_t couleur, float z, uint32_t stencil) {
    Commande c{Op::Effacer};
    c.a = flags; c.b = stencil; c.z = z;
    if (couleur) for (int i = 0; i < 4; i++) c.couleur[i] = F32(couleur + 4 * i);
    if ((c.avecRect = rect != 0)) for (int i = 0; i < 4; i++) c.rect[i] = int32_t(Ld32(rect + 4 * i));
    Envoyer(std::move(c));
}

void Resoudre(uint32_t flags, uint32_t rect, uint32_t tex, uint32_t point, uint32_t niveau, uint32_t couleur, float z,
              uint32_t stencil) {
    if (!tex) return;
    g_basesResolues.insert(BaseTexture(tex));
    Commande c{Op::Resoudre};
    c.a = flags; c.b = BaseTexture(tex); c.c = stencil; c.d = niveau; c.z = z;
    if (point) c.e = (Ld32(point) & 0xFFFF) << 16 | (Ld32(point + 4) & 0xFFFF);
    if (couleur) for (int i = 0; i < 4; i++) c.couleur[i] = F32(couleur + 4 * i);
    if ((c.avecRect = rect != 0)) for (int i = 0; i < 4; i++) c.rect[i] = int32_t(Ld32(rect + 4 * i));
    DescriptionTexture(tex, c);
    Envoyer(std::move(c));
}

void Relever(uint32_t dev, Instantane* d) {
    SynchroConstantes(dev);
    for (int s = 0; s < 2; s++) {   // VS et PS à part : le jeu change surtout les constantes VS d'un draw à l'autre
        if (g_constantesChangees[s] || !g_instantaneConstantes[s]) {
            auto a = std::make_shared<std::array<float, 1024>>();
            memcpy(a->data(), g_constantes + 1024 * s, 4096);
            g_instantaneConstantes[s] = std::move(a);
            g_serieConstantes[s]++;
            g_constantesChangees[s] = false;
        }
        d->constantes[s] = g_instantaneConstantes[s];
        d->serieConstantes[s] = g_serieConstantes[s];
    }
    d->ps = Ld32(dev + 0x328C);
    d->vs = Ld32(dev + 0x3290);
    d->declaration = Ld32(dev + 0x2F58);
    d->indices = Ld32(dev + 0x318C);
    for (int s = 0; s < 16; s++) { d->flux[s] = Ld32(dev + 0x31A4 + 4 * s); d->pas[s] = Ld8(dev + 0x31E8 + s) * 4; }   // stride en mots de 4 octets
    for (int t = 0; t < 26; t++) d->textures[t] = Ld32(dev + 0x31F8 + 4 * t);
    for (int i = 0; i < 192; i++) d->fetch[i] = Ld32(dev + 0x480 + 4 * i);
    for (int i = 0; i < 8; i++) d->booleens[i] = Ld32(dev + 0x2780 + 4 * i);
    for (int i = 0; i < 32; i++) d->entiers[i] = Ld32(dev + 0x27A0 + 4 * i);
    d->regs = {Ld32(dev + 0x2934), Ld32(dev + 0x2938), Ld32(dev + 0x293C), Ld32(dev + 0x2948), Ld32(dev + 0x28DC),
               Ld32(dev + 0x2900), Ld32(dev + 0x28FC)};
    for (int i = 0; i < 6; i++) d->viewport[i] = F32(dev + 12896 + 4 * i);
    d->vte = Ld32(dev + 0x294C);
    for (int i = 0; i < 4; i++) d->scissor[i] = int32_t(Ld32(dev + 12924 + 4 * i));
    d->scissorActif = Ld32(dev + 12160) != 0;
    for (int i = 0; i < 4; i++) d->constanteMelange[i] = F32(dev + 0x28E0 + 4 * i);
    d->refAlpha = F32(dev + 0x2904);
}

void Dessiner(uint32_t dev, std::unique_ptr<Instantane> d, uint32_t sommetsUP, uint32_t indicesUP) {
    const auto t0 = Horloge::now();
    g_jeu.draws++;
    if (dev) Relever(dev, d.get());
    // UP : sommets [0, (min + nombre) × stride) puis indices ; tailles bornées (arguments invités)
    if (sommetsUP) {
        const uint64_t nbS = d->type == 3 ? uint64_t(d->baseSommet) + d->sommetsUP : d->sommetsUP;
        const uint64_t tS = nbS * d->pasUP, tI = indicesUP ? uint64_t(d->nombre) * (d->formatIndicesUP == 6 ? 4 : 2) : 0;
        if (tS + tI <= (16u << 20)) {
            d->donneesUP.resize(tS + tI);
            memcpy(d->donneesUP.data(), Host(sommetsUP), tS);
            if (tI) memcpy(d->donneesUP.data() + tS, Host(indicesUP), tI);
        }
    }
    Commande c{Op::Dessiner};
    c.draw = std::move(d);
    Envoyer(std::move(c));
    g_jeu.dessinerMs += Ms(t0, Horloge::now());
}

void SwapCp(uint32_t n) {
    { std::lock_guard<std::mutex> l(g_mutex); g_swapCp = n; }
    g_cv.notify_all();
}

// Une ligne par 60 images du jeu : durée réelle d'une image du fil du jeu, son CPU (calcul), le temps dans les draws
// (relevé + envoi au fil natif) et l'attente du fil natif (2 images d'avance). Durée >> CPU + attente = le fil dort
// ailleurs (attentes du jeu, minuteurs). Une fois : précision réelle d'une attente de 1 ms sur ce système.
void JournalJeu() {
    static Horloge::time_point prec{};
    static std::thread::id fil{};
    const auto t = Horloge::now();
    if (std::this_thread::get_id() != fil) {   // autre fil (écran de chargement) : compteurs par fil, on repart de zéro
        fil = std::this_thread::get_id();
        prec = {};
        g_jeu = {};
    }
    if (prec != Horloge::time_point{}) {
        const double d = Ms(prec, t);
        g_jeu.maxMs = std::max(g_jeu.maxMs, d);
        g_jeu.lentes += d > 17.5;
    } else {
        g_jeu.debut = t;
        g_jeu.cpu0 = platform::ThreadCpuMs();
        g_jeu.boucles0 = WkBouclesNs();
        g_jeu.lectures0 = WkLecturesHorloge();
        static bool mesure = false;
        if (!mesure) {
            mesure = true;
            const auto s0 = Horloge::now();
            for (int i = 0; i < 5; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            fprintf(stderr, "[minuteur] attente de 1 ms = %.2f ms en moyenne (5 essais)\n", Ms(s0, Horloge::now()) / 5);
        }
    }
    prec = t;
    if (++g_jeu.images < 60) return;
    const double cpu = platform::ThreadCpuMs(), n = g_jeu.images, duree = Ms(g_jeu.debut, t) / n;
    const uint64_t boucles = WkBouclesNs(), lectures = WkLecturesHorloge();
    const double boucleMs = (boucles - g_jeu.boucles0) / 1e6 / n;
    fprintf(stderr, "[fil jeu] par image (60 dernières) : durée %.2f ms (max %.2f, %u > 17,5 ms), CPU du fil %.2f ms "
            "dont boucle sur l'horloge %.2f ms (%.0f lectures) -> travail %.2f ms, "
            "draws %.0f (%.2f ms, %.1f µs/draw), attente du fil natif %.2f ms, reste (attentes du jeu) %.2f ms\n",
            duree, g_jeu.maxMs, g_jeu.lentes, (cpu - g_jeu.cpu0) / n, boucleMs, (lectures - g_jeu.lectures0) / n,
            (cpu - g_jeu.cpu0) / n - boucleMs, g_jeu.draws / n, g_jeu.dessinerMs / n,
            g_jeu.draws ? 1000 * g_jeu.dessinerMs / g_jeu.draws : 0.0, g_jeu.attenteMs / n,
            duree - (cpu - g_jeu.cpu0) / n - g_jeu.attenteMs / n);
    g_jeu = {};
    g_jeu.debut = t;
    g_jeu.cpu0 = cpu;
    g_jeu.boucles0 = boucles;
    g_jeu.lectures0 = lectures;
}

void Echange(uint32_t frontTex) {
    Commande c{Op::Echange};
    c.a = frontTex ? BaseTexture(frontTex) : 0;
    c.b = SwapsEmis() + 1;   // appelé juste avant VdSwap (même fil) : numéro du swap de cette image
    Envoyer(std::move(c));
    JournalJeu();
}

}  // namespace wk::natif
