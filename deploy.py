#!/usr/bin/env python3
import os
import re
import sys
import json
import shutil
import argparse
import urllib.parse
import urllib.request
from pathlib import Path

PROXY_C = Path(__file__).parent.resolve() / "proxy.c"

# Constants
LIB_NAMES = ["libsteam_api.so", "steam_api64.dll", "steam_api.dll"]

PLATFORM_LINUX = "linux"
PLATFORM_WINDOWS = "windows"

LIBNAME_LINUX = "steam_api_o.so"
LIBNAME_WINDOWS = "steam_api_o.dll"

STEAM_STORE_API = "https://store.steampowered.com/api/appdetails"

# Files this tool (and the older cream* tools) leave behind; removed on --uninstall.
LEFTOVER_NAMES = [
    "creamy_log.txt",
    "creamy.sh",
    "cream.sh",
    "lib64CreamySteamy.so",
    "lib32CreamySteamy.so",
    "lib64Creamlinux.so",
    "lib32Creamlinux.so",
]

# Colors
RED = '\033[0;31m'
GREEN = '\033[0;32m'
YELLOW = '\033[1;33m'
NC = '\033[0m'

def info(msg):
    print(f"{GREEN}[+]{NC} {msg}")

def warn(msg):
    print(f"{YELLOW}[!]{NC} {msg}")

def error(msg):
    print(f"{RED}[-]{NC} {msg}")
    sys.exit(1)

def run(cmd, **kwargs):
    cmd_str = ""
    for arg in cmd:
        cmd_str += " "
        if " " in arg:
            cmd_str += f"\"{arg}\""
        else:
            cmd_str += arg
    info("run:" + cmd_str)
    import subprocess
    return subprocess.run(cmd, **kwargs)

def detect_platform(proxy_name):
    if str(proxy_name).endswith(".so"):
        return PLATFORM_LINUX
    else:
        return PLATFORM_WINDOWS

def generate_include(forward_fns, platform):
    out = ""
    out += "/* ========================================================================= */\n"
    out += "/* Auto-generated forwarding stubs                                           */\n"
    out += "/* ========================================================================= */\n"
    out += "\n"
    out += "/* These use a generic variadic forwarding approach via function pointers */\n"
    out += "\n"

    fn2idx = {}
    idx = 0
    for fn in forward_fns:
        out += f"#define _FWD_IDX_{fn} {idx}\n"
        fn2idx[fn] = idx
        idx = idx + 1
    out += f"#define _FWD_IDX_COUNT {idx}\n"

    out += "\n"
    out += 'static const char *_fwd_name[_FWD_IDX_COUNT] = {\n'

    for fn in forward_fns:
        out += f'\"{fn}\",\n'

    out += '};\n'
    out += "\n"

    out += "\n"
    out += f'__attribute__((used)) static void *_fwd_ptr[{idx}];\n'
    out += "\n"

    for fn in forward_fns:
        out += f'/* {fn} */\n'
        out += f'LIB_EXPORT __attribute__((naked)) void {fn}(void) {{\n'
        out += f'    __asm__ volatile (\n'
        # This is used for debug
        # out += f'        "int3\\n"\n'
        # out += f'        "leaq API_ID(%%rip), %%r11\\n"\n'
        # out += f'        "movl ${fn2idx[fn]}, (%%r11)\\n"\n'
        # out += f'        "pushq %%r11\\n"\n'
        # out += f'        "call  api_hook\\n"\n'
        # out += f'        "popq %%r11\\n"\n'
        out += f'        "movq _fwd_ptr+{fn2idx[fn]*8}(%%rip), %%r11\\n"\n'
        out += f'        "jmp *%%r11\\n"\n'
        out += f'        :::\n'
        out += f'    );\n'
        out += f'}}\n'
        out += "\n"
    return out


