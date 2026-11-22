# Compilation croisée Linux -> Windows x64 avec llvm-mingw (même version que la chaîne embarquée dans le paquet :
# mêmes en-têtes, libc++ et compiler-rt, donc bibliothèques précompilées liables chez l'utilisateur).
# -DLLVM_MINGW=<racine de llvm-mingw pour Linux>
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW)
    set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW}/bin/llvm-ranlib)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
