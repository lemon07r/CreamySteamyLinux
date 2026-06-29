# CreamySteamyLinux

A lightweight, pure-C Steam DLC unlocker for native Linux Steam games.

## Why CreamySteamyLinux?

On Windows, tools like [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) handle DLC unlocking, but they don't work on Linux. The existing Linux alternatives ([creamlinux](https://github.com/anticitizn/creamlinux), [CreamTripApiLinux](https://github.com/KVarnitZ/CreamTripApiLinux)) are outdated, unmaintained, and only support the `LD_PRELOAD` approach, which **fails for many games**, particularly those with launchers (like Unity's "dowser" launcher) that spawn child processes without propagating environment variables.

CreamySteamyLinux was built from scratch in pure C to solve these problems:

| Problem | Old tools | CreamySteamyLinux |
|---|---|---|
| Games with launchers (e.g. Unity dowser) | ✗ LD_PRELOAD doesn't propagate to child processes | ✓ Proxy replacement works without env vars |
| C++ runtime dependency | ✗ creamlinux requires libstdc++, spdlog, etc. | ✓ Pure C, only libc and libdl |
| Binary size | ✗ ~3.6MB (creamlinux) | ✓ ~30KB (LD_PRELOAD) / ~300KB (proxy) |
| Maintenance | ✗ Last updated 2+ years ago | ✓ Actively maintained |
| Steam API coverage | ✗ Hooks only vtable-based interface calls | ✓ Proxy forwards all 900+ Steam API functions, overrides 9 DLC functions (flat API + the `SteamInternal_FindOrCreateUserInterface` C++ vtable) |
| Platform | ✗ Linux only | ✓ Linux `.so` and Windows `.dll` proxies |
| Static analysis | ✗ None | ✓ Tested with gcc -fanalyzer, cppcheck, flawfinder, clang-tidy, scan-build |

### How it was built

The proxy approach works the same way CreamInstaller does on Windows (DLL proxying), adapted for both Linux shared objects (`.so`) and Windows libraries (`.dll`):

1. **Export extraction**: `deploy.py` extracts all exported symbols from the target game's Steam API library (`nm -D` for `.so`, `winedump` for `.dll`)
2. **Code generation**: `deploy.py` emits a small game-specific header of naked-asm trampolines (one entry per forwarded symbol) that jump to the real library (loaded as `steam_api_o.so` / `steam_api_o.dll`). The trampolines use `r11` (not `rax`, which the SysV ABI reserves for variadic calls) and the static `proxy.c` provides everything else.
3. **DLC overrides**: 9 DLC-related functions (`BIsDlcInstalled`, `BIsSubscribedApp`, `GetDLCCount`, `BGetDLCDataByIndex`, `SteamInternal_FindOrCreateUserInterface` vtable patch, etc.) are replaced with implementations that read `cream_api.ini` and report all listed DLCs as owned
4. **Compile & deploy**: The proxy is compiled on the fly and deployed as a drop-in replacement — works with any game's Steam API version automatically, on Linux or (via [msvc-wine](https://github.com/mstorsjo/msvc-wine)) Windows

### Recent improvements