def find_steam_api_dir(search_root, maxdepth=6):
    """Depth-bounded search for a steam_api library inside a game folder."""
    search_root = Path(search_root)
    if search_root.is_file() and search_root.name in LIB_NAMES:
        return search_root.parent, search_root
    if not search_root.is_dir():
        return None, None
    base_depth = len(search_root.resolve().parts)
    for root, dirs, files in os.walk(search_root):
        depth = len(Path(root).resolve().parts) - base_depth
        if depth >= maxdepth:
            dirs[:] = []
        for name in LIB_NAMES:
            if name in files:
                return Path(root), Path(root) / name
    return None, None


# --------------------------------------------------------------------------- #
# Steam library / game discovery                                              #
# --------------------------------------------------------------------------- #
def steam_libraries():
    """Return all steamapps directories Steam knows about (multi-drive aware)."""
    candidates = [
        Path.home() / ".local/share/Steam/steamapps",
        Path.home() / ".steam/steam/steamapps",
        Path.home() / ".steam/root/steamapps",
    ]
    libs = []
    for c in candidates:
        if not c.exists():
            continue
        r = c.resolve()
        if r not in libs:
            libs.append(r)
        vdf = c / "libraryfolders.vdf"
        if vdf.exists():
            txt = vdf.read_text(encoding="utf-8", errors="ignore")
            for m in re.finditer(r'"path"\s*"([^"]+)"', txt):
                p = (Path(m.group(1).replace("\\\\", "/")) / "steamapps").resolve()
                if p.exists() and p not in libs:
                    libs.append(p)
    return libs


def list_installed_games():
    """List (name, libdir, libpath) for every installed game with a steam_api lib."""
    games = []
    seen = set()
    for sa in steam_libraries():
        common = sa / "common"
        if not common.is_dir():
            continue
        for game in sorted(common.iterdir(), key=lambda p: p.name.lower()):
            if not game.is_dir():
                continue
            libdir, libpath = find_steam_api_dir(game)
            if libdir and str(libpath) not in seen:
                seen.add(str(libpath))
                games.append((game.name, libdir, libpath))
    return games


def pick_game():
    info("Scanning installed Steam games...")
    games = list_installed_games()
    if not games:
        error("No installed games with a steam_api library were found.")
    print("\nInstalled games:")
    for i, (name, _, libpath) in enumerate(games, 1):
        tag = " (proxy active)" if is_proxy(libpath) else ""
        print(f"  {i:2}. {name}{tag}")
    try:
        choice = input("\nPick a game number (or 'q' to quit): ").strip()
    except EOFError:
        sys.exit(0)
    if choice.lower() in ("q", "quit", ""):
        sys.exit(0)
    if not choice.isdigit() or not (1 <= int(choice) <= len(games)):
        error(f"Invalid selection: {choice}")
    name, libdir, libpath = games[int(choice) - 1]
    info(f"Selected: {name}")
    return libdir, libpath


def detect_app_id(lib_path):
    """Map a game's installdir to its appmanifest_<id>.acf to find the App ID."""
    parts = Path(lib_path).resolve().parts
    if "common" not in parts:
        return None
    idx = parts.index("common")
    steamapps_dir = Path(*parts[:idx])
    installdir = parts[idx + 1] if idx + 1 < len(parts) else None
    if not installdir:
        return None
    for acf in steamapps_dir.glob("appmanifest_*.acf"):
        txt = acf.read_text(encoding="utf-8", errors="ignore")
        m_install = re.search(r'"installdir"\s*"([^"]+)"', txt)
        m_appid = re.search(r'"appid"\s*"(\d+)"', txt)
        if m_install and m_appid and m_install.group(1).lower() == installdir.lower():
            return m_appid.group(1)
    return None


# --------------------------------------------------------------------------- #
# DLC fetching (folds in the old fetch_dlc.sh logic, stdlib only)             #
# --------------------------------------------------------------------------- #
def _appdetails(appid):
    url = STEAM_STORE_API + "?" + urllib.parse.urlencode(
        {"appids": str(appid), "filters": "basic"})
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.load(resp)


