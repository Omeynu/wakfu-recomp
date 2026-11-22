# Amorce de l'installateur Windows (WakfuRecomp.exe) ; construite par packaging/windows/build-installer.sh.
# Sous Linux : seulement test_gen_imports (comparaison avec runtime/tools/gen_imports.py).
# Windows : -Dzstd_DIR -DSDL2_DIR (bibliothèques statiques), -DIMGUI_DIR (Dear ImGui), -DINTER_DIR (Inter-*.ttf),
#           -DXENONRECOMP_DIR (TinySHA1 et toml++ de ses sous-modules).
cmake_minimum_required(VERSION 3.20)
project(wakfu_installer CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(test_gen_imports test_gen_imports.cpp gen_imports.cpp)
if(WIN32)
    enable_language(RC)
    find_package(zstd CONFIG REQUIRED)
    find_package(SDL2 CONFIG REQUIRED)
    set(IMGUI_DIR "" CACHE PATH "Sources de Dear ImGui")
    set(INTER_DIR "" CACHE PATH "Dossier contenant Inter-Regular.ttf et Inter-SemiBold.ttf")
    set(XENONRECOMP_DIR "" CACHE PATH "Racine du clone XenonRecomp")
    set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    add_executable(wakfu-installer main.cpp gen_imports.cpp lanceur.cpp textes.cpp verif.cpp wakfu-installer.rc
        ../wakfu-prepare/common.cpp ../wakfu-prepare/stfs.cpp ../wakfu-prepare/xiso.cpp ${R}/runtime/src/config.cpp ${R}/runtime/src/png.cpp
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
    target_include_directories(wakfu-installer PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/../wakfu-prepare;${IMGUI_DIR};${IMGUI_DIR}/backends;${XENONRECOMP_DIR}/thirdparty/TinySHA1;${XENONRECOMP_DIR}/thirdparty/tomlplusplus/include>
        $<$<COMPILE_LANGUAGE:RC>:${INTER_DIR}>)
    target_compile_definitions(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:IMGUI_DISABLE_DEMO_WINDOWS;IMGUI_DISABLE_DEBUG_TOOLS>)
    # Sources de wakfu-prepare : C++17 comme dans wakfu-prepare (u8string)
    set_source_files_properties(../wakfu-prepare/common.cpp ../wakfu-prepare/stfs.cpp ../wakfu-prepare/xiso.cpp PROPERTIES COMPILE_OPTIONS -std=c++17)
    target_link_libraries(wakfu-installer PRIVATE zstd::libzstd_static SDL2::SDL2-static comdlg32 ole32 windowscodecs uuid)
    target_compile_options(wakfu-installer PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall>)
    # Application à fenêtre (pas de console au double-clic) ; la ligne de commande ouvre ou rejoint une console.
    target_link_options(wakfu-installer PRIVATE -static -municode -mwindows)
endif()
