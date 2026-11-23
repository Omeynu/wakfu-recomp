# Lanceur WakfuRecomp (Windows : WakfuRecomp.exe, packaging/windows/build-installer.sh ; Linux : WakfuRecomp,
# packaging/linux/build-paquet.sh). Tests : test_gen_imports (comparé à runtime/tools/gen_imports.py), test_suppression, test_migration.
# Lanceur : -Dzstd_DIR (bibliothèque statique), -DIMGUI_DIR (Dear ImGui), -DINTER_DIR (Inter-*.ttf),
#           -DXENONRECOMP_DIR (TinySHA1, toml++ et xxHash de ses sous-modules) ; Windows : -DSDL2_DIR (statique),
#           Linux : SDL2 et zlib du système, Clang (#embed).
cmake_minimum_required(VERSION 3.20)
project(wakfu_installer CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(test_gen_imports test_gen_imports.cpp gen_imports.cpp)
add_executable(test_suppression test_suppression.cpp suppression.cpp)
target_include_directories(test_suppression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../wakfu-prepare)   # xiso.h (constantes)
add_executable(test_migration test_migration.cpp suppression.cpp)
target_include_directories(test_migration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../wakfu-prepare)
set(IMGUI_DIR "" CACHE PATH "Sources de Dear ImGui")
set(INTER_DIR "" CACHE PATH "Dossier contenant Inter-Regular.ttf et Inter-SemiBold.ttf")
set(XENONRECOMP_DIR "" CACHE PATH "Racine du clone XenonRecomp")
if(NOT IMGUI_DIR)
    return()   # tests seulement
endif()
find_package(zstd CONFIG REQUIRED)
set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SOURCES main.cpp gen_imports.cpp lanceur.cpp suppression.cpp textes.cpp verif.cpp
    ../wakfu-prepare/common.cpp ../wakfu-prepare/stfs.cpp ../wakfu-prepare/xiso.cpp ${R}/runtime/src/config.cpp ${R}/runtime/src/png.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
if(WIN32)
    enable_language(RC)
    find_package(SDL2 CONFIG REQUIRED)
    add_executable(wakfu-installer ${SOURCES} wakfu-installer.rc)
    target_include_directories(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:RC>:${INTER_DIR}>)
    target_link_libraries(wakfu-installer PRIVATE zstd::libzstd_static SDL2::SDL2-static comdlg32 ole32 windowscodecs uuid)
    # Application à fenêtre (pas de console au double-clic) ; la ligne de commande ouvre ou rejoint une console.
    target_link_options(wakfu-installer PRIVATE -static -municode -mwindows)
else()
    find_package(SDL2 REQUIRED)
    find_package(ZLIB REQUIRED)   # lecture des images PNG du jeu
    add_executable(wakfu-installer ${SOURCES})
    set_target_properties(wakfu-installer PROPERTIES OUTPUT_NAME WakfuRecomp)
    target_compile_options(wakfu-installer PRIVATE -Wno-c23-extensions --embed-dir=${INTER_DIR})   # #embed des polices
    target_link_libraries(wakfu-installer PRIVATE zstd::libzstd_static SDL2::SDL2 ZLIB::ZLIB)
endif()
target_include_directories(wakfu-installer PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/../wakfu-prepare;${IMGUI_DIR};${IMGUI_DIR}/backends;${XENONRECOMP_DIR}/thirdparty/TinySHA1;${XENONRECOMP_DIR}/thirdparty/tomlplusplus/include;${XENONRECOMP_DIR}/thirdparty/xxHash>)
target_compile_definitions(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:IMGUI_DISABLE_DEMO_WINDOWS;IMGUI_DISABLE_DEBUG_TOOLS>)
# Sources de wakfu-prepare : C++17 comme dans wakfu-prepare (u8string)
set_source_files_properties(../wakfu-prepare/common.cpp ../wakfu-prepare/stfs.cpp ../wakfu-prepare/xiso.cpp PROPERTIES COMPILE_OPTIONS -std=c++17)
target_compile_options(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall>)
# En-têtes Vulkan (détection du GPU au lancement, chargeur ouvert à l'exécution) : système, ou VULKAN_INCLUDE_DIR
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Dossier contenant vulkan/vulkan.h (sinon en-têtes du système)")
if(VULKAN_INCLUDE_DIR)
    target_include_directories(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${VULKAN_INCLUDE_DIR}>)
endif()
target_link_libraries(wakfu-installer PRIVATE ${CMAKE_DL_LIBS})