def fetch_dlc(appid):
    """Return [(dlc_id, name), ...] for an app, or None on any failure."""
    try:
        data = _appdetails(appid)
    except Exception as e:
        warn(f"Could not reach the Steam store API: {e}")
        return None
    app = data.get(str(appid), {})
    if not app.get("success"):
        warn(f"Steam store API has no data for App ID {appid}")
        return None
    dlc_ids = app.get("data", {}).get("dlc", [])
    result = []
    for did in dlc_ids:
        name = f"Unknown DLC {did}"
        try:
            d = _appdetails(did)
            nm = d.get(str(did), {}).get("data", {}).get("name")
            if nm:
                name = nm
        except Exception:
            pass
        result.append((did, name))
    return result


def write_config(path, dlcs):
    lines = ["[config]", "# issubscribedapp_on_false_use_real = true", "", "[dlc]"]
    for did, name in dlcs:
        lines.append(f"{did} = {name}")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def is_proxy(file_path):
    try:
        result = run(["strings", str(file_path)], capture_output=True, text=True)
        return "CreamySteamy" in result.stdout
    except Exception:
        return False


def generate_proxy(original, output, temp_dir, platform):
    import subprocess
    info(f"Generating game-specific proxy for {original.name}...")

    try:
        all_exports = []
        if platform == PLATFORM_WINDOWS:
            dump_proc = run(["winedump", "-j", "export", str(original)], capture_output=True, text=True, check=True)
            start_parsing = False
            for line in dump_proc.stdout.splitlines():
                if "Entry Pt" in line and "Name" in line:
                    start_parsing = True
                    continue
                if "Done dumping" in line:
                    break
                if start_parsing and line.strip():
                    parts = line.split()
                    if len(parts) >= 3:
                        symbol = parts[2]
                        if "Steam" in symbol:
                            all_exports.append(symbol)
        else:
            assert platform == PLATFORM_LINUX
            nm_proc = run(["nm", "-D", str(original)], capture_output=True, text=True, check=True)
            for line in nm_proc.stdout.splitlines():
                parts = line.split()
                if len(parts) >= 3 and parts[1] == "T":
                    symbol = parts[2]
                    if not symbol.startswith("__") and symbol not in ["_init", "_fini"]:
                        all_exports.append(symbol)

        all_exports = sorted(list(set(all_exports)))
        total = len(all_exports)
        info(f"Found {total} exported symbols in original library")

        overrides = {
            "SteamAPI_ISteamApps_BIsDlcInstalled",
            "SteamAPI_ISteamApps_BIsSubscribedApp",
            "SteamAPI_ISteamApps_BIsSubscribed",
            "SteamAPI_ISteamApps_GetDLCCount",
            "SteamAPI_ISteamApps_BGetDLCDataByIndex",
            "SteamAPI_ISteamApps_BIsAppInstalled",
            "SteamAPI_ISteamUser_UserHasLicenseForApp",
            "SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime",
            "SteamInternal_FindOrCreateUserInterface",
        }

        forward = [s for s in all_exports if s not in overrides]
        info(f"Forwarding {len(forward)} symbols, overriding {len(overrides)} DLC functions")

        forward_file = temp_dir / "forward.txt"
        forward_file.write_text("\n".join(forward) + "\n")

        generated_header_text = generate_include(forward, platform)
        generated_header = Path.absolute(temp_dir / "generate.h")
        with open(generated_header, "w", encoding="utf-8") as f:
            f.write(generated_header_text)

        # Compile
        if platform == PLATFORM_WINDOWS:
            obj_file = temp_dir / "out.obj"
            run([
                "clang-cl", "-c", "-g", "-O2", "-Wno-unused-parameter", "-Wno-reserved-identifier",
                "-Wno-declaration-after-statement",
                "-o", str(obj_file), str(PROXY_C), f"-DAUTOMATICALLY_GENERATED_STUFF=\"{generated_header}\""
            ], check=True)
            run([
                "lld-link", "-dll", "-subsystem:windows", str(obj_file), f"-out:{output}"
            ], check=True)
        else:
            assert platform == PLATFORM_LINUX
            run([
                "clang", "-shared", "-g", "-fPIC", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                "-o", str(output), str(PROXY_C), f"-DAUTOMATICALLY_GENERATED_STUFF=\"{generated_header}\"", "-ldl"
            ], check=True)

        info(f"Proxy compiled successfully ({total} symbols)")
    except subprocess.CalledProcessError as e:
        error(f"Command failed: {e}")


