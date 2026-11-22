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
                             d.volume == 1.0f && d.vibration && !d.skipLogos && err.empty());
    err.clear();
    Config c = Parse("[video]\nlargeur = 1920\nhauteur = 1080\nmode = \"sans_bordure\"\nvsync = true\nlimite_images = 60\n"
                     "gpu = \"RX 6700\"\nechelle_rendu = 2\n[post]\nanticrenelage = \"fxaa\"\nagrandissement = \"fsr1\"\nnettete = 0.5\ngamma = 1.2\nfiltrage_anisotrope = 8\n[audio]\nvolume = 0.5\n[entrees]\nvibrations = false\n[confort]\npasser_logos = true\n", &err);
    Expect("lecture", c.width == 1920 && c.height == 1080 && c.mode == "sans_bordure" && c.vsync && c.fpsLimit == 60 &&
                      c.gpu == "RX 6700" && c.renderScale == 2 && c.antiAliasing == "fxaa" && c.upscale == "fsr1" && c.sharpness == 0.5f && c.gamma == 1.2f && c.anisotropy == 8 && c.volume == 0.5f && !c.vibration && c.skipLogos && err.empty());
    err.clear();
    Config bad = Parse("[video]\nlargeur = 10\nmode = \"cinema\"\nvsync = 3\nlimite_images = 5\n[audio]\nvolume = 2.0\n[post]\nfiltrage_anisotrope = 3\n", &err);
    Expect("invalides : défauts gardés", bad.width == 1280 && bad.mode == "fenetre" && !bad.vsync && bad.fpsLimit == 0 &&
                                          bad.volume == 1.0f);
    Expect("invalides : 6 messages", err.size() == 6 && bad.anisotropy == 1);
    err.clear();
    Parse("[video\nlargeur = ", &err);
    Expect("fichier illisible : message", err.size() == 1);
    err.clear();
    Config r = Parse(Serialize(c), &err);
    Expect("aller-retour", r.width == c.width && r.mode == c.mode && r.vsync == c.vsync && r.fpsLimit == c.fpsLimit &&
                           r.gpu == c.gpu && r.renderScale == c.renderScale && r.antiAliasing == c.antiAliasing && r.upscale == c.upscale && r.sharpness == c.sharpness &&
                           r.gamma == c.gamma && r.anisotropy == c.anisotropy && r.volume == c.volume && r.vibration == c.vibration && r.skipLogos == c.skipLogos &&
                           err.empty());
    err.clear();
    Config k = Parse("[touches]\na = \"Space\"\nstick_gauche_haut = \"W\"\nvoler = \"F\"\nb = 3\n[confort]\nconsole = false\n", &err);
    Expect("touches lues", k.keys.size() == 2 && k.keys["a"] == "Space" && k.keys["stick_gauche_haut"] == "W" && !k.console);
    Expect("touches invalides : 2 messages", err.size() == 2);
    err.clear();
    Config kr = Parse(Serialize(k), &err);
    Expect("touches aller-retour", kr.keys == k.keys && !kr.console && err.empty());
    // Aller-retour complet, comme le lanceur (tools-src/wakfu-installer/lanceur.cpp : Serialize puis relecture par le
    // runtime) : toutes les clés changées, touche « \ » (Backslash) et guillemet dans le nom du GPU échappés.
    err.clear();
    Config t;
    t.width = 2560, t.height = 1440, t.mode = "plein_ecran", t.vsync = true, t.fpsLimit = 144, t.showFps = true;
    t.gpu = "RX \"6700\" \\ XT", t.renderScale = 3, t.antiAliasing = "fxaa", t.sharpness = 0.37f, t.brightness = 1.25f;
    t.gamma = 2.2f, t.xboxColor = true, t.anisotropy = 16, t.volume = 0.42f, t.vibration = false, t.backgroundInput = true;
    t.skipLogos = true, t.console = false;
    t.keys = {{"a", "Space"}, {"b", "\\"}, {"stick_droit_bas", "Left Shift"}};
    Config u = Parse(Serialize(t), &err);
    Expect("aller-retour complet", err.empty() && u.width == t.width && u.height == t.height && u.mode == t.mode && u.vsync == t.vsync &&
                                   u.fpsLimit == t.fpsLimit && u.showFps == t.showFps && u.gpu == t.gpu && u.renderScale == t.renderScale &&
                                   u.antiAliasing == t.antiAliasing && u.sharpness == t.sharpness && u.brightness == t.brightness &&
                                   u.gamma == t.gamma && u.xboxColor == t.xboxColor && u.anisotropy == t.anisotropy && u.volume == t.volume &&
                                   u.vibration == t.vibration && u.backgroundInput == t.backgroundInput && u.skipLogos == t.skipLogos &&
                                   u.console == t.console && u.keys == t.keys);
    Expect("texte stable", Serialize(u) == Serialize(t));
    if (g_fail == 0) printf("unit_config RÉUSSI\n");
    return g_fail ? 1 : 0;
}
