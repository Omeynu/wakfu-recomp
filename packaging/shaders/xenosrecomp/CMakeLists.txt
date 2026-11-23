# XenosRecomp (sources hedge-dev, commit de tools.lock) construit avec la publication OFFICIELLE de DXC
# (microsoft/DirectXShaderCompiler) au lieu de son sous-module tiers thirdparty/dxc-bin, plus xr_hash.
# Usage : cmake -S <ce dossier> -B <build> -DXENOSRECOMP_DIR=<sources> -DDXC_DIR=<DXC extrait>
#   Linux : DXC_DIR/include/dxc, DXC_DIR/lib/libdxcompiler.so ; Windows : DXC_DIR/inc, bin/x64, lib/x64.
cmake_minimum_required(VERSION 3.20)
project(XenosRecomp-wakfu C CXX)
set(CMAKE_CXX_STANDARD 17)
set(XENOS_RECOMP_DXIL OFF CACHE BOOL "" FORCE)   # SPIR-V seulement (pas de dxil.dll)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)    # fmt, xxHash, zstd liés statiquement (xxHash le met à ON)

add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
if (WIN32)
    target_include_directories(Microsoft::DirectXShaderCompiler INTERFACE "${DXC_DIR}/inc")
    set_property(TARGET Microsoft::DirectXShaderCompiler PROPERTY IMPORTED_LOCATION "${DXC_DIR}/bin/x64/dxcompiler.dll")
    set_property(TARGET Microsoft::DirectXShaderCompiler PROPERTY IMPORTED_IMPLIB "${DXC_DIR}/lib/x64/dxcompiler.lib")
else()
    target_include_directories(Microsoft::DirectXShaderCompiler INTERFACE "${DXC_DIR}/include/dxc")
    set_property(TARGET Microsoft::DirectXShaderCompiler PROPERTY IMPORTED_LOCATION "${DXC_DIR}/lib/libdxcompiler.so")
endif()

set(XENOS_RECOMP_THIRDPARTY_ROOT "${XENOSRECOMP_DIR}/thirdparty")
add_subdirectory("${XENOSRECOMP_DIR}/thirdparty" thirdparty)
add_subdirectory("${XENOSRECOMP_DIR}/XenosRecomp" XenosRecomp)
if (WIN32 AND CMAKE_CROSSCOMPILING)   # pch.h inclut <Windows.h>, les en-têtes MinGW sont en minuscules
    file(WRITE ${CMAKE_BINARY_DIR}/casse/Windows.h "#include <windows.h>\n")
    target_include_directories(XenosRecomp BEFORE PRIVATE ${CMAKE_BINARY_DIR}/casse)
    # MinGW émule __uuidof par des gabarits (__CRT_UUID_DECL) que dxcapi.h ne fournit pas : __uuidof de Clang
    # (-fms-extensions, __declspec(uuid) de dxcapi.h) après l'en-tête.
    file(WRITE ${CMAKE_BINARY_DIR}/casse/dxcapi.h "#include_next <dxcapi.h>\n#undef __uuidof\n")
endif()

add_executable(xr_hash "${CMAKE_CURRENT_SOURCE_DIR}/../xr_hash.c")
target_include_directories(xr_hash PRIVATE "${XENOSRECOMP_DIR}/thirdparty/xxHash")