def summarize_log(dest):
    log = dest / "creamy_log.txt"
    if not log.exists():
        warn("No creamy_log.txt yet — launch the game once to generate it.")
        return
    txt = log.read_text(encoding="utf-8", errors="ignore")
    unlocked = sorted(set(re.findall(r'\((\d{4,7})\)[^\n]*UNLOCKED', txt)), key=int)
    counts = re.findall(r'GetDLCCount -> (\d+)', txt)
    info(f"Last run log: {log}")
    if counts:
        info(f"Game queried GetDLCCount -> {counts[-1]}")
    info(f"DLC reported as owned/unlocked: {len(unlocked)}")
    if unlocked:
        print("    " + ", ".join(unlocked))


def do_status(dest, proxy_name, original_name, platform):
    print("=== CreamySteamyLinux Status ===")
    print(f"Directory: {dest}")

    proxy_path = dest / proxy_name
    if proxy_path.exists() and is_proxy(proxy_path):
        info(f"Proxy is IN PLACE ({proxy_name})")
    elif proxy_path.exists():
        warn(f"Original {proxy_name} found (proxy NOT deployed)")
    else:
        error(f"{proxy_name} not found in {dest}")

    backup_path = dest / original_name
    if backup_path.exists():
        info(f"Backup: {original_name} present")
    else:
        warn(f"Backup: {original_name} missing")

    config_path = dest / "cream_api.ini"
    if config_path.exists():
        info("Config: cream_api.ini present")
    else:
        warn("Config: cream_api.ini missing")

    summarize_log(dest)


def do_restore(dest, proxy_name, original_name, platform):
    backup_path = dest / original_name
    proxy_path = dest / proxy_name
    if not backup_path.exists():
        error(f"No backup ({original_name}) found — cannot restore")

    shutil.move(str(backup_path), str(proxy_path))
    info(f"Restored original {proxy_name}")


def game_root_of(dest):
    """Best-effort game root: the parent of the *_Data folder, if any."""
    for anc in Path(dest).resolve().parents:
        if anc.name.endswith("_Data"):
            return anc.parent
    return None


def do_uninstall(dest, proxy_name, original_name, platform):
    # 1. Restore the original library
    backup_path = dest / original_name
    proxy_path = dest / proxy_name
    if backup_path.exists():
        shutil.move(str(backup_path), str(proxy_path))
        info(f"Restored original {proxy_name}")
    else:
        warn(f"No backup ({original_name}) — leaving {proxy_name} untouched")

    # 2. Remove leftovers from the lib dir and the game root
    targets = {dest}
    root = game_root_of(dest)
    if root:
        targets.add(root)

    removed = []
    for d in targets:
        for n in LEFTOVER_NAMES + ["cream_api.ini"]:
            f = d / n
            if f.exists():
                f.unlink()
                removed.append(str(f))
        tmp = d / "tmp"
        if tmp.is_dir() and (tmp / "generate.h").exists():
            shutil.rmtree(tmp)
            removed.append(str(tmp))

    if removed:
        info("Removed leftover files:")
        for r in removed:
            print(f"    {r}")
    else:
        info("No leftover files to remove")
    info("Uninstalled. The game now uses its original Steam library.")


