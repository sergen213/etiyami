#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export WINEPREFIX="$ROOT/.wine-yami"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="winemenubuilder.exe=d${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
if [ ! -f "$ROOT/game/eti-reference.exe" ]; then
    python3 "$ROOT/prepare_reference.py"
fi
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    BACKEND=wayland
    unset DISPLAY
else
    BACKEND=x11
fi
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d "$BACKEND" /f
cd "$ROOT/game"
exec wine ./eti-reference.exe
