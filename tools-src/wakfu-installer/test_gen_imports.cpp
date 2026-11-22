// test_gen_imports <xex patché> <patched.txt> <XenonUtils/xbox> <sortie> : comparé au script Python par build-installer.sh
#include "gen_imports.h"
#include <cstdio>
#include <exception>

int main(int argc, char** argv)
{
    if (argc != 5) { std::fprintf(stderr, "usage : test_gen_imports <xex> <patched.txt> <tables> <sortie>\n"); return 2; }
    try { std::printf("%s\n", genImports(argv[1], argv[2], argv[3], argv[4]).c_str()); }
    catch (const std::exception& e) { std::fprintf(stderr, "erreur : %s\n", e.what()); return 1; }
    return 0;
}
