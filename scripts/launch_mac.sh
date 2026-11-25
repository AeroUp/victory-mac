#!/bin/sh
# Launch Grand Theft Auto VI (Xbox Series build) on Apple Silicon macOS via Wine + MoltenVK.
# Offline-only (blocked via macOS sandbox-exec). Uses real PS5/Xbox controllers or keyboard (no menu bypass).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

GAME_DIR="${LEONIDA_GAME_DIR:-$HOME/Downloads/ISOs/GTA6 Xbox personal}"
SHADERS_DIR="${LEONIDA_SHADERS_MAC:-$HOME/Leonida/work/shaders/spirv}"
STATE_DIR="${LEONIDA_STATE_DIR:-$HOME/Leonida}"
OVERLAY_DIR="$STATE_DIR/overlay"
SAVE_DIR="$STATE_DIR/saves"
WINEPREFIX="${WINEPREFIX:-$HOME/.wine-leonida}"
WINDOW_SIZE="${LEONIDA_WINDOW:-2560x1440}"
DEVICE_TYPE="${LEONIDA_DEVICE_TYPE:-7}"  # 7 = Xbox Series X, 6 = Xbox Series S
RESET_SAVES=0
EXTRA_ARGS=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --game-dir DIR      Path to your Xbox Series package folder (default: $GAME_DIR)
  --series-s          Emulate Xbox Series S (lower internal resolution, upscaled to --window)
  --series-x          Emulate Xbox Series X (default)
  --window WxH        Output window resolution (default: $WINDOW_SIZE, e.g. 1280x720, 1920x1080, 2560x1440)
  --reset-saves       Clear saved settings & overlay so first-boot setup screens appear again
  --help              Show this message

Controller & input environment overrides:
  LEONIDA_KEYBOARD_PAD=1|always|off    Keyboard stand-in gamepad (default: 1 = when focused)
  LEONIDA_DS_TRIGGERS=0|1              PS5 DualSense adaptive triggers
  LEONIDA_DS_LIGHTBAR=0|1              PS5 DualSense lightbar
  LEONIDA_RUMBLE=0|1                   Controller rumble
  LEONIDA_TITLE_FPS=0|1                Show FPS / ms / draws / frame in window title (default: 1)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --game-dir) GAME_DIR="$2"; shift 2 ;;
        --series-s) DEVICE_TYPE=6; shift ;;
        --series-x) DEVICE_TYPE=7; shift ;;
        --window) WINDOW_SIZE="$2"; shift 2 ;;
        --reset-saves) RESET_SAVES=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
    esac
done

WINE_BIN=""
for candidate in \
    "${WINE:-}" \
    "$HOME/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine" \
    "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine" \
    "$(command -v wine 2>/dev/null || true)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        WINE_BIN="$candidate"
        break
    fi
done

if [ -z "$WINE_BIN" ]; then
    echo "Error: Wine not found. Install Wine Stable or set WINE=/path/to/wine." >&2
    exit 1
fi

RT_EXE="$REPO_DIR/runtime/build/windows/gta6rt.exe"
if [ ! -f "$RT_EXE" ]; then
    echo "Error: $RT_EXE not found. Build gta6rt.exe first (see docs/macos.md)." >&2
    exit 1
fi

if [ ! -f "$GAME_DIR/game_scarlett_master.exe" ]; then
    echo "Error: game_scarlett_master.exe not found in: $GAME_DIR" >&2
    echo "Pass --game-dir /path/to/your/xbox/files or set LEONIDA_GAME_DIR." >&2
    exit 1
fi

if [ "$RESET_SAVES" = "1" ]; then
    echo "Resetting saves ($SAVE_DIR) and overlay ($OVERLAY_DIR)..."
    rm -rf "$SAVE_DIR" "$OVERLAY_DIR"
fi

mkdir -p "$OVERLAY_DIR" "$SAVE_DIR"

