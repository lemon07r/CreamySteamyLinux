#!/bin/bash
# CreamySteamyLinux launcher script
# Usage: sh ./creamy.sh %command%
# Set as Steam launch option: sh ./creamy.sh %command%

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find libsteam_api.so in the game directory
LIBSTEAM_API_DIR="$(find "$SCRIPT_DIR" -name "libsteam_api.so" -printf "%h\n" 2>/dev/null | head -n 1)"
if [ -z "$LIBSTEAM_API_DIR" ]; then
    echo "[CreamySteamy] ERROR: libsteam_api.so not found in game directory."
    exit 1
fi

# Check for cream_api.ini
if [ -z "$CREAM_CONFIG_PATH" ]; then
    if [ -f "$SCRIPT_DIR/cream_api.ini" ]; then
        export CREAM_CONFIG_PATH="$SCRIPT_DIR/cream_api.ini"
    elif [ -f "$PWD/cream_api.ini" ]; then
        export CREAM_CONFIG_PATH="$PWD/cream_api.ini"
    else
        echo "[CreamySteamy] ERROR: cream_api.ini not found."
        exit 1
    fi
fi

# Determine architecture and pick the right library
CREAMY_LIB=""
if [ -f "$SCRIPT_DIR/lib64CreamySteamy.so" ]; then
    CREAMY_LIB="$SCRIPT_DIR/lib64CreamySteamy.so"
elif [ -f "$SCRIPT_DIR/lib32CreamySteamy.so" ]; then
    CREAMY_LIB="$SCRIPT_DIR/lib32CreamySteamy.so"
else
    echo "[CreamySteamy] ERROR: No CreamySteamy .so library found."
    exit 1
fi

echo "[CreamySteamy] Config: $CREAM_CONFIG_PATH"
echo "[CreamySteamy] Library: $CREAMY_LIB"
echo "[CreamySteamy] Steam API: $LIBSTEAM_API_DIR/libsteam_api.so"

# LD_PRELOAD uses spaces as delimiters, so paths with spaces break it.
# Copy libraries to /tmp to avoid this issue.
TMP_CREAMY="/tmp/creamy_$$_lib64CreamySteamy.so"
TMP_STEAMAPI="/tmp/creamy_$$_libsteam_api.so"
cp "$CREAMY_LIB" "$TMP_CREAMY"
cp "$LIBSTEAM_API_DIR/libsteam_api.so" "$TMP_STEAMAPI"

cleanup() { rm -f "$TMP_CREAMY" "$TMP_STEAMAPI"; }
trap cleanup EXIT

export LD_PRELOAD="$TMP_CREAMY $TMP_STEAMAPI${LD_PRELOAD:+ $LD_PRELOAD}"

echo "[CreamySteamy] Launching: $@"
exec "$@"
