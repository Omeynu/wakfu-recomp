# Vérification de compilation du C++ généré par XenonRecomp (pas un build jouable).
# cmake -S check -B <build> -G Ninja -DPPC_DIR=<out/ppc> -DSIMDE_DIR=<XenonRecomp/thirdparty/simde>
cmake_minimum_required(VERSION 3.20)
project(wakfu_ppc_check CXX)

set(PPC_DIR "" CACHE PATH "Dossier du C++ généré (out/ppc)")
set(SIMDE_DIR "" CACHE PATH "XenonRecomp/thirdparty/simde")
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
    message(FATAL_ERROR "Clang >= 18 requis (trouvé ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
endif()
set(CMAKE_CXX_STANDARD 20)

file(GLOB PPC_SRC CONFIGURE_DEPENDS "${PPC_DIR}/*.cpp")
add_library(wakfu_ppc STATIC ${PPC_SRC})
target_include_directories(wakfu_ppc PRIVATE "${PPC_DIR}" "${SIMDE_DIR}")
# Mêmes options que UnleashedRecompLib : aliasing et flottants stricts, cible x86-64 avec AVX.
target_compile_options(wakfu_ppc PRIVATE -O2 -fno-strict-aliasing -ffp-model=strict -march=sandybridge)
target_precompile_headers(wakfu_ppc PRIVATE "${PPC_DIR}/ppc_recomp_shared.h")
