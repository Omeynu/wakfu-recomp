#!/bin/sh
# Usage : [BUILD=<dossier de build>] [BATTERIE_DIR=<sorties>] flock /tmp/wakfu-jeu.lock runtime/tests/batterie.sh
# Batterie de non-régression ; un résultat par ligne. Shaders natifs : WAKFU_NATIF_SPIRV, sinon <build>/spirv
# (packaging/shaders/precompile-shaders.sh). Rendu natif comparé à une partie de référence : perf/comparer-natif.py.
# RAPIDE=1 : j1, j4 (RADV), j5-partie, unitaires, options (≈ 30 min, avant chaque push).
R=$(cd "$(dirname "$0")/../.." && pwd); W=$R/work; B=${BUILD:-$W/build-runtime}; T=$(cd "$(dirname "$0")" && pwd); S=${BATTERIE_DIR:-/tmp/wakfu-batterie}
rm -rf $S; mkdir -p $S; cd $W
export WAKFU_NATIF_SPIRV=${WAKFU_NATIF_SPIRV:-$B/spirv}
G="game/patched/image.bin game/patched/default.xex"; A="$G game/base game/tu2"
radv=/usr/share/vulkan/icd.d/radeon_icd.json; lvp=/usr/share/vulkan/icd.d/lvp_icd.json
r() { n=$1; shift; if "$@" > $S/$n.out 2>&1; then echo "OK    $n"; else echo "ÉCHEC $n"; tail -3 $S/$n.out; fi; }
r j1 sh $T/j1.sh $B/wakfu $G $S/j1.log
gpus='radv lvp'; js='j2 j3 j4'; [ "$RAPIDE" = 1 ] && { gpus=radv; js=j4; }
for gpu in $gpus; do eval icd=\$$gpu
  for j in $js; do mkdir -p $S/d-$j-$gpu; r $j-$gpu env VK_ICD_FILENAMES=$icd sh $T/$j.sh $B/wakfu $A $S/d-$j-$gpu $S/$j-$gpu.log; done; done
mkdir -p $S/d-partie $S/d-sauv
r j5-partie env VK_ICD_FILENAMES=$radv sh $T/j5-partie.sh $B/wakfu $A $S/d-partie $S/j5-partie.log
[ "$RAPIDE" = 1 ] || r j5-sauvegarde env VK_ICD_FILENAMES=$radv sh $T/j5-sauvegarde.sh $B/wakfu $A $S/d-sauv $S/j5-sauvegarde.log
for u in audio gpu_cp pad strings vk time config natif_textures natif_etats natif_sommets; do r unit_$u $B/wakfu_unit_$u; done
r unit_natif_raccord env VK_ICD_FILENAMES=$radv $B/wakfu_unit_natif_raccord game/base/Media/Shaders $WAKFU_NATIF_SPIRV
mkdir -p $S/d-opt; r options sh $T/options.sh $B/wakfu $A $S/d-opt $S/options.log
mkdir -p $S/d-prof; r unit_profile $B/wakfu_unit_profile $S/d-prof
r unit_xma $B/wakfu_unit_xma game/base/Media/Sounds/WaveBank.xwb
[ "$RAPIDE" = 1 ] && exit 0
X=$W/tools/XenonRecomp; r insn python3 -I $R/tests/insn/run_tests.py $X/build/XenonRecomp/XenonRecomp $X/XenonUtils $X/thirdparty/simde $W/tests-insn
