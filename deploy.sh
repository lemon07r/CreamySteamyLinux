#!/bin/bash
# CreamySteamyLinux - Deploy/Re-deploy Script
# Re-applies the proxy after Steam updates overwrite it.
#
# Usage:
#   ./deploy.sh /path/to/game           # point to game folder, auto-finds libsteam_api.so
#   ./deploy.sh                          # auto-detect (searches Steam library, or uses CWD if dropped in game folder)
#   ./deploy.sh --status [/path/to/game] # check if proxy is in place
#   ./deploy.sh --restore [/path/to/game]# restore original libsteam_api.so
#   ./deploy.sh --rebuild                # rebuild proxy from source first
#
# The script can be dropped into a game folder and run directly from there.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIGINAL_NAME="steam_api_o.so"
PROXY_NAME="libsteam_api.so"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[-]${NC} $*"; exit 1; }

# Locate the proxy .so and cream_api.ini sources.
# If the script lives in the repo (next to proxy.c), use the repo build.
# If dropped into a game folder, look for them next to the script or in CWD.
find_sources() {
    PROXY_SRC=""
    CONFIG_SRC=""

    # Check repo location (script dir has proxy.c → it's the repo)
    if [ -f "$SCRIPT_DIR/proxy.c" ]; then
        if [ -f "$SCRIPT_DIR/libsteam_api.so" ]; then PROXY_SRC="$SCRIPT_DIR/libsteam_api.so"; fi
        if [ -f "$SCRIPT_DIR/cream_api.ini" ];   then CONFIG_SRC="$SCRIPT_DIR/cream_api.ini"; fi
    fi

    # Fallback: check next to script
    if [ -z "$PROXY_SRC" ] && [ -f "$SCRIPT_DIR/libsteam_api.so" ]; then PROXY_SRC="$SCRIPT_DIR/libsteam_api.so"; fi
    if [ -z "$CONFIG_SRC" ] && [ -f "$SCRIPT_DIR/cream_api.ini" ];  then CONFIG_SRC="$SCRIPT_DIR/cream_api.ini"; fi

    # Fallback: check CWD
    if [ -z "$PROXY_SRC" ] && [ -f "$PWD/libsteam_api.so" ]; then PROXY_SRC="$PWD/libsteam_api.so"; fi
    if [ -z "$CONFIG_SRC" ] && [ -f "$PWD/cream_api.ini" ];  then CONFIG_SRC="$PWD/cream_api.ini"; fi
}

# Find the directory containing libsteam_api.so within a game folder
# Searches recursively from the given root
find_steam_api_dir() {
    local search_root="$1"
    find "$search_root" -maxdepth 5 -name "libsteam_api.so" -printf "%h\n" 2>/dev/null | head -n 1
}

# Auto-detect: try CWD as game folder, then search Steam library
auto_detect_game() {
    local result=""

    # If CWD contains or has a child with libsteam_api.so, use CWD as game root
    result="$(find_steam_api_dir "$PWD")"
    if [ -n "$result" ]; then
        echo "$result"
        return
    fi

    # If script dir contains or has a child with libsteam_api.so (dropped in game folder)
    # But skip if script dir is the repo
    if [ ! -f "$SCRIPT_DIR/proxy.c" ]; then
        result="$(find_steam_api_dir "$SCRIPT_DIR")"
        if [ -n "$result" ]; then
            echo "$result"
            return
        fi
    fi

    # Search entire Steam library
    local steam_root="$HOME/.local/share/Steam/steamapps/common"
    if [ -d "$steam_root" ]; then
        result="$(find_steam_api_dir "$steam_root")"
        if [ -n "$result" ]; then
            echo "$result"
            return
        fi
    fi
}

# Resolve: given a path that might be a game root, find the actual dir with libsteam_api.so
resolve_plugins_dir() {
    local path="$1"

    # If it directly contains libsteam_api.so, use it
    if [ -f "$path/$PROXY_NAME" ]; then
        echo "$path"
        return
    fi

    # Search within it
    local found
    found="$(find_steam_api_dir "$path")"
    if [ -n "$found" ]; then
        echo "$found"
        return
    fi

    # Nothing found
    return 1
}

# Check if a file is our proxy (contains CreamySteamy marker)
is_proxy() {
    strings "$1" 2>/dev/null | grep -q "CreamySteamy"
}

do_status() {
    local dest="$1"
    echo "=== CreamySteamyLinux Status ==="
    echo "Directory: $dest"

    if [ -f "$dest/$PROXY_NAME" ] && is_proxy "$dest/$PROXY_NAME"; then
        info "Proxy is IN PLACE ($PROXY_NAME)"
    elif [ -f "$dest/$PROXY_NAME" ]; then
        warn "Original $PROXY_NAME found (proxy NOT deployed)"
    else
        error "$PROXY_NAME not found in $dest"
    fi

    [ -f "$dest/$ORIGINAL_NAME" ] && info "Backup: $ORIGINAL_NAME present" || warn "Backup: $ORIGINAL_NAME missing"
    [ -f "$dest/cream_api.ini" ]   && info "Config: cream_api.ini present"  || warn "Config: cream_api.ini missing"
}

