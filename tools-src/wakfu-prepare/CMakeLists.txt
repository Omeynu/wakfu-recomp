# wakfu-prepare : extraction du jeu (STFS / ISO-XISO / dossier) et application de la TU.
# cmake -S tools-src/wakfu-prepare -B <build> -G Ninja -DXENONRECOMP_DIR=<clone XenonRecomp>
#       [-DWAKFU_GAME_DIR=<dossier avec raw/ (paquets) et base/, tu2/, patched/ de référence>]
cmake_minimum_required(VERSION 3.20)
project(wakfu_prepare CXX C)
set(XENONRECOMP_DIR "" CACHE PATH "Racine du clone XenonRecomp")
set(WAKFU_GAME_DIR "" CACHE PATH "Jeu de test réel (facultatif) : raw/ + références base/, tu2/, patched/")
if(NOT EXISTS "${XENONRECOMP_DIR}/XenonUtils/xex_patcher.h")
    message(FATAL_ERROR "XENONRECOMP_DIR invalide : ${XENONRECOMP_DIR}")
endif()
add_subdirectory(${XENONRECOMP_DIR} xenonrecomp EXCLUDE_FROM_ALL)
if(WIN32 AND CMAKE_HOST_UNIX)
    # Compilation croisée : XenonUtils inclut <Windows.h>, les en-têtes MinGW sont en minuscules.
    file(WRITE ${CMAKE_BINARY_DIR}/casse/Windows.h "#include <windows.h>\n")
    target_include_directories(XenonUtils BEFORE PUBLIC ${CMAKE_BINARY_DIR}/casse)
endif()

add_library(prepare_core STATIC common.cpp stfs.cpp xiso.cpp ../xexdump/xexcore.cpp)
target_include_directories(prepare_core PUBLIC . ../xexdump "${XENONRECOMP_DIR}/thirdparty/TinySHA1")
target_link_libraries(prepare_core PUBLIC XenonUtils)
target_compile_options(prepare_core PRIVATE -Wall)

add_executable(wakfu-prepare main.cpp)
add_executable(test_extract tests/test_extract.cpp)
foreach(t wakfu-prepare test_extract)
    target_link_libraries(${t} PRIVATE prepare_core)
    if(WIN32)
        target_link_options(${t} PRIVATE -static -fuse-ld=lld)   # binaire autonome (aucune DLL MinGW)
    endif()
endforeach()
if(WIN32)
    target_link_options(wakfu-prepare PRIVATE -municode)   # point d'entrée wmain : chemins Unicode
endif()

enable_testing()
add_test(NAME xiso_synthetique COMMAND test_extract unit ${CMAKE_BINARY_DIR}/t-unit)
if(WAKFU_GAME_DIR)
    set(G ${WAKFU_GAME_DIR})
    set(RUN ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:wakfu-prepare> -DGEN=$<TARGET_FILE:test_extract> -DREF=${G})
    set(CASE ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_case.cmake)
    add_test(NAME stfs_tu_corruption COMMAND test_extract stfs ${G}/raw ${CMAKE_BINARY_DIR}/t-stfs)
    # Paquets STFS réels (dossier scanné) -> base, tu2, patched identiques aux références
    add_test(NAME reel_stfs COMMAND ${RUN} -DOUT=${CMAKE_BINARY_DIR}/t-reel-stfs "-DINPUTS=${G}/raw" -P ${CASE})
    # Image XGD2 générée depuis base/ de référence + TU en dossier extrait
    add_test(NAME reel_xiso_xgd2 COMMAND ${RUN} -DOUT=${CMAKE_BINARY_DIR}/t-reel-xiso -DISO=${CMAKE_BINARY_DIR}/t-reel.iso
             -DISO_OFFSET=0xFD90000 "-DINPUTS=${CMAKE_BINARY_DIR}/t-reel.iso;${G}/tu2" -P ${CASE})
    set_tests_properties(reel_stfs reel_xiso_xgd2 PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
endif()