def ensure_config(dest, appid_arg, no_fetch):
    """Write cream_api.ini next to the lib, fetching the DLC list when possible."""
    dest_config = dest / "cream_api.ini"

    if not no_fetch:
        appid = appid_arg or detect_app_id(dest)
        if appid:
            info(f"App ID: {appid} (auto-detected)" if not appid_arg else f"App ID: {appid}")
            dlcs = fetch_dlc(appid)
            if dlcs is not None:
                write_config(dest_config, dlcs)
                info(f"Wrote cream_api.ini with {len(dlcs)} DLCs")
                return
            warn("Keeping any existing config (DLC fetch failed)")
        else:
            warn("Could not auto-detect App ID; pass --app-id or provide cream_api.ini")

    # Offline / fallback: copy a cream_api.ini from CWD if the lib dir has none
    if not dest_config.exists():
        cwd_config = Path.cwd() / "cream_api.ini"
        if cwd_config.exists() and cwd_config.resolve() != dest_config.resolve():
            shutil.copy(str(cwd_config), str(dest_config))
            info("Copied cream_api.ini from current directory")
        else:
            warn("No cream_api.ini found — DLC will not be unlocked until one exists")
    else:
        info("Using existing cream_api.ini")


def do_deploy(dest, proxy_name, original_name, platform, appid_arg, no_fetch):
    temp_dir = Path(dest / "tmp")
    if not os.path.isdir(temp_dir):
        os.mkdir(temp_dir)

    proxy_path = dest / proxy_name
    backup_path = dest / original_name

    # Step 1: Ensure we have the original library backed up
    if proxy_path.exists():
        if is_proxy(proxy_path):
            warn(f"Proxy already deployed at {proxy_path}, rebuilding proxy")
        elif backup_path.exists():
            warn(f"Backup {original_name} already exists, skipping rename")
        else:
            shutil.copy(str(proxy_path), str(backup_path))
            info(f"Backed up original -> {original_name}")
    else:
        error(f"expected {proxy_path}, but not found")

    if not backup_path.exists():
        error(f"{proxy_path} is a proxy, and there is no original at {backup_path}")
    if is_proxy(backup_path):
        error(f"{backup_path} is a proxy, verify game files")

    generate_proxy(backup_path, proxy_path, temp_dir, platform)

    # Step 2: DLC config (auto-fetched by default)
    ensure_config(dest, appid_arg, no_fetch)

    print("")
    info("Done! Launch the game normally.")


def main():
    parser = argparse.ArgumentParser(description="CreamySteamyLinux - one-command DLC unlocker")
    parser.add_argument("game_dir", nargs="?", help="Game folder or directory with the steam_api library (omit to pick from a list)")
    parser.add_argument("--status", action="store_true", help="Check status and summarize the last run")
    parser.add_argument("--restore", action="store_true", help="Restore the original steam_api library")
    parser.add_argument("--uninstall", action="store_true", help="Restore original and remove leftover files")
    parser.add_argument("--app-id", help="Override the Steam App ID used to fetch DLC")
    parser.add_argument("--no-fetch", action="store_true", help="Do not fetch DLC online; use existing cream_api.ini")

    args = parser.parse_args()

    if args.game_dir:
        game_dir = Path.absolute(Path(args.game_dir))
        dest, proxy_name = find_steam_api_dir(game_dir)
        if not dest:
            error(f"No steam_api library found in {game_dir}")
        info(f"Found {proxy_name.name} in: {dest}")
    else:
        dest, proxy_path = pick_game()
        proxy_name = proxy_path.name

    proxy_name = Path(proxy_name).name
    platform = detect_platform(proxy_name)
    original_name = LIBNAME_LINUX if platform == PLATFORM_LINUX else LIBNAME_WINDOWS

    if args.status:
        do_status(dest, proxy_name, original_name, platform)
    elif args.restore:
        do_restore(dest, proxy_name, original_name, platform)
    elif args.uninstall:
        do_uninstall(dest, proxy_name, original_name, platform)
    else:
        do_deploy(dest, proxy_name, original_name, platform, args.app_id, args.no_fetch)


if __name__ == "__main__":
    main()