The current proxy architecture builds on a refactor generously shared by **jeO8vQJcr0gny** on [cs.rin.ru](https://cs.rin.ru/forum/viewtopic.php?p=3515330#p3515330). Ported from that work:

- **Flexible trampoline generation** — only the API-specific code is generated; the rest lives in a static `proxy.c`. Trampolines are stored as an array + `#define` index table (easier to hack on) and can optionally print on every hook.
- **ABI bug fix** — trampolines jump through `r11` instead of `rax`, because the SysV ABI uses `rax` to pass the variadic-argument register count.
- **Single entry point** — `deploy.py` replaces the old `deploy.sh` + `gen_proxy.py`, and writes its temp files (`forward.txt`, `generate.h`) next to the game library for easy debugging.
- **Vtable fallback fix** — `SteamInternal_FindOrCreateUserInterface` now keeps a private copy of the real vtable, fixing an infinite loop that occurred when falling back to the genuine implementation.
- **Windows `.dll` support** — in addition to Linux `.so`, `deploy.py` can build a `steam_api64.dll` / `steam_api.dll` proxy via `clang-cl` + `lld-link` + `winedump`.

On top of the port, this repository adds:

- **Verified on more titles** — confirmed unlocking on BATTLETECH (`.so`, flat-API path) alongside the upstream Victoria 3 (`.so`) and Total War: Warhammer 3 (`.dll`) testing.
- **Clean static analysis** — fixed sign-conversion and non-prototype warnings so `check.sh` (gcc `-fanalyzer`, cppcheck, flawfinder, clang-tidy, scan-build) stays clean.
- **Updated CI** — the GitHub Actions release workflow now ships `proxy.c` + `deploy.py` (the proxy is generated per-game at deploy time) and runs a `proxy.c` syntax check, instead of publishing an obsolete prebuilt generic proxy.

## How It Works

CreamySteamyLinux provides **two approaches** for unlocking DLCs:

### Approach 1: Proxy Replacement (Recommended)

Replaces the game's `libsteam_api.so` with a proxy that forwards all calls to the original library while overriding DLC-related functions. This is the same approach used by [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) on Windows. **Use this method.** It works with all games, including those with launchers that don't propagate environment variables.

### Approach 2: LD_PRELOAD Hook (Fallback)

Uses `LD_PRELOAD` to intercept Steam API calls before they reach the real `libsteam_api.so`. Only use this if the proxy approach doesn't work for your specific game.

## Key Features

- **Pure C** - no C++ runtime, no external dependencies
- **Simple INI config** - just list DLC IDs and names in `cream_api.ini`
- **Two hooking methods** - LD_PRELOAD for simple cases, proxy replacement for everything else
- **Debug logging** - optional file logging for troubleshooting (set `CREAMY_LOG=1`)
- **Lightweight** - tiny binaries (~30KB LD_PRELOAD, ~300KB proxy)
- **Cross-platform proxy** - generates both Linux `.so` and Windows `.dll` proxies
- **Game-specific proxy** - a static `proxy.c` plus an auto-generated trampoline header built from the game's actual Steam API exports

## Building

### LD_PRELOAD library

```bash
./build.sh
```

This produces `lib64CreamySteamy.so` (64-bit) in the `build/` directory.

To cross-compile 32-bit as well (requires `gcc-multilib`):

```bash
./build.sh --32
```

### Proxy library

You don't build this by hand — `deploy.py` generates a game-specific trampoline header and compiles `proxy.c` against it automatically (see below). The temp files (`forward.txt`, `generate.h`) are written to a `tmp/` directory next to the game's library so they're easy to inspect when debugging.

Linux proxies need `clang`; Windows `.dll` proxies additionally need `clang-cl`, `lld-link`, and `winedump` (install [msvc-wine](https://github.com/mstorsjo/msvc-wine)).

## Installation

### Quick Start (Recommended)

`deploy.py` automatically generates a game-specific proxy tailored to each game's Steam API version. No pre-built binaries needed — it extracts symbols, generates code, and compiles on the fly. It auto-detects whether the game uses `libsteam_api.so`, `steam_api64.dll`, or `steam_api.dll` and builds the matching proxy.

1. Generate `cream_api.ini` for your game:
   ```bash
   ./fetch_dlc.sh <APP_ID>
   ```
   Replace `<APP_ID>` with your game's Steam App ID (find it on the game's Steam store page URL).

2. Deploy:
   ```bash
   ./deploy.py /path/to/game
   ```
   That's it. The script finds the Steam API library wherever it lives, backs up the original (`steam_api_o.so` / `steam_api_o.dll`), generates a proxy matched to that game's API version, and deploys it. A `cream_api.ini` in your current directory is copied alongside it.

3. Launch the game normally — no launch options, no LD_PRELOAD, nothing.

To restore the original: `./deploy.py --restore /path/to/game`

### Re-deploying After Steam Updates

Steam updates overwrite the Steam API library, breaking the proxy. Just re-run:

```bash
./deploy.py /path/to/game
```

The proxy is regenerated fresh each time, so it always matches the game's current Steam API version — even if Steam updated the library.

Run `./deploy.py --status /path/to/game` to check, or `./deploy.py --restore /path/to/game` to undo.

### Method 2: LD_PRELOAD (Fallback)

Only use this if Method 1 doesn't work for your game.

1. Generate `cream_api.ini` for your game:
   ```bash
   ./fetch_dlc.sh <APP_ID>
   ```
   Replace `<APP_ID>` with your game's Steam App ID.

2. Copy these files to your game's root directory:
   - `lib64CreamySteamy.so` (and/or `lib32CreamySteamy.so`)
   - `creamy.sh`
   - `cream_api.ini` (generated in step 1)

3. In Steam: Right-click game > Properties > Launch Options:
   ```
   sh ./creamy.sh %command%
   ```

4. Launch the game!

## cream_api.ini Format

```ini
[config]
# issubscribedapp_on_false_use_real = true

[dlc]
12345 = DLC Name One
67890 = DLC Name Two
```

## Static Analysis

Run all static analyzers (gcc -fanalyzer, cppcheck, flawfinder, clang-tidy, scan-build):

```bash
./check.sh
```

## Troubleshooting

Set `CREAMY_LOG=1` to enable debug logging to `creamy_log.txt`:

```
CREAMY_LOG=1 sh ./creamy.sh %command%
```

For the proxy approach, set the environment variable before launching:

```
CREAMY_LOG=1 %command%
```

## Credits

Cross-platform proxy refactor (flexible trampoline generation, `r11` ABI fix, unified `deploy.py`, vtable fallback fix, and Windows `.dll` support) based on improvements shared by **jeO8vQJcr0gny** on [cs.rin.ru](https://cs.rin.ru/forum/viewtopic.php?p=3515330#p3515330).

Inspired by:
- [CreamAPI](https://cs.rin.ru/forum/viewtopic.php?f=29&t=70576) by deadmau5 (cs.rin.ru) — the original Steam DLC unlocker
- [creamlinux](https://github.com/anticitizn/creamlinux) by anticitizn
- [CreamTripApiLinux](https://github.com/KVarnitZ/CreamTripApiLinux) by KVarnitZ
- [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) by FroggMaster
- [SmokeAPI](https://github.com/acidicoala/SmokeAPI) by acidicoala

## License

MIT License
