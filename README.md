# CreamySteamyLinux

A lightweight, pure-C Steam DLC unlocker for native Linux Steam games.

## How It Works

CreamySteamyLinux provides **two approaches** for unlocking DLCs:

### Approach 1: LD_PRELOAD Hook (Simple)

Uses `LD_PRELOAD` to intercept Steam API calls before they reach the real `libsteam_api.so`. Works great for games that launch directly.

### Approach 2: Proxy Replacement (Recommended)

Replaces the game's `libsteam_api.so` with a proxy that forwards all calls to the original library while overriding DLC-related functions. This is the same approach used by [CreamInstaller](https://github.com/FroggMaster/CreamInstaller) on Windows and works even when games use launchers that don't propagate `LD_PRELOAD`.

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

### Method 1: LD_PRELOAD (Simple games)

1. Copy these files to your game's root directory:
   - `build/lib64CreamySteamy.so` (and/or `build/lib32CreamySteamy.so`)
   - `creamy.sh`
   - `cream_api.ini` (edit with your game's DLC IDs)

2. In Steam: Right-click game → Properties → Launch Options:
   ```
   sh ./creamy.sh %command%
   ```

3. Launch the game!

### Method 2: Proxy Replacement (Games with launchers)

1. Navigate to the game's directory where `libsteam_api.so` lives (e.g. `GameName_Data/Plugins/`)
2. Rename the original: `mv libsteam_api.so libsteam_api_o.so`
3. Copy the proxy: `cp libsteam_api.so <game plugins dir>/libsteam_api.so`
4. Copy `cream_api.ini` to the same directory as the proxy `libsteam_api.so`
5. Launch the game normally — no special launch options needed!

To restore the original, just rename `libsteam_api_o.so` back to `libsteam_api.so`.

## cream_api.ini Format

```ini
[config]
# issubscribedapp_on_false_use_real = true

[dlc]
12345 = DLC Name One
67890 = DLC Name Two
```

## Auto-fetching DLC IDs

Run the included helper to auto-populate `cream_api.ini` for any Steam game:

```bash
./fetch_dlc.sh 1385380
```

(Replace `1385380` with your game's Steam App ID)

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
