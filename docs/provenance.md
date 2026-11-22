# Provenance des dépendances

Vérifiée le 2026-11-17 : dépôt officiel public (propriétaire attendu, pas un fork ni un compte
homonyme), adoption (étoiles GitHub), activité (dernier envoi), version publiée depuis plus de deux semaines,
intégrité (somme ou signature). Métadonnées lues par l'API publique de GitHub.

## Paquet Windows (`packaging/windows/deps.lock`)

| Dépendance | Source | Propriétaire | Étoiles | Version (publication, auteur) | Intégrité |
|---|---|---|---|---|---|
| llvm-mingw | github.com/mstorsjo/llvm-mingw | Martin Storsjö (auteur) | 3 030 | 20250114 (2025-01-14, CI du projet) | somme relevée au 1er téléchargement (pas de condensé publié) |
| CMake | github.com/Kitware/CMake | Kitware | 8 164 | 3.31.6 (2025-02-24) | fichier SHA-256 publié par Kitware |
| ninja | github.com/ninja-build/ninja | ninja-build | 13 364 | 1.12.1 (2024-05-11, jhasse, mainteneur) | somme relevée au 1er téléchargement |
| SDL2 | github.com/libsdl-org/SDL | libsdl-org | 17 007 | 2.32.10 (2025-09-01, slouken, auteur) | condensé GitHub identique |
| zstd | github.com/meta/zstd | Meta (ancien `facebook/zstd`, même dépôt id 29759715, renommage de l'organisation) | 28 256 | 1.5.7 (2025-02-19, Cyan4973, auteur) | fichier SHA-256 amont |
| FFmpeg | ffmpeg.org (miroir github.com/FFmpeg/FFmpeg) | FFmpeg | 67 525 | 7.1.2 | signature GPG valide, clé de publication FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8 ; somme identique |
| glslang | github.com/KhronosGroup/glslang | Khronos Group | 3 618 | 15.1.0 (archive de tag) | somme relevée au 1er téléchargement |
| Vulkan-Headers | github.com/KhronosGroup/Vulkan-Headers | Khronos Group | 1 107 | 1.4.309 (archive de tag) | somme relevée au 1er téléchargement |
| Dear ImGui | github.com/ocornut/imgui | Omar Cornut (auteur) | 77 148 | 1.93.0 (2026-10-14, ocornut ; tag sur le commit c1aaac65) | somme de l'archive du tag relevée au 1er téléchargement |
| Inter (police, OFL 1.1) | github.com/rsms/inter | Rasmus Andersson (auteur) | 20 087 | 4.1 (2024-11-16, rsms) | somme relevée au 1er téléchargement (pas de condensé publié pour cet envoi) |

Fenêtre du lanceur (`tools-src/wakfu-installer/lanceur.cpp`) : Dear ImGui (sources compilées dans l'amorce, avec les
modules `imgui_impl_sdl2` et `imgui_impl_sdlrenderer2` du même dépôt) sur la SDL2 déjà construite pour l'archive,
rendu 2D de SDL (sans Vulkan). Police : `Inter-Regular.ttf` et `Inter-SemiBold.ttf` de l'archive officielle,
embarquées comme ressources de l'exécutable. Lecture des illustrations du joueur (`KeyArt.png`, `Background.png`)
par WIC (Windows Imaging Component, composant de Windows, décodeur PNG présent depuis Windows XP) : aucune
bibliothèque ajoutée, donc ni stb_image (dont les derniers envois datent de moins de deux semaines) ni SDL2_image
(absent du projet). Ces images ne sont jamais distribuées : elles sont lues dans la copie du jeu préparée chez le joueur.

## Outils (`tools.lock`, commits épinglés)

| Outil | Source | Propriétaire | Étoiles | Remarque |
|---|---|---|---|---|
| XenonRecomp | github.com/hedge-dev/XenonRecomp | hedge-dev | 6 725 | patché localement (`patches/xenonrecomp-wakfu.patch`) |
| XenosRecomp | github.com/hedge-dev/XenosRecomp | hedge-dev | 513 | |
| UnleashedRecomp | github.com/hedge-dev/UnleashedRecomp | hedge-dev | 5 387 | référence seulement, rien n'est copié |
| Xenia | github.com/xenia-project/xenia | xenia-project | 9 836 | fichiers `src/xenia/gpu` (BSD) dans `runtime/third_party/xenia` |

## Sources intégrées au dépôt (`runtime/third_party/`)

| Composant | Source | Propriétaire | Étoiles | Version (publication) | Intégrité |
|---|---|---|---|---|---|
| AMD FidelityFX FSR 1 (`ffx_a.h`, `ffx_fsr1.h`, licence MIT) | github.com/GPUOpen-Effects/FidelityFX-FSR | GPUOpen-Effects (AMD), non archivé | 2 190 | v1.0.2 (2021-11-16) | archive du tag SHA-256 `92ee8f96…04acd` ; fichiers copiés tels quels : `ffx_a.h` `f60e2722…f3fb`, `ffx_fsr1.h` `93c39223…a5`, `LICENSE.txt` `db089274…e802` |

## Sous-modules de XenonRecomp (`thirdparty/`)

| Bibliothèque | Source | Propriétaire | Étoiles | Utilisée par le runtime |
|---|---|---|---|---|
| fmt | github.com/fmtlib/fmt | fmtlib | 25 928 | oui (fichiers Xenia) |
| simde | github.com/simd-everywhere/simde-no-tests | simd-everywhere (miroir officiel sans tests de `simde`, peu d'étoiles mais même organisation) | 13 | oui |
| toml++ | github.com/marzer/tomlplusplus | Mark Gillard (auteur) | 2 187 | oui (`config.toml`) |
| xxHash | github.com/Cyan4973/xxHash | Yann Collet (auteur) | 11 444 | non |
| libmspack | github.com/kyz/libmspack | Stuart Caie (auteur) | 221 | non (XenonRecomp) |
| tiny-AES-c | github.com/kokke/tiny-AES-c | kokke (auteur) | 5 028 | non (XenonRecomp) |

Aucune source douteuse. Limites : pour llvm-mingw, ninja, glslang et Vulkan-Headers, la somme n'est vérifiable que
contre celle relevée au premier téléchargement (le fichier n'a pas changé depuis), pas contre une valeur publiée
par l'amont ; l'historique des commits n'a pas été relu commit par commit.
