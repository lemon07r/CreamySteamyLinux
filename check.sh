#!/bin/bash
# Static analysis suite for CreamySteamyLinux
# Runs all available analyzers on the C source files.
set -e

cd "$(dirname "$0")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

SOURCES="creamy.c proxy.c"
ISSUES=0

section() { echo -e "\n${YELLOW}=== $1 ===${NC}"; }

# 1. GCC warnings + -fanalyzer
section "GCC Warnings + Analyzer"
if gcc -fanalyzer -Wall -Wextra -Wshadow -Wformat=2 -Wconversion \
      -Wnull-dereference -Wdouble-promotion -Wstrict-prototypes \
      -Wno-unused-parameter -Wno-missing-prototypes -Wno-pedantic \
      -std=c11 -c $SOURCES 2>&1 | tee /dev/stderr | grep -qE "error:"; then
    ISSUES=$((ISSUES+1))
fi
rm -f creamy.o proxy.o

# 2. cppcheck
section "cppcheck"
cppcheck --enable=all --std=c11 \
    --suppress=missingIncludeSystem \
    --suppress=unusedFunction \
    --suppress=knownConditionTrueFalse \
    $SOURCES 2>&1

# 3. flawfinder (security)
section "flawfinder (level 3+)"
flawfinder --minlevel=3 $SOURCES 2>&1

# 4. clang-tidy
section "clang-tidy"
for f in $SOURCES; do
    echo "--- $f ---"
    clang-tidy "$f" -- -std=c11 -D_GNU_SOURCE 2>&1 || true
done

# 5. scan-build (path-sensitive)
section "scan-build"
for f in $SOURCES; do
    echo "--- $f ---"
    scan-build gcc -shared -fPIC -o /dev/null "$f" -ldl 2>&1 | tail -2
done

echo -e "\n${GREEN}=== Analysis complete ===${NC}"
