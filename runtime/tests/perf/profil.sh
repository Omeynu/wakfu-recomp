#!/bin/bash
# profil.sh <binaire> <étiquette> : partie sur la sauvegarde du joueur, en jeu : charge par thread (5 s) puis N (20) échantillons gdb de profondeur PROF (14)
BIN=$1; TAG=$2; O=${PROF_DIR:-/tmp/wakfu-prof}/$TAG; rm -rf $O; mkdir -p $O
cd "$(dirname "$0")/../../.."
BENCH_DIR=$O BENCH_TIMEOUT=1500 BIN=$BIN python3 -I runtime/tests/perf/bench-joueur.py $TAG WAKFU_STOP_AT_SWAP=20000 WAKFU_PROFIL=1 > $O/bench.txt 2>&1 &
L=$O/b2-$TAG.log
until [ -f $L ] && grep -qE "^\[image (4[0-9]{3})\] draws [0-9]{4}" $L; do sleep 5; done
PID=$(pgrep -nx wakfu)
declare -A a; for t in /proc/$PID/task/*; do s=($(sed 's/.*) //' $t/stat)); a[${t##*/}]=$(( ${s[11]} + ${s[12]} )); done
sleep 5
for t in /proc/$PID/task/*; do id=${t##*/}; s=($(sed 's/.*) //' $t/stat)); echo "$(( (${s[11]} + ${s[12]} - ${a[$id]:-0}) * 100 / 500 ))% $id $(cat $t/comm)"; done | sort -rn | head -8 > $O/threads.txt
for i in $(seq ${N:-20}); do gdb -p $PID -batch -ex "set pagination off" -ex "thread apply all bt ${PROF:-14}" > $O/s$i.txt 2>/dev/null; sleep 0.5; done
kill -9 $PID
echo fini
