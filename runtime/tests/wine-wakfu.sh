#!/bin/sh
# Lance le binaire Windows (compilation croisée) sous Wine, pour exécuter les tests j*.sh sans PC Windows.
# Usage : WAKFU_WIN_EXE=<wakfu.exe> wine-wakfu.sh <arguments de wakfu>
: "${WAKFU_WIN_EXE:?WAKFU_WIN_EXE non défini}"
export WINEDEBUG=-all
exec /usr/lib/wine/wine64 "$WAKFU_WIN_EXE" "$@"
