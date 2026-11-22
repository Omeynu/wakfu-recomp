# Runtime d'Islands of Wakfu (jalon 1 : Linux, sans affichage).
# cmake -S runtime -B <build> -G Ninja -DPPC_DIR=<out/ppc> -DSIMDE_DIR=<simde> -DGEN_DIR=<sorties de gen_imports.py>
cmake_minimum_required(VERSION 3.20)
project(wakfu_runtime CXX)
set(CMAKE_CXX_STANDARD 20)

set(GEN_DIR "" CACHE PATH "Dossier des fichiers générés par runtime/tools/gen_imports.py")
# Bibliothèque du C++ généré (mêmes options que la vérification)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../check ${CMAKE_BINARY_DIR}/ppc)

# Options du joueur : toml++ de XenonRecomp (en-têtes seulement), comme fmt et simde
set(TOMLPP_INCLUDE "${SIMDE_DIR}/../tomlplusplus/include")
set(XXHASH_INCLUDE "${SIMDE_DIR}/../xxHash")   # empreintes des textures (XXH3)

add_executable(wakfu
    src/main.cpp src/config.cpp src/game_log.cpp src/guest.cpp src/process.cpp src/platform.cpp
    src/kernel/stub.cpp src/kernel/variables.cpp src/kernel/core.cpp
    src/kernel/sync.cpp src/kernel/threads.cpp src/kernel/gpu.cpp src/kernel/files.cpp src/kernel/xam.cpp src/kernel/profile.cpp src/kernel/xma.cpp src/kernel/audio_out.cpp src/kernel/strings.cpp src/kernel/format.cpp src/kernel/gpu_cp.cpp src/kernel/frontbuffer.cpp src/kernel/window.cpp src/png.cpp src/gpu/texture_decode.cpp
    ${GEN_DIR}/imports_stub.cpp)
target_include_directories(wakfu PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}" "${GEN_DIR}" "${TOMLPP_INCLUDE}" "${XXHASH_INCLUDE}")
target_compile_options(wakfu PRIVATE -O2 -g -fno-strict-aliasing -ffp-model=strict -march=sandybridge -Wall -Wno-unused-function)
if(WIN32)
    # binaire autonome (exécutable sous Wine pour les tests) ; lieur lld
    target_link_options(wakfu PRIVATE -static -fuse-ld=lld)
else()
    # -rdynamic : les noms sub_<adresse invitée> apparaissent dans les piles de plantage
    target_link_options(wakfu PRIVATE -rdynamic)
endif()
if(WIN32)
    # COFF : les définitions faibles (alias sub_* -> __imp__sub_*) d'une archive ne sont pas indexées ;
    # on lie donc la bibliothèque entière.
    add_dependencies(wakfu wakfu_ppc)
    target_link_options(wakfu PRIVATE -Wl,--whole-archive $<TARGET_FILE:wakfu_ppc> -Wl,--no-whole-archive)
else()
    target_link_libraries(wakfu PRIVATE wakfu_ppc)
endif()
# Appels indirects du C++ généré vérifiés par le runtime (journal au lieu d'un plantage opaque)
target_compile_options(wakfu_ppc PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/indirect.h)

# Test unitaire du processeur de commandes PM4
add_executable(wakfu_unit_gpu_cp tests/unit_gpu_cp.cpp src/guest.cpp src/platform.cpp src/kernel/gpu_cp.cpp)
target_include_directories(wakfu_unit_gpu_cp PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}")
target_compile_options(wakfu_unit_gpu_cp PRIVATE -O1 -g -march=sandybridge)
if(WIN32)
    target_link_options(wakfu_unit_gpu_cp PRIVATE -static -fuse-ld=lld)
endif()

