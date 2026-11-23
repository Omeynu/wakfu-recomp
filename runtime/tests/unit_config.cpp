// Test des options du joueur (config.cpp) : défauts, lecture, valeurs invalides, aller-retour du fichier écrit.
#include <cstdio>
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
    Expect("lecture (anciennes clés echelle_rendu, filtrage_anisotrope ignorées)", c.width == 1920 && c.height == 1080 && c.mode == "sans_bordure" && c.vsync && c.fpsLimit == 60 &&
                      c.gpu == "RX 6700" && c.antiAliasing == "fxaa" && c.upscale == "fsr1" && c.sharpness == 0.5f && c.gamma == 1.2f && c.volume == 0.5f && !c.vibration && c.skipLogos && c.msaa == "desactive" && err.empty());
    err.clear();
    Config bad = Parse("[video]\nlargeur = 10\nmode = \"cinema\"\nvsync = 3\nlimite_images = 5\nmulti_echantillonnage = \"oui\"\n[audio]\nvolume = 2.0\n", &err);
    Expect("invalides : défauts gardés", bad.width == 1280 && bad.mode == "fenetre" && !bad.vsync && bad.fpsLimit == 0 &&
                                          bad.volume == 1.0f && bad.msaa == "auto");
    Expect("invalides : 6 messages", err.size() == 6);
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
        Expect("msaa activé", MsaaActif("active", type));
        Expect("msaa désactivé", !MsaaActif("desactive", type));
        Expect("msaa auto", MsaaActif("auto", type) == (type == 2));
    }
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
                           r.gpu == c.gpu && r.antiAliasing == c.antiAliasing && r.upscale == c.upscale && r.sharpness == c.sharpness &&
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
    // Aller-retour complet, comme le lanceur (tools-src/wakfu-installer/lanceur.cpp : Serialize puis relecture par le
    // runtime) : toutes les clés changées, touche « \ » (Backslash) et guillemet dans le nom du GPU échappés.
    err.clear();
    Config t;
    t.width = 2560, t.height = 1440, t.mode = "plein_ecran", t.vsync = true, t.fpsLimit = 144, t.showFps = true;
    t.gpu = "RX \"6700\" \\ XT", t.antiAliasing = "fxaa", t.sharpness = 0.37f, t.brightness = 1.25f;
    t.gamma = 2.2f, t.xboxColor = true, t.volume = 0.42f, t.vibration = false, t.backgroundInput = true;
    t.skipLogos = true, t.console = true;
    t.keys = {{"a", "Space"}, {"b", "\\"}, {"stick_droit_bas", "Left Shift"}};
    Config u = Parse(Serialize(t), &err);
    Expect("aller-retour complet", err.empty() && u.width == t.width && u.height == t.height && u.mode == t.mode && u.vsync == t.vsync &&
                                   u.fpsLimit == t.fpsLimit && u.showFps == t.showFps && u.gpu == t.gpu &&
                                   u.antiAliasing == t.antiAliasing && u.sharpness == t.sharpness && u.brightness == t.brightness &&
                                   u.gamma == t.gamma && u.xboxColor == t.xboxColor && u.volume == t.volume &&
                                   u.vibration == t.vibration && u.backgroundInput == t.backgroundInput && u.skipLogos == t.skipLogos &&
                                   u.console == t.console && u.keys == t.keys);
    Expect("texte stable", Serialize(u) == Serialize(t));
    if (g_fail == 0) printf("unit_config RÉUSSI\n");
    return g_fail ? 1 : 0;
}
