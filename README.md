# CreamySteamyLinux

A lightweight, pure-C Steam DLC unlocker for native Linux Steam games.

## How It Works

CreamySteamyLinux uses `LD_PRELOAD` to intercept Steam API calls before they reach the real `libsteam_api.so`. It hooks key functions like `SteamAPI_Init`, `SteamInternal_CreateInterface`, and `SteamInternal_FindOrCreateUserInterface` to replace the ISteamApps interface with a custom implementation that reports all configured DLCs as owned.

### Key Features

- **Pure C** — no C++ runtime, no spdlog, no external dependencies
- **Simple INI config** — just list DLC IDs and names in `cream_api.ini`
- **Robust hooking** — intercepts both flat API and interface-based API calls
- **Debug logging** — optional file logging for troubleshooting (set `CREAMY_LOG=1`)
- **Lightweight** — single source file, tiny binary

## Building

```bash
./build.sh
```

This produces `lib64CreamySteamy.so` (64-bit) in the `build/` directory.

To cross-compile 32-bit as well (requires `gcc-multilib`):
```bash
./build.sh --32
```

## Installation

1. Copy these files to your game's root directory:
   - `build/lib64CreamySteamy.so` (and/or `build/lib32CreamySteamy.so`)
   - `creamy.sh`
   - `cream_api.ini` (edit with your game's DLC IDs)

2. In Steam: Right-click game → Properties → Launch Options:
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

## Auto-fetching DLC IDs

Run the included helper to auto-populate `cream_api.ini` for any Steam game:

```bash
./fetch_dlc.sh 1385380
```

(Replace `1385380` with your game's Steam App ID)

## Troubleshooting

Set `CREAMY_LOG=1` in your environment or launch options to enable debug logging to `creamy_log.txt`:
```
CREAMY_LOG=1 sh ./creamy.sh %command%
```

## Credits

Inspired by:
- [creamlinux](https://github.com/anticitizn/creamlinux) by anticitizn
- [CreamTripApiLinux](https://github.com/KVarnitZ/CreamTripApiLinux) by KVarnitZ
- [SmokeAPI](https://github.com/acidicoala/SmokeAPI) by acidicoala

## License

MIT License