# Socle Vulkan (chargement dynamique ; en-têtes Vulkan requis)
# En-têtes Vulkan seuls, recopiés à part (éviter d'ajouter /usr/include entier en compilation croisée)
find_path(VULKAN_SYS_INCLUDE vulkan/vulkan.h PATHS /usr/include NO_CMAKE_FIND_ROOT_PATH)
set(VULKAN_HEADERS_DIR ${CMAKE_BINARY_DIR}/vulkan-headers)
file(COPY ${VULKAN_SYS_INCLUDE}/vulkan ${VULKAN_SYS_INCLUDE}/vk_video DESTINATION ${VULKAN_HEADERS_DIR})
add_executable(wakfu_unit_vk tests/unit_vk.cpp src/gpu/vk.cpp src/config.cpp)
target_include_directories(wakfu_unit_vk PRIVATE src ${VULKAN_HEADERS_DIR} "${TOMLPP_INCLUDE}")
if(WIN32)
    target_link_options(wakfu_unit_vk PRIVATE -static -fuse-ld=lld)
else()
    target_link_libraries(wakfu_unit_vk PRIVATE ${CMAKE_DL_LIBS})
endif()

# Analyse du microcode Xenos (fichiers repris de Xenia, BSD) ; fmt en en-têtes seuls (copie de XenonRecomp)
set(XENIA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenia)
set(XENIA_FMT_ROOT ${CMAKE_BINARY_DIR}/xenia-fmt)
file(COPY ${SIMDE_DIR}/../fmt/include DESTINATION ${XENIA_FMT_ROOT}/third_party/fmt)
add_library(xenia_ucode STATIC
    ${XENIA_DIR}/src/xenia/gpu/ucode.cc ${XENIA_DIR}/src/xenia/gpu/xenos.cc ${XENIA_DIR}/src/xenia/gpu/shader.cc
    ${XENIA_DIR}/src/xenia/gpu/shader_translator.cc ${XENIA_DIR}/src/xenia/gpu/shader_translator_disasm.cc
    ${XENIA_DIR}/src/xenia/gpu/registers.cc ${XENIA_DIR}/src/xenia/base/string_buffer.cc ${XENIA_DIR}/shims.cpp)
target_include_directories(xenia_ucode PUBLIC ${XENIA_DIR}/src ${XENIA_FMT_ROOT} ${XENIA_FMT_ROOT}/third_party/fmt/include)
target_compile_definitions(xenia_ucode PUBLIC FMT_HEADER_ONLY NDEBUG)   # assertions de débogage de Xenia désactivées, comme ses builds de production
target_compile_options(xenia_ucode PRIVATE -O2 -w)

# Traducteur Xenos -> GLSL -> SPIR-V (glslang). Sous Windows : glslang de MSYS2, ou en compilation croisée
# glslang construit pour MinGW et désigné par -Dglslang_DIR=<préfixe>/lib/cmake/glslang (docs/build-windows.md).
find_package(glslang CONFIG)
if(NOT glslang_FOUND)
    message(STATUS "glslang introuvable : runtime compilé sans rendu (WAKFU_HAS_SHADERGEN absent)")
