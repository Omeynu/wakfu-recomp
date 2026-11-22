// Génère imports_stub.cpp, imports_list.inc et kernel_vars.inc (équivalent de runtime/tools/gen_imports.py).
#pragma once
#include <filesystem>
#include <string>

// Lève std::runtime_error en cas d'erreur ; renvoie le résumé « N fonctions, M variables ».
std::string genImports(const std::filesystem::path& xex, const std::filesystem::path& dump,
                       const std::filesystem::path& tables, const std::filesystem::path& out);
