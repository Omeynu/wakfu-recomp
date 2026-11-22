// xexdump : applique un .xexp (optionnel), écrit le XEX patché, vide l'image chargée
// et liste sections + imports nommés. S'appuie sur XenonUtils (XenonRecomp).
// Usage : xexdump <xex> <image.bin> [<xexp> <xex_patché>]
#include <cstdio>
#include <iostream>
#include "xexcore.h"

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 5) { std::fprintf(stderr, "usage: xexdump <xex> <image.bin> [<xexp> <xex_patche>]\n"); return 2; }
    bool ok;
    auto xex = readAllBytes(argv[1], ok);
    if (!ok || xex.empty()) { std::fprintf(stderr, "lecture impossible : %s\n", argv[1]); return 1; }
    std::vector<uint8_t> patch;
    if (argc == 5)
    {
        patch = readAllBytes(argv[3], ok);
        if (!ok || patch.empty()) { std::fprintf(stderr, "lecture impossible : %s\n", argv[3]); return 1; }
    }
    std::string err;
    if (!xexPrepare(std::move(xex), patch, argc == 5 ? argv[4] : "", argv[2], std::cout, err))
    {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    return 0;
}