endif()
if(glslang_FOUND)
    add_library(wakfu_shadergen STATIC src/gpu/glsl_translator.cpp)
    target_include_directories(wakfu_shadergen PUBLIC src)
    target_link_libraries(wakfu_shadergen PUBLIC xenia_ucode glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
    target_sources(wakfu_shadergen PRIVATE src/gpu/shader_cache.cpp src/gpu/renderer.cpp src/gpu/vk.cpp src/gpu/present.cpp src/gpu/fsr1.cpp)
    # AMD FidelityFX FSR 1.0.2 (MIT, docs/provenance.md) : en-têtes GLSL intégrés au programme en chaînes brutes
    set(FSR1_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FSR1_DIR}/ffx_a.h ${FSR1_DIR}/ffx_fsr1.h)
    file(READ ${FSR1_DIR}/ffx_a.h FSR1_A)
    file(READ ${FSR1_DIR}/ffx_fsr1.h FSR1_FSR)
    file(WRITE ${CMAKE_BINARY_DIR}/fsr1-gen/fsr1_glsl.h
         "// Produit par CMake depuis third_party/fidelityfx-fsr1 (ne pas modifier)\n#pragma once\n"
         "static const char* const kFfxAGlsl = R\"FFXSRC(${FSR1_A})FFXSRC\";\n"
         "static const char* const kFfxFsr1Glsl = R\"FFXSRC(${FSR1_FSR})FFXSRC\";\n")
    target_include_directories(wakfu_shadergen PRIVATE ${CMAKE_BINARY_DIR}/fsr1-gen ${FSR1_DIR})
    target_include_directories(wakfu_shadergen PUBLIC ${VULKAN_HEADERS_DIR} "${PPC_DIR}" "${SIMDE_DIR}" "${XXHASH_INCLUDE}")
    target_compile_options(wakfu_shadergen PRIVATE -march=sandybridge)
    target_link_libraries(wakfu_shadergen PUBLIC ${CMAKE_DL_LIBS})
    add_executable(wakfu_shaders tools/shader_tool.cpp)
    target_link_libraries(wakfu_shaders PRIVATE wakfu_shadergen)
    target_link_libraries(wakfu PRIVATE wakfu_shadergen)
    target_compile_definitions(wakfu PRIVATE WAKFU_HAS_SHADERGEN)
    # Décodage des textures : calcul GPU comparé au décodage CPU (pilote choisi par VK_ICD_FILENAMES)
    add_executable(wakfu_unit_texdecode tests/unit_texdecode.cpp src/gpu/texture_decode.cpp src/config.cpp)
    target_include_directories(wakfu_unit_texdecode PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}" "${XXHASH_INCLUDE}" "${TOMLPP_INCLUDE}")
    target_compile_options(wakfu_unit_texdecode PRIVATE -O1 -g -march=sandybridge)
    target_link_libraries(wakfu_unit_texdecode PRIVATE wakfu_shadergen)
    if(WIN32)
        target_link_options(wakfu_unit_texdecode PRIVATE -static -fuse-ld=lld)
    endif()
endif()

# Sortie son, fenêtre et manette par SDL2 si disponible (sinon pilote audio muet, cadencé par minuteur,
# ni fenêtre ni manette)
find_package(SDL2 CONFIG)
if(WIN32 AND TARGET SDL2::SDL2-static)
    set(WAKFU_SDL2 SDL2::SDL2-static)
elseif(TARGET SDL2::SDL2)
    set(WAKFU_SDL2 SDL2::SDL2)
endif()
if(WAKFU_SDL2)
    target_link_libraries(wakfu PRIVATE ${WAKFU_SDL2})
    target_compile_definitions(wakfu PRIVATE WAKFU_HAS_SDL2)
    target_sources(wakfu PRIVATE src/kernel/pad_map.cpp)
    # Test de la correspondance manette SDL / clavier -> XINPUT
    add_executable(wakfu_unit_pad tests/unit_pad.cpp src/kernel/pad_map.cpp)
    target_include_directories(wakfu_unit_pad PRIVATE src)
    target_compile_options(wakfu_unit_pad PRIVATE -O1 -g)
    target_link_libraries(wakfu_unit_pad PRIVATE ${WAKFU_SDL2})
    if(WIN32)
        target_link_options(wakfu_unit_pad PRIVATE -static -fuse-ld=lld)
    endif()
endif()

# Test de la réduction 5.1 -> stéréo des trames audio
add_executable(wakfu_unit_audio tests/unit_audio.cpp src/kernel/audio_out.cpp)
target_include_directories(wakfu_unit_audio PRIVATE src)
target_compile_options(wakfu_unit_audio PRIVATE -O1 -g)
if(WIN32)
    target_link_options(wakfu_unit_audio PRIVATE -static -fuse-ld=lld)
endif()

