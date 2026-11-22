# Compilation croisée Linux -> Windows x64 (Clang + en-têtes/bibliothèques MinGW-w64, lieur lld).
# Sert à vérifier la portabilité depuis Linux et à produire un .exe testable sous Wine.
# Build final sur le PC Windows : Clang (clang-cl) + CMake, voir docs/build-windows.md.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
