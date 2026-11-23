/* Empreinte d'un conteneur de shader Xenos (.xpu/.xvu) : XXH3-64 sur virtualSize + physicalSize
 * octets, comme XenosRecomp (mode dossier) et UnleashedRecomp. Sortie : "<hex16> <vs|ps>".
 * xr_hash -x <image.bin> <dossier> : extrait les conteneurs intégrés au XEX (<adresse>.xvu|.xpu), même
 * recherche que l'ancien runtime/tools/shader_tool.cpp, retiré en N7 (pas de 4, champs 0x1C/0x20 nuls, microcode dans le
 * conteneur).
 * Construction : cc -O2 -I<xxHash> xr_hash.c -o xr_hash */
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned be32(const unsigned char* p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int extraire(const char* image, const char* dossier) {
    FILE* f = fopen(image, "rb");
    if (!f) { perror(image); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "%s : lecture impossible\n", image); return 1; }
    fclose(f);
    int k = 0;
    for (size_t o = 0; o + 0x24 <= (size_t)n; o += 4) {
        unsigned fl = be32(b + o);
        if ((fl & 0xFFFFFF00u) != 0x102A1100u || be32(b + o + 0x1C) || be32(b + o + 0x20)) continue;
        size_t virt = be32(b + o + 4), phys = be32(b + o + 8), sh = be32(b + o + 0x18);
        if (o + sh + 8 > (size_t)n) continue;
        size_t poff = be32(b + o + sh), size = be32(b + o + sh + 4);
        if (!size || size % 4 || o + virt + phys > (size_t)n || poff + size > phys) continue;
        char nom[4096];
        snprintf(nom, sizeof nom, "%s/%08zX.%s", dossier, (size_t)0x82000000 + o, (fl & 1) ? "xvu" : "xpu");
        FILE* g = fopen(nom, "wb");
        int ok = g && fwrite(b + o, 1, virt + phys, g) == virt + phys;
        if (!g || fclose(g) != 0 || !ok) { fprintf(stderr, "%s : écriture impossible\n", nom); return 1; }
        k++;
        o += virt + phys - 4;
    }
    free(b);
    printf("%d conteneurs extraits\n", k);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && argv[1][0] == '-' && argv[1][1] == 'x' && !argv[1][2]) return extraire(argv[2], argv[3]);
    if (argc != 2) { fprintf(stderr, "usage : xr_hash <conteneur> | xr_hash -x <image.bin> <dossier>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 36 || (be32(buf) & 0xFFFFFF00u) != 0x102A1100u) { fprintf(stderr, "%s : pas un conteneur Xenos\n", argv[1]); return 1; }
    size_t size = (size_t)be32(buf + 4) + be32(buf + 8);
    if (size > n) { fprintf(stderr, "%s : conteneur tronqué (%zu > %zu)\n", argv[1], size, n); return 1; }
    printf("%016llx %s\n", (unsigned long long)XXH3_64bits(buf, size), (be32(buf) & 1) ? "vs" : "ps");
    return 0;
}
