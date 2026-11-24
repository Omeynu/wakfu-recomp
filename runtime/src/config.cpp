#define TOML_EXCEPTIONS 0
#include "config.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <toml++/toml.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

namespace wk::config {

Config g;
const char* const kControls[] = {"a", "b", "x", "y", "start", "back", "lb", "rb", "lt", "rt", "haut", "bas", "gauche",
                                 "droite", "clic_gauche", "clic_droit", "stick_gauche_haut", "stick_gauche_bas",
                                 "stick_gauche_gauche", "stick_gauche_droite", "stick_droit_haut", "stick_droit_bas",
                                 "stick_droit_gauche", "stick_droit_droite", nullptr};

namespace {
const char* const kLangues[] = {"en", "ja", "de", "fr", "es", "it"};   // index + 1 = XC_LANGUAGE
template <class T> void Get(const toml::table& t, const char* section, const char* key, T* out, std::vector<std::string>* errors,
                            bool (*valid)(const T&) = nullptr) {
    const toml::node* n = t.at_path(std::string(section) + "." + key).node();
    if (!n) return;
    std::optional<T> v;
    // Types exacts (vsync = 3 refusé) ; un flottant peut s'écrire 1 ou 1.0.
    if constexpr (std::is_same_v<T, float>) { if (auto d = n->value<double>()) v = float(*d); }
    else if constexpr (std::is_same_v<T, int>) { if (auto i = n->value_exact<int64_t>(); i && *i >= INT32_MIN && *i <= INT32_MAX) v = int(*i); }
    else v = n->value_exact<T>();
    if (!v || (valid && !valid(*v))) {
        errors->push_back(std::string("[") + section + "] " + key + " : valeur invalide, défaut gardé");
        return;
    }
    *out = *v;
}
}  // namespace

Config Parse(const std::string& text, std::vector<std::string>* errors) {
    Config c;
    toml::parse_result r = toml::parse(text);
    if (!r) {
        std::ostringstream m;
        m << "config.toml illisible (ligne " << r.error().source().begin.line << ") : " << r.error().description();
        errors->push_back(m.str());
        return c;
    }
    const toml::table& t = r.table();
    Get<int>(t, "video", "largeur", &c.width, errors, [](const int& v) { return v >= 320 && v <= 16384; });
    Get<int>(t, "video", "hauteur", &c.height, errors, [](const int& v) { return v >= 240 && v <= 16384; });
    Get<std::string>(t, "video", "mode", &c.mode, errors,
                     [](const std::string& v) { return v == "fenetre" || v == "plein_ecran" || v == "sans_bordure"; });
    Get<int>(t, "video", "ecran", &c.screen, errors, [](const int& v) { return v >= 0 && v <= 63; });
    for (auto [key, out] : {std::pair{"position_x", &c.posX}, {"position_y", &c.posY}}) {
        int v = INT32_MIN;   // absente ou invalide : centrée
        Get<int>(t, "video", key, &v, errors, [](const int& p) { return p >= -16384 && p <= 16384; });
        if (v != INT32_MIN) *out = v;
    }
    Get<bool>(t, "video", "vsync", &c.vsync, errors);
    Get<int>(t, "video", "limite_images", &c.fpsLimit, errors, [](const int& v) { return v == 0 || (v >= 10 && v <= 1000); });
    Get<bool>(t, "video", "afficher_images_par_seconde", &c.showFps, errors);
    Get<std::string>(t, "video", "gpu", &c.gpu, errors);
    if (auto b = t.at_path("video.multi_echantillonnage").value_exact<bool>()) c.msaa = *b ? "auto" : "desactive";   // ancienne forme
    else Get<std::string>(t, "video", "multi_echantillonnage", &c.msaa, errors,
                     [](const std::string& v) { return v == "auto" || v == "active" || v == "desactive" || v == "2x" || v == "4x" || v == "8x"; });
    Get<int>(t, "video", "echelle_rendu", &c.renderScale, errors, [](const int& v) { return v >= 1 && v <= 4; });
    Get<int>(t, "video", "filtrage_anisotrope", &c.anisotropy, errors,
             [](const int& v) { return v == 1 || v == 2 || v == 4 || v == 8 || v == 16; });
    Get<std::string>(t, "video", "ombres", &c.ombres, errors, [](const std::string& v) { return v == "origine" || v == "x2" || v == "x4"; });
    Get<std::string>(t, "post", "anticrenelage", &c.antiAliasing, errors,
                     [](const std::string& v) { return v == "aucun" || v == "fxaa"; });
    Get<std::string>(t, "post", "agrandissement", &c.upscale, errors,
                     [](const std::string& v) { return v == "lineaire" || v == "fsr1"; });
    Get<float>(t, "post", "nettete", &c.sharpness, errors, [](const float& v) { return v >= 0.0f && v <= 1.0f; });
    Get<float>(t, "post", "luminosite", &c.brightness, errors, [](const float& v) { return v >= 0.5f && v <= 2.0f; });
    Get<float>(t, "post", "gamma", &c.gamma, errors, [](const float& v) { return v >= 0.5f && v <= 3.0f; });
    Get<bool>(t, "post", "correction_couleur_xbox", &c.xboxColor, errors);
    Get<float>(t, "audio", "volume", &c.volume, errors, [](const float& v) { return v >= 0.0f && v <= 1.0f; });
    Get<bool>(t, "audio", "couper_hors_focus", &c.muteUnfocused, errors);
    if (auto b = t.at_path("entrees.vibrations").value_exact<bool>()) c.vibration = *b ? 1.0f : 0.0f;   // ancienne forme
    else Get<float>(t, "entrees", "vibrations", &c.vibration, errors, [](const float& v) { return v >= 0.0f && v <= 1.0f; });
    Get<float>(t, "entrees", "zone_morte", &c.deadZone, errors, [](const float& v) { return v >= 0.0f && v <= 0.5f; });
    Get<bool>(t, "entrees", "entree_arriere_plan", &c.backgroundInput, errors);
    for (int j = 0; j < 2; j++)
        Get<std::string>(t, "entrees", j ? "joueur2" : "joueur1", &c.joueurs[j], errors, [](const std::string& v) {
            return v == "clavier" || (v.size() == 1 && v[0] >= '1' && v[0] <= '4');
        });
    // une seule clé donnée (ex. joueur2 = "1") : l'autre joueur prend le défaut libre au lieu d'annuler le choix
    const bool donne[2] = {bool(t.at_path("entrees.joueur1")), bool(t.at_path("entrees.joueur2"))};
    if (c.joueurs[0] == c.joueurs[1] && donne[0] != donne[1]) {
        const int libre = donne[0] ? 1 : 0;
        c.joueurs[libre] = c.joueurs[!libre] == "1" ? "2" : "1";
    }
    if (c.joueurs[0] == c.joueurs[1]) {
        errors->push_back("[entrees] joueur1 et joueur2 identiques (« " + c.joueurs[0] + " ») : défauts gardés");
        c.joueurs[0] = "1", c.joueurs[1] = "2";
    }
    Get<bool>(t, "confort", "passer_logos", &c.skipLogos, errors);
    Get<bool>(t, "confort", "console", &c.console, errors);
    Get<std::string>(t, "confort", "touche_capture", &c.captureKey, errors, [](const std::string& v) { return !v.empty(); });
    Get<std::string>(t, "jeu", "langue", &c.langue, errors,
                     [](const std::string& v) { return v == "auto" || LangueXbox(v, "") != 0; });
    if (const toml::table* k = t["touches"].as_table())
        for (auto&& [name, node] : *k) {
            const std::string n(name.str());
            bool known = false;
            for (const char* const* p = kControls; *p; p++) known |= n == *p;
            auto v = node.value_exact<std::string>();
            if (!known || !v || v->empty()) { errors->push_back("[touches] " + n + " : commande ou touche invalide"); continue; }
            c.keys[n] = *v;
        }
    return c;
}

std::string Serialize(const Config& c) {
    std::ostringstream o;
    auto b = [](bool v) { return v ? "true" : "false"; };
    auto q = [](const std::string& v) {   // chaîne TOML entre guillemets (« \ » et « " » échappés : touche Backslash, nom de GPU saisi)
        std::string r = "\"";
        for (char ch : v) { if (ch == '"' || ch == '\\') r += '\\'; r += ch; }
        return r + "\"";
    };
    o << "# Options d'Islands of Wakfu (runtime recompilé). Fichier relu à chaque lancement ; supprimez-le pour\n"
         "# retrouver les valeurs par défaut. Détail des options : docs/options.md.\n\n"
         "[video]\n"
         "largeur = " << c.width << "        # taille de la fenêtre en mode fenêtré (pixels)\n"
         "hauteur = " << c.height << "\n"
         "mode = " << q(c.mode) << "   # fenetre | plein_ecran | sans_bordure\n"
         "ecran = " << c.screen << "        # écran de la fenêtre (0 = principal)\n";
    if (c.posX && c.posY) o << "position_x = " << *c.posX << "   # position dans l'écran, mémorisée à la fermeture\n"
                               "position_y = " << *c.posY << "\n";
    else o << "# position_x, position_y : position dans l'écran, mémorisée à la fermeture (absente : centrée)\n";
    o << "vsync = " << b(c.vsync) << "        # synchronisation verticale\n"
         "limite_images = " << c.fpsLimit << "    # images par seconde au plus (0 = aucune limite)\n"
         "afficher_images_par_seconde = " << b(c.showFps) << "   # dans le titre de la fenêtre\n"
         "gpu = " << q(c.gpu) << "           # partie du nom du GPU voulu (vide = automatique : dédié, puis intégré)\n"
         "multi_echantillonnage = " << q(c.msaa) << "   # auto (x4 sur GPU dédié seulement) | active (x4) | 2x | 4x | 8x | desactive : MSAA des scènes qui le demandent (plafonné par le GPU)\n"
         "echelle_rendu = " << c.renderScale << "    # définition interne : 1 = 1280x720, 2 = 2560x1440 (1 à 4)\n"
         "filtrage_anisotrope = " << c.anisotropy << "   # textures : 1 (désactivé), 2, 4, 8 ou 16 (plafonné par le GPU)\n"
         "ombres = " << q(c.ombres) << "   # origine | x2 | x4 : ombre (zone 1) plus fine, en plus de echelle_rendu\n\n"
         "[post]\n"
         "anticrenelage = " << q(c.antiAliasing) << "   # aucun | fxaa (passe finale sur l'image affichée)\n"
         "agrandissement = " << q(c.upscale) << "   # lineaire | fsr1 (AMD FSR 1 : image 1280x720 agrandie vers la fenêtre)\n"
         "nettete = " << c.sharpness << "            # netteté AMD CAS, de 0 (désactivée) à 1\n"
         "luminosite = " << c.brightness << "         # multiplicateur, de 0.5 à 2\n"
         "gamma = " << c.gamma << "              # de 0.5 à 3 (plus grand : tons moyens plus clairs)\n"
         "correction_couleur_xbox = " << b(c.xboxColor) << "   # courbe gamma PWL du Xenos -> sRGB (approximation)\n\n"
         "[audio]\n"
         "volume = " << c.volume << "         # volume général, de 0 à 1 (musique et effets : menu du jeu)\n"
         "couper_hors_focus = " << b(c.muteUnfocused) << "   # son coupé quand la fenêtre n'a pas le focus\n\n"
         "[entrees]\n"
         "vibrations = " << c.vibration << "   # intensité des vibrations, de 0 (aucune) à 1\n"
         "zone_morte = " << c.deadZone << "   # zone morte des sticks, de 0 (aucune) à 0.5\n"
         "entree_arriere_plan = " << b(c.backgroundInput) << "   # manette lue quand la fenêtre n'a pas le focus\n"
         "joueur1 = " << q(c.joueurs[0]) << "   # commandes du joueur 1 : \"1\" à \"4\" (manette n°) ou \"clavier\"\n"
         "joueur2 = " << q(c.joueurs[1]) << "   # joueur 2 (coopératif) ; le clavier va au joueur 1 si personne n'a \"clavier\"\n\n"
         "[confort]\n"
         "passer_logos = " << b(c.skipLogos) << "   # sauter les logos d'introduction\n"
         "console = " << b(c.console) << "   # Windows : console du journal (false : journal dans journal.log)\n"
         "touche_capture = " << q(c.captureKey) << "   # capture d'écran (PNG dans captures/) ; nom de touche SDL\n\n"
         "[jeu]\n"
         "langue = " << q(c.langue) << "   # auto (langue du système) | fr | en | de | es | it | ja\n\n"
         "[touches]\n"
         "# Clavier : commande = nom de touche SDL (position physique, noms QWERTY). Une commande listée ici remplace\n"
         "# ses touches par défaut. Commandes : a b x y start back lb rb lt rt haut bas gauche droite clic_gauche\n"
         "# clic_droit stick_gauche_{haut,bas,gauche,droite} stick_droit_{haut,bas,gauche,droite}. Exemple :\n"
         "# a = \"Space\"\n";
    for (auto& [k, v] : c.keys) o << k << " = " << q(v) << "\n";
    return o.str();
}

void Load(const std::string& dataDir) {
    namespace fs = std::filesystem;
    const fs::path path = fs::path(dataDir) / "config.toml";
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = path.string() + ".tmp";
        std::ofstream(tmp, std::ios::binary) << Serialize(g);
        fs::rename(tmp, path, ec);
        fprintf(stderr, "[config] %s créé (valeurs par défaut)%s\n", path.string().c_str(), ec ? " : échec" : "");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<std::string> errors;
    g = Parse(ss.str(), &errors);
    for (auto& e : errors) fprintf(stderr, "[config] %s\n", e.c_str());
    fprintf(stderr, "[config] %s lu\n", path.string().c_str());
}

bool SaveWindow(const std::string& dataDir, int screen, int x, int y, int w, int h) {
    namespace fs = std::filesystem;
    const fs::path path = fs::path(dataDir) / "config.toml", tmp = path.string() + ".tmp";
    std::ifstream f(path, std::ios::binary);
    if (!f && fs::exists(path)) { fprintf(stderr, "[config] %s non lisible : fenêtre non mémorisée\n", path.string().c_str()); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();   // Windows : un fichier encore ouvert ne peut pas être remplacé par rename
    std::vector<std::string> errors;
    Config c = Parse(ss.str(), &errors);
    // réécrire un fichier invalide remettrait ses valeurs aux défauts et effacerait ce que le joueur a écrit
    if (!toml::parse(ss.str()) || !errors.empty()) { fprintf(stderr, "[config] %s invalide : fenêtre non mémorisée\n", path.string().c_str()); return false; }
    c.screen = std::clamp(screen, 0, 63);
    c.posX = std::clamp(x, -16384, 16384), c.posY = std::clamp(y, -16384, 16384);
    c.width = std::clamp(w, 320, 16384), c.height = std::clamp(h, 240, 16384);
    std::error_code ec;
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o << Serialize(c);
        if (!o.flush()) ec = std::make_error_code(std::errc::io_error);
    }
    if (!ec) fs::rename(tmp, path, ec);
    fprintf(stderr, "[config] fenêtre mémorisée : écran %d, position %d,%d, taille %dx%d%s\n", c.screen, *c.posX, *c.posY,
            c.width, c.height, ec ? " : échec de l'écriture" : "");
    return !ec;
}

int ChoisirGpu(const std::vector<GpuVu>& gpus, const std::string& option, bool* optionTrouvee) {
    auto bas = [](std::string s) { for (char& ch : s) ch = char(tolower(uint8_t(ch))); return s; };
    int best = -1, bestScore = -1;
    for (size_t i = 0; i < gpus.size(); i++) {
        if (!gpus[i].vulkan13) continue;
        const int score = gpus[i].type == 2 ? 3 : gpus[i].type == 1 ? 2 : 1;
        if (score > bestScore) { best = int(i); bestScore = score; }
    }
    bool trouvee = option.empty();
    for (size_t i = 0; i < gpus.size() && !trouvee; i++)
        if (gpus[i].vulkan13 && bas(gpus[i].nom).find(bas(option)) != std::string::npos) { best = int(i); trouvee = true; }
    if (optionTrouvee) *optionTrouvee = trouvee;
    return best;
}

int LangueXbox(const std::string& choix, const std::string& systeme) {
    const std::string code = choix == "auto" ? systeme.substr(0, 2) : choix;
    for (int i = 0; i < 6; i++) if (code == kLangues[i]) return i + 1;
    return choix == "auto" ? 1 : 0;   // 0 : choix invalide
}

std::string LangueSysteme() {
#ifdef _WIN32
    wchar_t w[LOCALE_NAME_MAX_LENGTH] = {};
    std::string r;
    if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH)) for (const wchar_t* p = w; *p; p++) r += char(*p < 128 ? *p : '?');
    return r;
