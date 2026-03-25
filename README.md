# CreamySteamyLinux

A lightweight, pure-C Steam DLC unlocker for native Linux Steam games.

## Why CreamySteamyLinux?

On Windows, tools like [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) make DLC unlocking effortless — but they don't work on Linux. The existing Linux alternatives ([creamlinux](https://github.com/anticitizn/creamlinux), [CreamTripApiLinux](https://github.com/KVarnitZ/CreamTripApiLinux)) are outdated, unmaintained, and only support the `LD_PRELOAD` approach, which **fails for many games** — particularly those with launchers (like Unity's "dowser" launcher) that spawn child processes without propagating environment variables.

CreamySteamyLinux was built from scratch in pure C to solve these problems:

| Problem | Old tools | CreamySteamyLinux |
|---|---|---|
| Games with launchers (e.g. Unity dowser) | ❌ LD_PRELOAD doesn't propagate to child processes | ✅ Proxy replacement works transparently — no env vars needed |
| C++ runtime dependency | ❌ creamlinux requires libstdc++, spdlog, etc. | ✅ Pure C — only libc and libdl |
| Binary size | ❌ ~3.6MB (creamlinux) | ✅ ~30KB (LD_PRELOAD) / ~300KB (proxy) |
| Maintenance | ❌ Last updated 2+ years ago | ✅ Actively maintained |
| Steam API coverage | ❌ Hooks only vtable-based interface calls | ✅ Proxy forwards all 1100+ Steam API functions, overrides 8 DLC-related flat API functions |
| Static analysis | ❌ None | ✅ Tested with gcc -fanalyzer, cppcheck, flawfinder, clang-tidy, scan-build |

### How it was built

The proxy approach works the same way CreamInstaller does on Windows (DLL proxying), adapted for Linux shared objects:

1. **Export extraction** — All exported symbols from the game's real `libsteam_api.so` are extracted using `nm -D`
2. **Code generation** — A Python script generates `proxy.c` with assembly trampolines that forward every function call to the real library (loaded as `libsteam_api_o.so` via `dlopen`)
3. **DLC overrides** — 8 DLC-related functions (`BIsDlcInstalled`, `BIsSubscribedApp`, `GetDLCCount`, `BGetDLCDataByIndex`, etc.) are replaced with implementations that read `cream_api.ini` and report all listed DLCs as owned
4. **Drop-in replacement** — The compiled proxy is a direct replacement for `libsteam_api.so`, so the game loads it without any special configuration

## How It Works

CreamySteamyLinux provides **two approaches** for unlocking DLCs:

### Approach 1: Proxy Replacement (Recommended)

Replaces the game's `libsteam_api.so` with a proxy that forwards all calls to the original library while overriding DLC-related functions. This is the same approach used by [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) on Windows. **Use this method** — it works with all games, including those with launchers that don't propagate environment variables.

### Approach 2: LD_PRELOAD Hook (Fallback)

Uses `LD_PRELOAD` to intercept Steam API calls before they reach the real `libsteam_api.so`. Only use this if the proxy approach doesn't work for your specific game.

## Key Features

- **Pure C** — no C++ runtime, no external dependencies
- **Simple INI config** — just list DLC IDs and names in `cream_api.ini`
- **Two hooking methods** — LD_PRELOAD for simple cases, proxy replacement for everything else
- **Debug logging** — optional file logging for troubleshooting (set `CREAMY_LOG=1`)
- **Lightweight** — tiny binaries (~30KB LD_PRELOAD, ~300KB proxy)
- **Auto-generated proxy** — `proxy.c` is generated from the game's actual `libsteam_api.so` exports

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

```bash
gcc -shared -fPIC -O2 -o libsteam_api.so proxy.c -ldl
```

## Installation

### Method 1: Proxy Replacement (Recommended)

This is the recommended method. It works with all games and requires no special Steam launch options.

1. Generate `cream_api.ini` for your game:
   ```bash
   ./fetch_dlc.sh <APP_ID>
   ```
   Replace `<APP_ID>` with your game's Steam App ID (find it on the game's Steam store page URL).

2. Find `libsteam_api.so` in your game's directory (e.g. `GameName_Data/Plugins/`)

3. Back up the original: `mv libsteam_api.so libsteam_api_o.so`

4. Copy the proxy and config to the same directory:
   ```bash
   cp libsteam_api.so <game plugins dir>/libsteam_api.so
   cp cream_api.ini <game plugins dir>/cream_api.ini
   ```

5. Launch the game normally — no special launch options needed!

To restore the original, just rename `libsteam_api_o.so` back to `libsteam_api.so`.

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

3. In Steam: Right-click game → Properties → Launch Options:
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

Inspired by:
- [creamlinux](https://github.com/anticitizn/creamlinux) by anticitizn
- [CreamTripApiLinux](https://github.com/KVarnitZ/CreamTripApiLinux) by KVarnitZ
- [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) by FroggMaster
- [SmokeAPI](https://github.com/acidicoala/SmokeAPI) by acidicoala

## License

MIT License
