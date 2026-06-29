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

## How It Works

CreamySteamyLinux provides **two approaches** for unlocking DLCs:

### Approach 1: Proxy Replacement (Recommended)

Replaces the game's `libsteam_api.so` with a proxy that forwards all calls to the original library while overriding DLC-related functions. This is the same approach used by [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) on Windows. **Use this method.** It works with all games, including those with launchers that don't propagate environment variables.

### Approach 2: LD_PRELOAD Hook (Fallback)

Uses `LD_PRELOAD` to intercept Steam API calls before they reach the real `libsteam_api.so`. Only use this if the proxy approach doesn't work for your specific game.

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

One command does everything — no App ID hunting, no separate config step:

```bash
./deploy.py
```

With no arguments it lists your installed Steam games (across all library folders) and lets you pick one by number. Then it:

1. Finds the game's Steam API library and backs up the original (`steam_api_o.so` / `steam_api_o.dll`).
2. Auto-detects the Steam App ID from the game's `appmanifest`.
3. Fetches the current DLC list online and writes `cream_api.ini` next to the library.
4. Generates and compiles a proxy matched to that game's exact Steam API version (auto-detecting `.so` vs `.dll`) and deploys it.

Then just launch the game normally — no launch options, no `LD_PRELOAD`, nothing.

You can also point it straight at a game:

```bash
./deploy.py "/path/to/game"
```

Useful flags:

| Command | What it does |
|---|---|
| `./deploy.py [game]` | Deploy / re-deploy (re-fetches the DLC list each time) |
| `./deploy.py --status [game]` | Show whether the proxy is active and summarize the last run's unlocked DLC |
| `./deploy.py --restore [game]` | Restore the original Steam library |
| `./deploy.py --uninstall [game]` | Restore the original **and** remove leftover files (logs, configs, old `LD_PRELOAD` helpers) |
| `./deploy.py --app-id <id> [game]` | Override the auto-detected App ID |
| `./deploy.py --no-fetch [game]` | Skip the online fetch and use an existing `cream_api.ini` (offline) |

### Re-deploying After Steam Updates

Steam updates overwrite the Steam API library (and games sometimes add new DLC). Just re-run:

```bash
./deploy.py "/path/to/game"
```

The proxy is regenerated fresh and the DLC list is re-fetched every time, so it always matches the game's current Steam API version and current DLC set.

### Method 2: LD_PRELOAD (Fallback)

Only use this if the proxy method doesn't work for your game. It needs a `cream_api.ini` (the proxy method writes one for you; you can reuse that file, or write one by hand using the format below).

1. Copy these files to your game's root directory:
   - `lib64CreamySteamy.so` (and/or `lib32CreamySteamy.so`)
   - `creamy.sh`
   - `cream_api.ini`

2. In Steam: Right-click game > Properties > Launch Options:
   ```
   sh ./creamy.sh %command%
   ```

3. Launch the game!

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
