#define TOML_EXCEPTIONS 0
#include "config.h"
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
    Get<bool>(t, "video", "vsync", &c.vsync, errors);
    Get<int>(t, "video", "limite_images", &c.fpsLimit, errors, [](const int& v) { return v == 0 || (v >= 10 && v <= 1000); });
    Get<bool>(t, "video", "afficher_images_par_seconde", &c.showFps, errors);
    Get<std::string>(t, "video", "gpu", &c.gpu, errors);
    if (auto b = t.at_path("video.multi_echantillonnage").value_exact<bool>()) c.msaa = *b ? "auto" : "desactive";   // ancienne forme
    else Get<std::string>(t, "video", "multi_echantillonnage", &c.msaa, errors,
                     [](const std::string& v) { return v == "auto" || v == "active" || v == "desactive"; });
    Get<std::string>(t, "post", "anticrenelage", &c.antiAliasing, errors,
                     [](const std::string& v) { return v == "aucun" || v == "fxaa"; });
    Get<std::string>(t, "post", "agrandissement", &c.upscale, errors,
                     [](const std::string& v) { return v == "lineaire" || v == "fsr1"; });
    Get<float>(t, "post", "nettete", &c.sharpness, errors, [](const float& v) { return v >= 0.0f && v <= 1.0f; });
    Get<float>(t, "post", "luminosite", &c.brightness, errors, [](const float& v) { return v >= 0.5f && v <= 2.0f; });
    Get<float>(t, "post", "gamma", &c.gamma, errors, [](const float& v) { return v >= 0.5f && v <= 3.0f; });
    Get<bool>(t, "post", "correction_couleur_xbox", &c.xboxColor, errors);
    Get<float>(t, "audio", "volume", &c.volume, errors, [](const float& v) { return v >= 0.0f && v <= 1.0f; });
    Get<bool>(t, "entrees", "vibrations", &c.vibration, errors);
    Get<bool>(t, "entrees", "entree_arriere_plan", &c.backgroundInput, errors);
    Get<bool>(t, "confort", "passer_logos", &c.skipLogos, errors);
    Get<bool>(t, "confort", "console", &c.console, errors);
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
         "vsync = " << b(c.vsync) << "        # synchronisation verticale\n"
         "limite_images = " << c.fpsLimit << "    # images par seconde au plus (0 = aucune limite)\n"
         "afficher_images_par_seconde = " << b(c.showFps) << "   # dans le titre de la fenêtre\n"
         "gpu = " << q(c.gpu) << "           # partie du nom du GPU voulu (vide = automatique : dédié, puis intégré)\n"
         "multi_echantillonnage = " << q(c.msaa) << "   # auto (GPU dédié seulement) | active | desactive : MSAA x4 demandé par le jeu\n\n"
         "[post]\n"
         "anticrenelage = " << q(c.antiAliasing) << "   # aucun | fxaa (passe finale sur l'image affichée)\n"
         "agrandissement = " << q(c.upscale) << "   # lineaire | fsr1 (AMD FSR 1 : image 1280x720 agrandie vers la fenêtre)\n"
         "nettete = " << c.sharpness << "            # netteté AMD CAS, de 0 (désactivée) à 1\n"
         "luminosite = " << c.brightness << "         # multiplicateur, de 0.5 à 2\n"
         "gamma = " << c.gamma << "              # de 0.5 à 3 (plus grand : tons moyens plus clairs)\n"
         "correction_couleur_xbox = " << b(c.xboxColor) << "   # courbe gamma PWL du Xenos -> sRGB (approximation)\n\n"
         "[audio]\n"
         "volume = " << c.volume << "         # volume général, de 0 à 1 (musique et effets : menu du jeu)\n\n"
         "[entrees]\n"
         "vibrations = " << b(c.vibration) << "\n"
         "entree_arriere_plan = " << b(c.backgroundInput) << "   # manette lue quand la fenêtre n'a pas le focus\n\n"
         "[confort]\n"
         "passer_logos = " << b(c.skipLogos) << "   # sauter les logos d'introduction\n"
         "console = " << b(c.console) << "   # Windows : console du journal (false : journal dans journal.log)\n\n"
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

bool MsaaActif(const std::string& choix, int typeGpu) { return choix == "active" || (choix == "auto" && typeGpu == 2); }

}  // namespace wk::config
