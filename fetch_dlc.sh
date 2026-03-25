#!/bin/bash
# Fetch DLC App IDs from Steam API and generate cream_api.ini
# Usage: ./fetch_dlc.sh <APP_ID> [output_file]

APP_ID="$1"
OUTPUT="${2:-cream_api.ini}"

if [ -z "$APP_ID" ]; then
    echo "Usage: $0 <STEAM_APP_ID> [output_file]"
    echo "Example: $0 1385380"
    exit 1
fi

if ! command -v curl &>/dev/null; then
    echo "ERROR: curl is required but not installed."
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required but not installed."
    exit 1
fi

echo "Fetching DLC list for App ID: $APP_ID..."

# Get DLC IDs from Steam API
DLC_IDS=$(curl -s "https://store.steampowered.com/api/appdetails?appids=$APP_ID&filters=basic" | \
    python3 -c "
import sys, json
data = json.load(sys.stdin)
app = data.get('$APP_ID', {}).get('data', {})
dlcs = app.get('dlc', [])
for d in dlcs:
    print(d)
" 2>/dev/null)

if [ -z "$DLC_IDS" ]; then
    echo "No DLCs found for App ID $APP_ID (or API error)."
    exit 1
fi

DLC_COUNT=$(echo "$DLC_IDS" | wc -l)
echo "Found $DLC_COUNT DLCs. Fetching names..."

# Build the ini file
{
    echo "[config]"
    echo "# issubscribedapp_on_false_use_real = true"
    echo ""
    echo "[dlc]"

    for DLC_ID in $DLC_IDS; do
        DLC_NAME=$(curl -s "https://store.steampowered.com/api/appdetails?appids=$DLC_ID&filters=basic" | \
            python3 -c "
import sys, json
data = json.load(sys.stdin)
name = data.get('$DLC_ID', {}).get('data', {}).get('name', 'Unknown DLC $DLC_ID')
print(name)
" 2>/dev/null)
        [ -z "$DLC_NAME" ] && DLC_NAME="Unknown DLC $DLC_ID"
        echo "$DLC_ID = $DLC_NAME"
        echo "  $DLC_ID = $DLC_NAME" >&2
    done
} > "$OUTPUT"

echo ""
echo "Generated $OUTPUT with $DLC_COUNT DLCs."
