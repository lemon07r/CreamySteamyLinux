#!/bin/bash
# CreamySteamyLinux - Deploy/Re-deploy Script
# Re-applies the proxy after Steam updates overwrite it.
#
# Usage:
#   ./deploy.sh                          # auto-detect game directory
#   ./deploy.sh /path/to/game/Plugins    # specify plugins directory
#   ./deploy.sh --rebuild                # rebuild proxy from source first
#   ./deploy.sh --status                 # check if proxy is in place
#   ./deploy.sh --restore                # restore original libsteam_api.so

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROXY_SRC="$SCRIPT_DIR/libsteam_api.so"
CONFIG_SRC="$SCRIPT_DIR/cream_api.ini"
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

# Find plugins directory containing libsteam_api.so
find_plugins_dir() {
    local search_root="${1:-$HOME/.local/share/Steam/steamapps/common}"
    find "$search_root" -name "libsteam_api.so" -printf "%h\n" 2>/dev/null | head -n 1
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
    info "Rebuilding proxy from source..."
    gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
        -o "$PROXY_SRC" "$SCRIPT_DIR/proxy.c" -ldl
    info "Built $PROXY_SRC"
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
    [ -f "$PROXY_SRC" ] || error "Proxy not built. Run: ./deploy.sh --rebuild"
    cp "$PROXY_SRC" "$dest/$PROXY_NAME"
    info "Deployed proxy → $dest/$PROXY_NAME"

    # Copy config if not present or if source is newer
    if [ -f "$CONFIG_SRC" ]; then
        if [ ! -f "$dest/cream_api.ini" ] || [ "$CONFIG_SRC" -nt "$dest/cream_api.ini" ]; then
            cp "$CONFIG_SRC" "$dest/cream_api.ini"
            info "Copied cream_api.ini"
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
            echo "Usage: $0 [OPTIONS] [PLUGINS_DIR]"
            echo ""
            echo "Options:"
            echo "  --status    Check if proxy is deployed"
            echo "  --restore   Restore original libsteam_api.so"
            echo "  --rebuild   Rebuild proxy from source before deploying"
            echo "  -h, --help  Show this help"
            echo ""
            echo "If PLUGINS_DIR is omitted, auto-detects from Steam library."
            exit 0
            ;;
        -*)         error "Unknown option: $arg" ;;
        *)          DEST="$arg" ;;
    esac
done

# Auto-detect if no directory given
if [ -z "$DEST" ]; then
    DEST="$(find_plugins_dir)"
    [ -n "$DEST" ] || error "Could not find libsteam_api.so. Specify the plugins directory manually."
    info "Auto-detected: $DEST"
fi

[ -d "$DEST" ] || error "Directory not found: $DEST"

$REBUILD && do_rebuild

case "$ACTION" in
    status)  do_status "$DEST" ;;
    restore) do_restore "$DEST" ;;
    deploy)  do_deploy "$DEST" ;;
esac
