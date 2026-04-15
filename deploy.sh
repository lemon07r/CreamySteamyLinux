#!/bin/bash
# CreamySteamyLinux - Deploy/Re-deploy Script
# Generates a game-specific proxy and deploys it. Always uses the proxy method.
#
# Usage:
#   ./deploy.sh /path/to/game           # point to game folder, auto-finds libsteam_api.so
#   ./deploy.sh                          # auto-detect (searches CWD, then Steam library)
#   ./deploy.sh --status [/path/to/game] # check if proxy is in place
#   ./deploy.sh --restore [/path/to/game]# restore original libsteam_api.so
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

# Find the directory containing libsteam_api.so within a game folder
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
    ( set +o pipefail; strings "$1" 2>/dev/null | grep -q "CreamySteamy" )
}

# Generate a game-specific proxy from the original library's symbols
# This is the core of the proxy approach: extract all exports, generate
# trampolines, compile a drop-in replacement.
generate_proxy() {
    local original="$1"
    local output="$2"
    local tmpdir
    tmpdir="$(mktemp -d /tmp/creamy_build_XXXXXX)"

    info "Generating game-specific proxy..."

    # Extract all exported function symbols from the original library
    # Filter: only "T" (text/code) symbols, skip _init/_fini
    nm -D "$original" | awk '$2 == "T" { print $3 }' | grep -v '^_init$\|^_fini$' | sort > "$tmpdir/all_exports.txt"

    local total
    total="$(wc -l < "$tmpdir/all_exports.txt")"
    info "Found $total exported symbols in original library"

    # DLC override functions — these are implemented in the proxy, not forwarded
    local overrides=(
        SteamAPI_ISteamApps_BIsDlcInstalled
        SteamAPI_ISteamApps_BIsSubscribedApp
        SteamAPI_ISteamApps_BIsSubscribed
        SteamAPI_ISteamApps_GetDLCCount
        SteamAPI_ISteamApps_BGetDLCDataByIndex
        SteamAPI_ISteamApps_BIsAppInstalled
        SteamAPI_ISteamUser_UserHasLicenseForApp
        SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime
        SteamInternal_FindOrCreateUserInterface
    )

    # Filter out override functions to get the forwarded-only list
    local override_pattern
    override_pattern="$(printf '%s\n' "${overrides[@]}" | paste -sd'|')"
    grep -vE "^($override_pattern)$" "$tmpdir/all_exports.txt" > "$tmpdir/forward.txt"

    local fwd_count
    fwd_count="$(wc -l < "$tmpdir/forward.txt")"
    info "Forwarding $fwd_count symbols, overriding ${#overrides[@]} DLC functions"

    # Check that gen_proxy.py is available
    local gen_script="$SCRIPT_DIR/gen_proxy.py"
    if [ ! -f "$gen_script" ]; then
        error "gen_proxy.py not found in $SCRIPT_DIR — cannot generate proxy"
    fi

    # Generate proxy.c
    cp "$tmpdir/forward.txt" /tmp/steam_api_forward.txt
    python3 "$gen_script" > "$tmpdir/proxy.c"

    # Compile
    gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
        -o "$output" "$tmpdir/proxy.c" -ldl 2>&1

    # Verify no undefined _fwd_ symbols leaked
    local undef_count
    undef_count="$(nm -D "$output" 2>/dev/null | grep ' U ' | grep -c '_fwd_' || true)"
    if [ "$undef_count" -gt 0 ]; then
        error "Build error: $undef_count undefined _fwd_ symbols in proxy"
    fi

    info "Proxy compiled successfully ($total symbols)"
    rm -rf "$tmpdir"
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

    if [ -f "$dest/$ORIGINAL_NAME" ]; then
        info "Backup: $ORIGINAL_NAME present"
    else
        warn "Backup: $ORIGINAL_NAME missing"
    fi
    if [ -f "$dest/cream_api.ini" ]; then
        info "Config: cream_api.ini present"
    else
        warn "Config: cream_api.ini missing"
    fi
}

do_restore() {
    local dest="$1"
    [ -f "$dest/$ORIGINAL_NAME" ] || error "No backup ($ORIGINAL_NAME) found — cannot restore"
    mv "$dest/$ORIGINAL_NAME" "$dest/$PROXY_NAME"
    info "Restored original $PROXY_NAME"
}

do_deploy() {
    local dest="$1"

    # Step 1: Ensure we have the original library backed up
    if [ -f "$dest/$PROXY_NAME" ] && ! is_proxy "$dest/$PROXY_NAME"; then
        if [ -f "$dest/$ORIGINAL_NAME" ]; then
            warn "Backup $ORIGINAL_NAME already exists, skipping rename"
        else
            mv "$dest/$PROXY_NAME" "$dest/$ORIGINAL_NAME"
            info "Backed up original → $ORIGINAL_NAME"
        fi
    fi

    [ -f "$dest/$ORIGINAL_NAME" ] || error "No original library ($ORIGINAL_NAME) found — nothing to proxy"

    # Step 2: Generate a game-specific proxy from the original library
    local proxy_out
    proxy_out="$(mktemp /tmp/creamy_proxy_XXXXXX.so)"
    generate_proxy "$dest/$ORIGINAL_NAME" "$proxy_out"

    # Step 3: Deploy
    cp "$proxy_out" "$dest/$PROXY_NAME"
    rm -f "$proxy_out"
    info "Deployed proxy → $dest/$PROXY_NAME"

    # Step 4: Handle cream_api.ini
    # Look for a config source: repo, script dir, or CWD
    local config_src=""
    if [ -f "$SCRIPT_DIR/cream_api.ini" ]; then
        config_src="$SCRIPT_DIR/cream_api.ini"
    elif [ -f "$PWD/cream_api.ini" ]; then
        config_src="$PWD/cream_api.ini"
    fi

    if [ -n "$config_src" ]; then
        if [ ! -f "$dest/cream_api.ini" ] || [ "$config_src" -nt "$dest/cream_api.ini" ]; then
            if [ "$(realpath "$config_src")" != "$(realpath "$dest/cream_api.ini" 2>/dev/null || true)" ]; then
                cp "$config_src" "$dest/cream_api.ini"
                info "Copied cream_api.ini"
            else
                info "cream_api.ini already up to date"
            fi
        else
            info "cream_api.ini already up to date"
        fi
    else
        if [ ! -f "$dest/cream_api.ini" ]; then
            warn "No cream_api.ini found — generate one with: ./fetch_dlc.sh <APP_ID>"
        else
            info "Using existing cream_api.ini"
        fi
    fi

    echo ""
    info "Done! Launch the game normally — no launch options needed."
}

# --- Main ---

ACTION="deploy"
DEST=""

for arg in "$@"; do
    case "$arg" in
        --status)   ACTION="status" ;;
        --restore)  ACTION="restore" ;;
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
            echo "  The proxy is generated specifically for each game's Steam API version."
            echo "  No launch options or LD_PRELOAD needed — just deploy and play."
            echo ""
            echo "Options:"
            echo "  --status    Check if proxy is deployed"
            echo "  --restore   Restore original libsteam_api.so"
            echo "  -h, --help  Show this help"
            exit 0
            ;;
        -*)         error "Unknown option: $arg" ;;
        *)          DEST="$arg" ;;
    esac
done

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
