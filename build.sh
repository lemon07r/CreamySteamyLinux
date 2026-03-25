#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p build

echo "Building 64-bit lib64CreamySteamy.so..."
gcc -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
    -o build/lib64CreamySteamy.so creamy.c -ldl
echo "  -> build/lib64CreamySteamy.so"

if [ "$1" = "--32" ]; then
    echo "Building 32-bit lib32CreamySteamy.so..."
    gcc -m32 -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
        -o build/lib32CreamySteamy.so creamy.c -ldl
    echo "  -> build/lib32CreamySteamy.so"
fi

echo "Done!"