do_restore() {
    local dest="$1"
    [ -f "$dest/$ORIGINAL_NAME" ] || error "No backup ($ORIGINAL_NAME) found — cannot restore"
    mv "$dest/$ORIGINAL_NAME" "$dest/$PROXY_NAME"
    info "Restored original $PROXY_NAME"
}

do_rebuild() {
    [ -f "$SCRIPT_DIR/proxy.c" ] || error "proxy.c not found — cannot rebuild (not in repo directory?)"
    info "Rebuilding proxy from source..."
    gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
        -o "$SCRIPT_DIR/libsteam_api.so" "$SCRIPT_DIR/proxy.c" -ldl
    info "Built $SCRIPT_DIR/libsteam_api.so"
    PROXY_SRC="$SCRIPT_DIR/libsteam_api.so"
}

do_deploy() {
    local dest="$1"

    # If libsteam_api.so exists and is NOT our proxy, back it up
    if [ -f "$dest/$PROXY_NAME" ] && ! is_proxy "$dest/$PROXY_NAME"; then
        if [ -f "$dest/$ORIGINAL_NAME" ]; then
            warn "Backup $ORIGINAL_NAME already exists, skipping rename"
        else
            mv "$dest/$PROXY_NAME" "$dest/$ORIGINAL_NAME"
            info "Backed up original → $ORIGINAL_NAME"
        fi
    fi

    # Verify backup exists
    [ -f "$dest/$ORIGINAL_NAME" ] || error "No original library ($ORIGINAL_NAME) found — nothing to proxy"

    # Copy proxy
    [ -n "${PROXY_SRC:-}" ] && [ -f "$PROXY_SRC" ] || error "Proxy not built. Run: ./deploy.sh --rebuild"

    # Don't copy over itself
    if [ "$(realpath "$PROXY_SRC")" != "$(realpath "$dest/$PROXY_NAME" 2>/dev/null || true)" ]; then
        cp "$PROXY_SRC" "$dest/$PROXY_NAME"
        info "Deployed proxy → $dest/$PROXY_NAME"
    else
        info "Proxy already in place"
    fi

    # Copy config if not present or if source is newer
    if [ -n "${CONFIG_SRC:-}" ] && [ -f "$CONFIG_SRC" ]; then
        if [ ! -f "$dest/cream_api.ini" ] || [ "$CONFIG_SRC" -nt "$dest/cream_api.ini" ]; then
            # Don't copy over itself
            if [ "$(realpath "$CONFIG_SRC")" != "$(realpath "$dest/cream_api.ini" 2>/dev/null || true)" ]; then
                cp "$CONFIG_SRC" "$dest/cream_api.ini"
                info "Copied cream_api.ini"
            else
                info "cream_api.ini already up to date"
            fi
        else
            info "cream_api.ini already up to date"
        fi
    else
        [ -f "$dest/cream_api.ini" ] || warn "No cream_api.ini found — generate one with: ./fetch_dlc.sh <APP_ID>"
    fi

    echo ""
    info "Done! Launch the game normally."
}

# --- Main ---

ACTION="deploy"
DEST=""
REBUILD=false

for arg in "$@"; do
    case "$arg" in
        --status)   ACTION="status" ;;
        --restore)  ACTION="restore" ;;
        --rebuild)  REBUILD=true ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS] [GAME_DIR]"
            echo ""
            echo "  GAME_DIR can be the game's root folder or the specific directory"
            echo "  containing libsteam_api.so. The script will search recursively."
            echo ""
            echo "  If omitted, the script auto-detects by searching:"
            echo "    1. Current working directory (for when dropped in a game folder)"
            echo "    2. Script's own directory (if not the repo)"
            echo "    3. Steam library (~/.local/share/Steam/steamapps/common/)"
            echo ""
            echo "Options:"
            echo "  --status    Check if proxy is deployed"
            echo "  --restore   Restore original libsteam_api.so"
            echo "  --rebuild   Rebuild proxy from source before deploying"
            echo "  -h, --help  Show this help"
            exit 0
            ;;
        -*)         error "Unknown option: $arg" ;;
        *)          DEST="$arg" ;;
    esac
done

find_sources

if $REBUILD; then do_rebuild; fi

# Resolve target directory
if [ -n "$DEST" ]; then
    [ -d "$DEST" ] || error "Directory not found: $DEST"
    RESOLVED="$(resolve_plugins_dir "$DEST")" || error "No libsteam_api.so found in $DEST (searched recursively)"
    DEST="$RESOLVED"
    info "Found libsteam_api.so in: $DEST"
else
    DEST="$(auto_detect_game)"
    [ -n "$DEST" ] || error "Could not find libsteam_api.so. Specify the game directory: $0 /path/to/game"
    info "Auto-detected: $DEST"
fi

case "$ACTION" in
    status)  do_status "$DEST" ;;
    restore) do_restore "$DEST" ;;
    deploy)  do_deploy "$DEST" ;;
esac