# Test du formateur printf invité
add_executable(wakfu_unit_config tests/unit_config.cpp src/config.cpp)
target_include_directories(wakfu_unit_config PRIVATE src "${TOMLPP_INCLUDE}")
target_compile_options(wakfu_unit_config PRIVATE -O1 -g)
add_executable(wakfu_unit_watch tests/unit_watch.cpp src/platform.cpp)
target_include_directories(wakfu_unit_watch PRIVATE src)
target_compile_options(wakfu_unit_watch PRIVATE -O1 -g)
if(WIN32)
    target_link_options(wakfu_unit_watch PRIVATE -static -fuse-ld=lld)
endif()
add_executable(wakfu_unit_time tests/unit_time.cpp)
target_compile_options(wakfu_unit_time PRIVATE -O1 -g)
add_executable(wakfu_unit_strings tests/unit_strings.cpp src/guest.cpp src/platform.cpp src/kernel/format.cpp)
target_include_directories(wakfu_unit_strings PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}")
target_compile_options(wakfu_unit_strings PRIVATE -O1 -g -march=sandybridge)
if(WIN32)
    target_link_options(wakfu_unit_strings PRIVATE -static -fuse-ld=lld)
endif()

# Test des réglages de profil sur disque : wakfu_unit_profile <dossier temporaire>
add_executable(wakfu_unit_profile tests/unit_profile.cpp src/guest.cpp src/platform.cpp src/kernel/profile.cpp)
target_include_directories(wakfu_unit_profile PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}")
target_compile_options(wakfu_unit_profile PRIVATE -O1 -g -march=sandybridge)
if(WIN32)
    target_link_options(wakfu_unit_profile PRIVATE -static -fuse-ld=lld)
endif()

# Décodage XMA par FFmpeg (libavcodec, décodeur xma2, LGPL) s'il est trouvé par pkg-config ; sinon les tampons
# d'entrée XMA sont consommés sans son. Préfixe non standard : PKG_CONFIG_PATH=<préfixe>/lib/pkgconfig.
# Sans pkg-config (installateur Windows) : -DFFMPEG_PREFIX=<préfixe d'une libavcodec/libavutil statique>.
set(FFMPEG_PREFIX "" CACHE PATH "Préfixe FFmpeg statique (remplace pkg-config)")
if(FFMPEG_PREFIX)
    add_library(wakfu_ffmpeg INTERFACE)
    target_include_directories(wakfu_ffmpeg INTERFACE ${FFMPEG_PREFIX}/include)
    target_link_libraries(wakfu_ffmpeg INTERFACE ${FFMPEG_PREFIX}/lib/libavcodec.a ${FFMPEG_PREFIX}/lib/libavutil.a
                          $<$<BOOL:${WIN32}>:bcrypt ole32 user32>)
    set(FFMPEG_FOUND TRUE)
    set(WAKFU_FFMPEG wakfu_ffmpeg)
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    set(WAKFU_FFMPEG PkgConfig::FFMPEG)
endif()
if(FFMPEG_FOUND)
    message(STATUS "FFmpeg ${FFMPEG_libavcodec_VERSION} : décodage XMA activé")
    target_link_libraries(wakfu PRIVATE ${WAKFU_FFMPEG})
    target_compile_definitions(wakfu PRIVATE WAKFU_HAS_FFMPEG)
    # Test du décodage XMA par l'interface des contextes : wakfu_unit_xma <WaveBank.xwb> [index]
    add_executable(wakfu_unit_xma tests/unit_xma.cpp src/guest.cpp src/platform.cpp src/kernel/xma.cpp)
    target_include_directories(wakfu_unit_xma PRIVATE src "${PPC_DIR}" "${SIMDE_DIR}")
    target_compile_options(wakfu_unit_xma PRIVATE -O2 -g -march=sandybridge -Wall)
    target_compile_definitions(wakfu_unit_xma PRIVATE WAKFU_HAS_FFMPEG)
    target_link_libraries(wakfu_unit_xma PRIVATE ${WAKFU_FFMPEG})
    if(WIN32)
        target_link_options(wakfu_unit_xma PRIVATE -static -fuse-ld=lld)
    endif()
else()
    message(STATUS "FFmpeg (libavcodec) introuvable : runtime compilé sans décodage XMA")
endif()
