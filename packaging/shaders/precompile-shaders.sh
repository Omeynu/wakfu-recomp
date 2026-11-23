#!/bin/bash
# Précompilation des shaders du jeu en SPIR-V (moteur natif B3), à lancer à l'installation.
# Utilitaire autonome (le lanceur WakfuRecomp fait la même boucle en C++, natifShaders) ; le runtime lit <hash>.spv dans WAKFU_NATIF_SPIRV.
#
# Usage : precompile-shaders.sh <dossier de conteneurs .xpu/.xvu> <dossier de sortie>
# Outils (variables d'environnement) : XENOSRECOMP (binaire), XENOS_INCLUDE (shader_common.h),
#   DXC (dxc officiel), DXC_LIB (dossier de libdxcompiler.so), XR_HASH (binaire de xr_hash.c).
# Sortie : <hash>.spv par shader traduit + table.tsv (hash, vs|ps, fichier, conteneur source)
#   + echecs.tsv (conteneur, étape, code). Code de sortie 0 même avec des échecs : le moteur
#   doit retomber sur le traducteur maison pour les hash absents de la table.
set -u
[ $# -eq 2 ] || { echo "usage : $0 <conteneurs> <sortie>" >&2; exit 2; }
src=$1; out=$2
for v in XENOSRECOMP XENOS_INCLUDE DXC DXC_LIB XR_HASH; do
    [ -e "${!v:-}" ] || { echo "$v absent ou introuvable : '${!v:-}'" >&2; exit 2; }
done
[ -d "$src" ] || { echo "dossier source introuvable : $src" >&2; exit 2; }
mkdir -p "$out" || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
# Table existante reprise : relancer sur <sortie>/conteneurs (déposés par le jeu) ne la remplace pas.
cp "$out/table.tsv" "$tmp/table.tsv" 2>/dev/null || : > "$tmp/table.tsv"; : > "$tmp/echecs.tsv"
ok=0; ko=0
while IFS= read -r -d '' f; do
    if ! read -r hash type < <("$XR_HASH" "$f" 2>/dev/null); then
        printf '%s\thash\t-\n' "${f#$src/}" >> "$tmp/echecs.tsv"; ko=$((ko+1)); continue
    fi
    grep -q "^$hash	" "$tmp/table.tsv" && continue   # doublon (même conteneur)
    # XenosRecomp plante (SIGSEGV) sur certains vertex shaders XDK : on isole chaque appel.
    { timeout 60 "$XENOSRECOMP" "$f" "$tmp/$hash.hlsl" "$XENOS_INCLUDE" > "$tmp/log" 2>&1; } 2>/dev/null   # sans « Segmentation fault » du shell
    rc=$?
    if [ $rc -ne 0 ] || [ ! -s "$tmp/$hash.hlsl" ]; then
        printf '%s\txenosrecomp\t%s\n' "${f#$src/}" "$rc" >> "$tmp/echecs.tsv"; ko=$((ko+1)); continue
    fi
    inv=; [ "$type" = vs ] && inv=-fvk-invert-y
    # Mêmes options que XenosRecomp/dxc_compiler.cpp (chemin SPIR-V).
    LD_LIBRARY_PATH="$DXC_LIB" timeout 120 "$DXC" -T "${type}_6_0" -HV 2021 -all-resources-bound \
        -spirv -fvk-use-dx-layout $inv -Qstrip_debug -E main -Fo "$tmp/$hash.spv" "$tmp/$hash.hlsl" > "$tmp/log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        printf '%s\tdxc\t%s\n' "${f#$src/}" "$rc" >> "$tmp/echecs.tsv"; ko=$((ko+1)); continue
    fi
    mv "$tmp/$hash.spv" "$out/$hash.spv" || exit 1
    printf '%s\t%s\t%s.spv\t%s\n' "$hash" "$type" "$hash" "${f#$src/}" >> "$tmp/table.tsv"
    ok=$((ok+1))
done < <(find "$src" -type f \( -name '*.xpu' -o -name '*.xvu' \) -print0 | sort -z)
mv "$tmp/table.tsv" "$out/table.tsv" && mv "$tmp/echecs.tsv" "$out/echecs.tsv" || exit 1
echo "shaders : $ok en SPIR-V, $ko échecs (voir $out/echecs.tsv)"