#else
    for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* l = std::getenv(v); l && *l) return l;
    return "";
#endif
}

// Valeurs mesurées (docs/options.md, « Préréglages ») : économie = GPU intégré (Vega 8), qualité = 60 img/s p99
// sur RX 6700 en grande zone.
namespace {
struct Image { const char* nom; int echelle; const char* msaa; int aniso; const char* ombres; };
const Image kImages[] = {{"auto", 1, "auto", 1, "origine"}, {"economie", 1, "desactive", 1, "origine"},
                         {"equilibre", 1, "4x", 8, "x2"}, {"qualite", 2, "4x", 16, "x2"}};
}  // namespace
bool AppliquerPrereglage(const std::string& nom, Config* c) {
    for (const Image& i : kImages)
        if (nom == i.nom) {
            c->renderScale = i.echelle, c->msaa = i.msaa, c->anisotropy = i.aniso, c->ombres = i.ombres;
            return true;
        }
    return false;
}
std::string Prereglage(const Config& c) {
    for (const Image& i : kImages)
        if (c.renderScale == i.echelle && c.msaa == i.msaa && c.anisotropy == i.aniso && c.ombres == i.ombres) return i.nom;
    return "personnalise";
}

int MsaaEchantillons(const std::string& choix, int typeGpu) {
    if (choix == "2x" || choix == "8x") return choix[0] - '0';
    return choix == "active" || choix == "4x" || (choix == "auto" && typeGpu == 2) ? 4 : 0;
}

}  // namespace wk::config
