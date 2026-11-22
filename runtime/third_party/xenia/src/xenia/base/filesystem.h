// Cale wakfu-recomp (remplace xenia/base/filesystem.h) : seul OpenFile est utilisé.
#pragma once
#include <cstdio>
#include <filesystem>
namespace xe::filesystem {
inline FILE* OpenFile(const std::filesystem::path& p, const char* mode) { return std::fopen(p.string().c_str(), mode); }
}
