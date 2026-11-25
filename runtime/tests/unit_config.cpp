// Test des options du joueur (config.cpp) : défauts, lecture, valeurs invalides, aller-retour du fichier écrit.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../src/config.h"
static int g_fail = 0;
static void Expect(const char* what, bool ok) { if (!ok) { printf("ÉCHEC %s\n", what); g_fail++; } }
int main() {
    using namespace wk::config;
    std::vector<std::string> err;
    Config d = Parse("", &err);
    Expect("vide : défauts", d.width == 1280 && d.height == 720 && d.mode == "fenetre" && !d.vsync && d.fpsLimit == 0 &&
                             d.volume == 1.0f && d.vibration && !d.skipLogos && d.msaa == "auto" && err.empty());
    err.clear();
    Config c = Parse("[video]\nlargeur = 1920\nhauteur = 1080\nmode = \"sans_bordure\"\nvsync = true\nlimite_images = 60\n"
                     "gpu = \"RX 6700\"\nechelle_rendu = 2\nmulti_echantillonnage = \"desactive\"\n[post]\nanticrenelage = \"fxaa\"\nagrandissement = \"fsr1\"\nnettete = 0.5\ngamma = 1.2\nfiltrage_anisotrope = 8\n[audio]\nvolume = 0.5\n[entrees]\nvibrations = false\n[confort]\npasser_logos = true\n", &err);
    Expect("lecture (ancienne clé [post] filtrage_anisotrope ignorée)", c.anisotropy == 1 && c.width == 1920 && c.height == 1080 && c.mode == "sans_bordure" && c.vsync && c.fpsLimit == 60 &&
                      c.gpu == "RX 6700" && c.renderScale == 2 && c.antiAliasing == "fxaa" && c.upscale == "fsr1" && c.sharpness == 0.5f && c.gamma == 1.2f && c.volume == 0.5f && !c.vibration && c.skipLogos && c.msaa == "desactive" && err.empty());
    err.clear();
    Config bad = Parse("[video]\nlargeur = 10\nmode = \"cinema\"\nvsync = 3\nlimite_images = 5\nmulti_echantillonnage = \"oui\"\nechelle_rendu = 5\n[audio]\nvolume = 2.0\n", &err);
    Expect("invalides : défauts gardés", bad.width == 1280 && bad.mode == "fenetre" && !bad.vsync && bad.fpsLimit == 0 &&
                                          bad.volume == 1.0f && bad.msaa == "auto" && bad.renderScale == 1);
    Expect("invalides : 7 messages", err.size() == 7);
    err.clear();
    Expect("multi_echantillonnage booléen (ancienne forme)", Parse("[video]\nmulti_echantillonnage = false\n", &err).msaa == "desactive" &&
                                                            Parse("[video]\nmulti_echantillonnage = true\n", &err).msaa == "auto" && err.empty());
    // Langue : défaut auto, valeurs invalides refusées, auto suit le système (anglais si le jeu n'a pas la langue)
    Expect("langue défaut", d.langue == "auto");
    Expect("langue lue", Parse("[jeu]\nlangue = \"de\"\n", &err).langue == "de" && err.empty());
    Expect("langue invalide", Parse("[jeu]\nlangue = \"pt\"\n", &err).langue == "auto" && err.size() == 1);
    err.clear();
    Expect("langue choisie prioritaire", LangueXbox("ja", "fr_FR.UTF-8") == 2 && LangueXbox("fr", "") == 4);
    Expect("langue auto", LangueXbox("auto", "fr-FR") == 4 && LangueXbox("auto", "it_IT") == 6 && LangueXbox("auto", "C.UTF-8") == 1 &&
                          LangueXbox("auto", "pt-BR") == 1 && LangueXbox("auto", "") == 1);
    // Multi-échantillonnage : choix du joueur prioritaire ; auto = GPU dédié (2) seulement (intégré 1, processeur 4)
    for (int type : {1, 2, 4}) {
        Expect("msaa activé", MsaaEchantillons("active", type) == 4);
        Expect("msaa désactivé", MsaaEchantillons("desactive", type) == 0);
        Expect("msaa auto", MsaaEchantillons("auto", type) == (type == 2 ? 4 : 0));
        Expect("msaa niveaux", MsaaEchantillons("2x", type) == 2 && MsaaEchantillons("4x", type) == 4 && MsaaEchantillons("8x", type) == 8);
    }
    {
        std::vector<std::string> e;
        Expect("msaa active lu comme 4x", Parse("[video]\nmulti_echantillonnage = \"active\"\n", &e).msaa == "4x" && e.empty());
    }
    Expect("msaa niveau lu", Parse("[video]\nmulti_echantillonnage = \"8x\"\n", &err).msaa == "8x" && err.empty());
    for (const char* v : {"\"16x\"", "\"x4\"", "4"})
        Expect("msaa niveau invalide", Parse(std::string("[video]\nmulti_echantillonnage = ") + v + "\n", &err).msaa == "auto");
    Expect("msaa niveau invalide : 3 messages", err.size() == 3);
    err.clear();
    // Choix du GPU : dédié > intégré > processeur, Vulkan 1.3 exigé, option (casse ignorée) prioritaire
    const std::vector<GpuVu> g = {{"llvmpipe", 4, true}, {"AMD Radeon Graphics (RADV RENOIR)", 1, true},
                                  {"Vieille carte", 2, false}, {"AMD Radeon RX 6700 (RADV NAVI22)", 2, true}};
    bool trouvee = false;
    Expect("gpu auto : dédié", ChoisirGpu(g, "", &trouvee) == 3 && trouvee);
    Expect("gpu auto : intégré sans dédié", ChoisirGpu({g[0], g[1], g[2]}, "") == 1);
    Expect("gpu auto : processeur seul", ChoisirGpu({g[0]}, "") == 0);
    Expect("gpu option", ChoisirGpu(g, "renoir", &trouvee) == 1 && trouvee);
    Expect("gpu option introuvable", ChoisirGpu(g, "vieille", &trouvee) == 3 && !trouvee);
    Expect("gpu aucun", ChoisirGpu({g[2]}, "") == -1);
    err.clear();
    Parse("[video\nlargeur = ", &err);
    Expect("fichier illisible : message", err.size() == 1);
    err.clear();
    Config r = Parse(Serialize(c), &err);
    Expect("aller-retour", r.width == c.width && r.mode == c.mode && r.vsync == c.vsync && r.fpsLimit == c.fpsLimit &&
                           r.gpu == c.gpu && r.renderScale == c.renderScale && r.antiAliasing == c.antiAliasing && r.upscale == c.upscale && r.sharpness == c.sharpness &&
                           r.gamma == c.gamma && r.volume == c.volume && r.vibration == c.vibration && r.skipLogos == c.skipLogos &&
                           r.msaa == c.msaa && err.empty());
    Expect("aller-retour langue", Parse(Serialize(Parse("[jeu]\nlangue = \"es\"\n", &err)), &err).langue == "es" && err.empty());
    err.clear();
    Config k = Parse("[touches]\na = \"Space\"\nstick_gauche_haut = \"W\"\nvoler = \"F\"\nb = 3\n[confort]\nconsole = true\n", &err);
    Expect("touches lues", k.keys.size() == 2 && k.keys["a"] == "Space" && k.keys["stick_gauche_haut"] == "W" && k.console);
    Expect("console désactivée par défaut", !Config{}.console);
    Expect("touches invalides : 2 messages", err.size() == 2);
    err.clear();
    Config kr = Parse(Serialize(k), &err);
    Expect("touches aller-retour", kr.keys == k.keys && kr.console && err.empty());
    // [manette] : bouton du jeu inconnu, bouton physique inconnu ou non-chaîne refusés ; doublons acceptés ; aller-retour
    err.clear();
    Config m = Parse("[manette]\na = \"b\"\nb = \"b\"\nlt = \"aucun\"\nrs = \"paddle1\"\nvoler = \"a\"\nx = \"croix\"\ny = 2\n", &err);
    Expect("manette lue", m.pad.size() == 4 && m.pad["a"] == "b" && m.pad["b"] == "b" && m.pad["lt"] == "aucun" && m.pad["rs"] == "paddle1");
    Expect("manette invalide : 3 messages", err.size() == 3);
    err.clear();
    Expect("manette aller-retour", Parse(Serialize(m), &err).pad == m.pad && err.empty());
    Expect("manette \"\" = aucun", Parse("[manette]\nstart = \"\"\n", &err).pad["start"] == "aucun" && err.empty());
    Expect("manette vide par défaut", Config{}.pad.empty() && Parse(Serialize(Config{}), &err).pad.empty() && err.empty());
    // [entrees] icones : auto par défaut, valeur inconnue refusée, aller-retour
    err.clear();
    Expect("icones auto par défaut", Config{}.icones == "auto");
    Expect("icones playstation", Parse("[entrees]\nicones = \"playstation\"\n", &err).icones == "playstation" && err.empty());
    Expect("icones inconnue refusée", Parse("[entrees]\nicones = \"sony\"\n", &err).icones == "auto" && err.size() == 1);
    err.clear();
    Expect("icones aller-retour", Parse(Serialize(Parse("[entrees]\nicones = \"xbox\"\n", &err)), &err).icones == "xbox" && err.empty());
    // Aller-retour complet, comme le lanceur (tools-src/wakfu-installer/lanceur.cpp : Serialize puis relecture par le
    // runtime) : toutes les clés changées, touche « \ » (Backslash) et guillemet dans le nom du GPU échappés.
    err.clear();
    Config t;
    t.width = 2560, t.height = 1440, t.mode = "plein_ecran", t.vsync = true, t.fpsLimit = 144, t.showFps = true;
    t.gpu = "RX \"6700\" \\ XT", t.renderScale = 3, t.antiAliasing = "fxaa", t.sharpness = 0.37f, t.brightness = 1.25f;
    t.gamma = 2.2f, t.xboxColor = true, t.volume = 0.42f, t.vibration = false, t.backgroundInput = true;
    t.skipLogos = true, t.console = true;
    t.keys = {{"a", "Space"}, {"b", "\\"}, {"stick_droit_bas", "Left Shift"}};
    Config u = Parse(Serialize(t), &err);
    Expect("aller-retour complet", err.empty() && u.width == t.width && u.height == t.height && u.mode == t.mode && u.vsync == t.vsync &&
                                   u.fpsLimit == t.fpsLimit && u.showFps == t.showFps && u.gpu == t.gpu && u.renderScale == t.renderScale &&
                                   u.antiAliasing == t.antiAliasing && u.sharpness == t.sharpness && u.brightness == t.brightness &&
                                   u.gamma == t.gamma && u.xboxColor == t.xboxColor && u.volume == t.volume &&
                                   u.vibration == t.vibration && u.backgroundInput == t.backgroundInput && u.skipLogos == t.skipLogos &&
                                   u.console == t.console && u.keys == t.keys);
    Expect("texte stable", Serialize(u) == Serialize(t));
    // Filtrage anisotrope : [video], puissances de 2 de 1 à 16 seulement
    err.clear();
    Expect("anisotrope lu", Parse("[video]\nfiltrage_anisotrope = 16\n", &err).anisotropy == 16 && err.empty());
    for (const char* v : {"0", "3", "32", "true", "\"8\""})
        Expect("anisotrope invalide", Parse(std::string("[video]\nfiltrage_anisotrope = ") + v + "\n", &err).anisotropy == 1);
    Expect("anisotrope invalide : 5 messages", err.size() == 5);
    err.clear();
    t.anisotropy = 8;
    Expect("anisotrope aller-retour", Parse(Serialize(t), &err).anisotropy == 8 && err.empty());
    // Ombres : origine (défaut) | x2 | x4
    err.clear();
    Expect("ombres défaut", d.ombres == "origine");
    Expect("ombres lu", Parse("[video]\nombres = \"x4\"\n", &err).ombres == "x4" && err.empty());
    for (const char* v : {"\"x3\"", "2", "\"\""})
        Expect("ombres invalide", Parse(std::string("[video]\nombres = ") + v + "\n", &err).ombres == "origine");
    Expect("ombres invalide : 3 messages", err.size() == 3);
    err.clear();
    t.ombres = "x2";
    Expect("ombres aller-retour", Parse(Serialize(t), &err).ombres == "x2" && err.empty());
    // Préréglages image du lanceur : défauts = auto ; chacun relu sous son nom ; un réglage changé = personnalisé
    Expect("préréglage défaut", Prereglage(Config{}) == "auto");
    for (const char* p : {"economie", "equilibre", "qualite", "auto"}) {
        Config x;
        x.volume = 0.3f;   // hors préréglage : gardé
        Expect("préréglage appliqué", AppliquerPrereglage(p, &x) && Prereglage(x) == p && x.volume == 0.3f);
    }
    Config pq;
    AppliquerPrereglage("qualite", &pq);
    Expect("préréglage qualité", pq.renderScale == 2 && pq.msaa == "4x" && pq.anisotropy == 16 && pq.ombres == "x2");
    pq.anisotropy = 4;
    Expect("préréglage personnalisé", Prereglage(pq) == "personnalise");
    Expect("préréglage inconnu", !AppliquerPrereglage("ultra", &pq) && pq.anisotropy == 4);
    // Son coupé hors focus : booléen, défaut false
    err.clear();
    Expect("couper_hors_focus défaut", !d.muteUnfocused);
    Expect("couper_hors_focus lu", Parse("[audio]\ncouper_hors_focus = true\n", &err).muteUnfocused && err.empty());
    Expect("couper_hors_focus invalide", !Parse("[audio]\ncouper_hors_focus = 1\n", &err).muteUnfocused && err.size() == 1);
    err.clear();
    t.muteUnfocused = true;
    Expect("couper_hors_focus aller-retour", Parse(Serialize(t), &err).muteUnfocused && err.empty());
    // Zone morte : 0 à 0,5, défaut 0
    err.clear();
    Expect("zone_morte défaut", d.deadZone == 0.0f);
    Expect("zone_morte lue", Parse("[entrees]\nzone_morte = 0.25\n", &err).deadZone == 0.25f && err.empty());
    Expect("zone_morte hors bornes", Parse("[entrees]\nzone_morte = 0.6\n", &err).deadZone == 0.0f &&
                                     Parse("[entrees]\nzone_morte = -0.1\n", &err).deadZone == 0.0f && err.size() == 2);
    err.clear();
    t.deadZone = 0.15f;
    Expect("zone_morte aller-retour", Parse(Serialize(t), &err).deadZone == 0.15f && err.empty());
    // Joueurs 1 et 2 : "1" à "4" ou "clavier", différents ; défauts "1" et "2"
    err.clear();
    Expect("joueurs défaut", d.joueurs[0] == "1" && d.joueurs[1] == "2");
    {
        const Config j = Parse("[entrees]\njoueur1 = \"clavier\"\njoueur2 = \"1\"\n", &err);
        Expect("joueurs clavier + manette", j.joueurs[0] == "clavier" && j.joueurs[1] == "1" && err.empty());
    }
    Expect("joueur invalide", Parse("[entrees]\njoueur2 = \"5\"\n", &err).joueurs[1] == "2" &&
                              Parse("[entrees]\njoueur1 = 1\n", &err).joueurs[0] == "1" && err.size() == 2);
    err.clear();
    {
        const Config j = Parse("[entrees]\njoueur1 = \"2\"\n", &err);   // seul joueur1 donné : joueur2 prend "1"
        Expect("joueur1 seul échangé", j.joueurs[0] == "2" && j.joueurs[1] == "1" && err.empty());
        const Config k = Parse("[entrees]\njoueur2 = \"1\"\n", &err);
        Expect("joueur2 seul échangé", k.joueurs[0] == "2" && k.joueurs[1] == "1" && err.empty());
        const Config l = Parse("[entrees]\njoueur1 = \"3\"\njoueur2 = \"3\"\n", &err);
        Expect("joueurs identiques", l.joueurs[0] == "1" && l.joueurs[1] == "2" && err.size() == 1);
    }
    err.clear();
    t.joueurs[0] = "3", t.joueurs[1] = "clavier";
    {
        const Config j = Parse(Serialize(t), &err);
        Expect("joueurs aller-retour", j.joueurs[0] == "3" && j.joueurs[1] == "clavier" && err.empty());
    }
    // Vibrations : intensité 0 à 1, true / false acceptés (1 / 0)
    err.clear();
    Expect("vibrations défaut", d.vibration == 1.0f);
    Expect("vibrations true/false", Parse("[entrees]\nvibrations = true\n", &err).vibration == 1.0f &&
                                    Parse("[entrees]\nvibrations = false\n", &err).vibration == 0.0f && err.empty());
    Expect("vibrations intensité", Parse("[entrees]\nvibrations = 0.4\n", &err).vibration == 0.4f && err.empty());
    Expect("vibrations hors bornes", Parse("[entrees]\nvibrations = 1.5\n", &err).vibration == 1.0f &&
                                     Parse("[entrees]\nvibrations = \"fort\"\n", &err).vibration == 1.0f && err.size() == 2);
    err.clear();
    t.vibration = 0.7f;
    Expect("vibrations aller-retour", Parse(Serialize(t), &err).vibration == 0.7f && err.empty());
    // Touche de capture : nom de touche non vide, défaut F12
    err.clear();
    Expect("touche_capture défaut", d.captureKey == "F12");
    Expect("touche_capture lue", Parse("[confort]\ntouche_capture = \"Print Screen\"\n", &err).captureKey == "Print Screen" && err.empty());
    Expect("touche_capture invalide", Parse("[confort]\ntouche_capture = \"\"\n", &err).captureKey == "F12" &&
                                      Parse("[confort]\ntouche_capture = 12\n", &err).captureKey == "F12" && err.size() == 2);
    err.clear();
    t.captureKey = "F9";
    Expect("touche_capture aller-retour", Parse(Serialize(t), &err).captureKey == "F9" && err.empty());
    // Affichage des performances : booléen, défaut false
    err.clear();
    Expect("afficher_perfs défaut", !d.afficherPerfs);
    Expect("afficher_perfs lu", Parse("[confort]\nafficher_perfs = true\n", &err).afficherPerfs && err.empty());
    Expect("afficher_perfs invalide", !Parse("[confort]\nafficher_perfs = \"oui\"\n", &err).afficherPerfs && err.size() == 1 &&
                                      err[0] == "[confort] afficher_perfs : valeur invalide, défaut gardé");
    err.clear();
    t.afficherPerfs = true;
    Expect("afficher_perfs aller-retour", Parse(Serialize(t), &err).afficherPerfs && err.empty());
    // Notifications de succès : booléen, défaut true
    err.clear();
    Expect("notifications_succes défaut", d.notificationsSucces);
    Expect("notifications_succes lu", !Parse("[confort]\nnotifications_succes = false\n", &err).notificationsSucces && err.empty());
    Expect("notifications_succes invalide", Parse("[confort]\nnotifications_succes = 0\n", &err).notificationsSucces && err.size() == 1 &&
                                           err[0] == "[confort] notifications_succes : valeur invalide, défaut gardé");
    err.clear();
    t.notificationsSucces = false;
    Expect("notifications_succes aller-retour", !Parse(Serialize(t), &err).notificationsSucces && err.empty());
    // Anticrénelage de la transparence : booléen, défaut false
    err.clear();
    Expect("anticrenelage_transparence défaut", !d.anticrenelageTransparence);
    Expect("anticrenelage_transparence lu", Parse("[video]\nanticrenelage_transparence = true\n", &err).anticrenelageTransparence && err.empty());
    Expect("anticrenelage_transparence invalide", !Parse("[video]\nanticrenelage_transparence = \"4x\"\n", &err).anticrenelageTransparence &&
                                                  err.size() == 1 && err[0] == "[video] anticrenelage_transparence : valeur invalide, défaut gardé");
    err.clear();
    t.anticrenelageTransparence = true;
    Expect("anticrenelage_transparence aller-retour", Parse(Serialize(t), &err).anticrenelageTransparence && err.empty());
    // Filtre pour daltoniens : aucun (défaut) | protanopie | deuteranopie | tritanopie
    err.clear();
    Expect("daltonisme défaut", d.daltonisme == "aucun");
    for (const char* v : {"protanopie", "deuteranopie", "tritanopie", "aucun"})
        Expect("daltonisme lu", Parse(std::string("[post]\ndaltonisme = \"") + v + "\"\n", &err).daltonisme == v && err.empty());
    for (const char* v : {"\"Protanopie\"", "\"deuteranomalie\"", "1", "\"\""})
        Expect("daltonisme invalide", Parse(std::string("[post]\ndaltonisme = ") + v + "\n", &err).daltonisme == "aucun");
    Expect("daltonisme invalide : 4 messages", err.size() == 4 && err[0] == "[post] daltonisme : valeur invalide, défaut gardé");
    err.clear();
    t.daltonisme = "tritanopie";
    Expect("daltonisme aller-retour", Parse(Serialize(t), &err).daltonisme == "tritanopie" && err.empty());
    // Écran et position de la fenêtre : défaut écran 0, centrée (position absente)
    err.clear();
    Expect("écran défaut", d.screen == 0 && !d.posX && !d.posY);
    Config pos = Parse("[video]\necran = 1\nposition_x = 100\nposition_y = -20\n", &err);
    Expect("écran et position lus", pos.screen == 1 && pos.posX == 100 && pos.posY == -20 && err.empty());
    Config posBad = Parse("[video]\necran = -1\nposition_x = 99999\nposition_y = \"haut\"\n", &err);
    Expect("écran et position invalides", posBad.screen == 0 && !posBad.posX && !posBad.posY && err.size() == 3);
    err.clear();
    Expect("position aller-retour", Parse(Serialize(pos), &err).posY == -20 && Parse(Serialize(pos), &err).posX == 100 && err.empty());
    Expect("position absente : pas écrite", !Parse(Serialize(d), &err).posX && err.empty());
    // SaveWindow : fichier relu, seules la fenêtre et l'écran changent ; fichier illisible laissé tel quel
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "wakfu_unit_config_fenetre";
        fs::remove_all(dir);
        fs::create_directories(dir);
        auto lire = [&] { std::ifstream f(dir / "config.toml", std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); };
        std::ofstream(dir / "config.toml") << Serialize(t);
        Expect("SaveWindow écrit", SaveWindow(dir.string(), 1, 40, 50, 1600, 900));
        err.clear();
        Config w = Parse(lire(), &err);
        Config attendu = t;
        attendu.screen = 1, attendu.posX = 40, attendu.posY = 50, attendu.width = 1600, attendu.height = 900;
        Expect("SaveWindow : fenêtre changée, reste gardé", err.empty() && Serialize(w) == Serialize(attendu));
        // Wayland : position inconnue, celle du fichier gardée
        Expect("SaveWindow sans position", SaveWindow(dir.string(), 0, std::nullopt, std::nullopt, 1280, 720));
        w = Parse(lire(), &err);
        Expect("SaveWindow sans position : 40,50 gardé", err.empty() && w.posX == 40 && w.posY == 50 && w.width == 1280 && w.screen == 0);
        std::ofstream(dir / "config.toml") << "[video\nlargeur = ";
        Expect("SaveWindow : illisible refusé", !SaveWindow(dir.string(), 0, 1, 1, 800, 600) && lire() == "[video\nlargeur = ");
        std::ofstream(dir / "config.toml") << "[video]\nvsync = 3\n";
        Expect("SaveWindow : valeur invalide refusée", !SaveWindow(dir.string(), 0, 1, 1, 800, 600) && lire() == "[video]\nvsync = 3\n");
        fs::remove_all(dir);
    }
    if (g_fail == 0) printf("unit_config RÉUSSI\n");
    return g_fail ? 1 : 0;
}