# The DirectStorage BCPack predictor profile, built once from your own package (docs/directstorage.md). Without
# it, BC7 streams that need it fail with E_DSTORAGE_DECOMPRESSION_ERROR and those textures stay missing. Takes a
# few minutes the first time (about 3 on an M3 Pro); later launches reuse it.
BCPACK_PROFILE="$(dirname "$SHADERS_DIR")/bcpack/history.bcph"
if [ ! -f "$BCPACK_PROFILE" ]; then
    PYTHON_BIN=""
    for candidate in python3.15 python3.14 python3.13 python3; do
        if command -v "$candidate" >/dev/null 2>&1; then PYTHON_BIN=$(command -v "$candidate"); break; fi
    done
    if [ -n "$PYTHON_BIN" ]; then
        echo "Building the BCPack texture profile from your game files (first launch only)..."
        mkdir -p "$(dirname "$BCPACK_PROFILE")"
        "$PYTHON_BIN" -I "$REPO_DIR/tools/build_dstorage_history.py" "$GAME_DIR" --out "$BCPACK_PROFILE" --if-needed ||
            echo "Warning: could not build $BCPACK_PROFILE; some textures may be missing." >&2
    else
        echo "Warning: no python3 found to build $BCPACK_PROFILE; some textures may be missing." >&2
    fi
fi

to_win_path() {
    printf 'Z:%s' "$(printf '%s' "$1" | sed 's|/|\\|g')"
}

WIN_GAME_DIR=$(to_win_path "$GAME_DIR")
WIN_EXE="$WIN_GAME_DIR\\game_scarlett_master.exe"
WIN_OVERLAY=$(to_win_path "$OVERLAY_DIR")
WIN_SAVES=$(to_win_path "$SAVE_DIR")
WIN_SHADERS=$(to_win_path "$SHADERS_DIR")

OFFLINE_SB="$STATE_DIR/offline.sb"
cat > "$OFFLINE_SB" <<'SBEOF'
(version 1)
(allow default)
(deny network-outbound)
(deny network-inbound)
(allow network-outbound (local unix))
(allow network-inbound (local unix))
SBEOF

export WINEPREFIX
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-winedbg.exe=d}"
export ROSETTA_ADVERTISE_AVX=1
export LEONIDA_D3D12=vulkan
export LEONIDA_EXPERIMENT_MESH_SIGNATURE="${LEONIDA_EXPERIMENT_MESH_SIGNATURE:-1}"
export LEONIDA_SHADERS="$WIN_SHADERS"
export LEONIDA_SAVE_DIR="$WIN_SAVES"
export LEONIDA_ARENA_MB="${LEONIDA_ARENA_MB:-2048}"
export LEONIDA_GRAPHICS_STAGE_LAYOUT="${LEONIDA_GRAPHICS_STAGE_LAYOUT:-1}"
export LEONIDA_PIPELINE_COMPILE="${LEONIDA_PIPELINE_COMPILE:-on-demand}"
export LEONIDA_GPU_LABELS="${LEONIDA_GPU_LABELS:-1}"
export LEONIDA_FRAME_PACING="${LEONIDA_FRAME_PACING:-1}"
export LEONIDA_PRESENT_NO_TEAR="${LEONIDA_PRESENT_NO_TEAR:-1}"
export MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE="${MVK_CONFIG_FULL_IMAGE_VIEW_SWIZZLE:-1}"
export MVK_CONFIG_FAST_MATH_ENABLED="${MVK_CONFIG_FAST_MATH_ENABLED:-0}"
export LEONIDA_FPS_LOG="${LEONIDA_FPS_LOG:-1}"
export LEONIDA_COMPUTE_WARMUP="${LEONIDA_COMPUTE_WARMUP:-0}"
export LEONIDA_DEVICE_TYPE="$DEVICE_TYPE"
export LEONIDA_WINDOW="$WINDOW_SIZE"
export LEONIDA_KEYBOARD_PAD="${LEONIDA_KEYBOARD_PAD:-1}"
export LEONIDA_TITLE_FPS="${LEONIDA_TITLE_FPS:-1}"

PROFILE_NAME="Xbox Series X"
[ "$DEVICE_TYPE" = "6" ] && PROFILE_NAME="Xbox Series S"

echo "Launching VIctory Mac (MoltenVK / Apple Silicon)"
echo "  Profile : $PROFILE_NAME (LEONIDA_DEVICE_TYPE=$DEVICE_TYPE)"
echo "  Window  : $WINDOW_SIZE"
echo "  Game    : $GAME_DIR"
echo "  Shaders : $SHADERS_DIR"
echo "  Saves   : $SAVE_DIR"
echo "  Overlay : $OVERLAY_DIR"
echo "  Network : blocked (offline sandbox)"
echo ""

# shellcheck disable=SC2086
exec sandbox-exec -f "$OFFLINE_SB" "$WINE_BIN" "$RT_EXE" \
    --run "$WIN_EXE" \
    --game-dir "$WIN_GAME_DIR" \
    --overlay "$WIN_OVERLAY" \
    $EXTRA_ARGS
