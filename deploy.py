#!/usr/bin/env python3
import os
import sys
import subprocess
import shutil
import argparse
from pathlib import Path

PROXY_C = Path(__file__).parent.resolve() / "proxy.c"

# Constants
LIB_NAMES = ["libsteam_api.so", "steam_api64.dll", "steam_api.dll"]

PLATFORM_LINUX = "linux"
PLATFORM_WINDOWS = "windows"

LIBNAME_LINUX = "steam_api_o.so"
LIBNAME_WINDOWS = "steam_api_o.dll"

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
    info("run: " + cmd_str)
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


def find_steam_api_dir(search_root):
    search_root = Path(search_root)
    for name in LIB_NAMES:
        for path in search_root.rglob(name):
            if path.is_file():
                return path.parent, path
    return None, None

def auto_detect_game():
    cwd = Path.cwd()
    res, name = find_steam_api_dir(cwd)
    if res:
        return res, name

    script_dir = Path(__file__).parent.resolve()
    if not (script_dir / "proxy.c").exists():
        res, name = find_steam_api_dir(script_dir)
        if res:
            return res, name

    steam_root = Path.home() / ".local/share/Steam/steamapps/common"
    if steam_root.exists():
        res, name = find_steam_api_dir(steam_root)
        if res:
            return res, name

    return None, None

def is_proxy(file_path):
    try:
        result = run(["strings", str(file_path)], capture_output=True, text=True)
        return "CreamySteamy" in result.stdout
    except Exception:
        return False


def generate_proxy(original, output, temp_dir, platform):
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
            # clang-cl -c ./test/test_lib.c -o out.o
            # lld-link -dll -subsystem:windows out.o user32.lib -out:lib.dll
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


def do_status(dest, proxy_name: Path, original_name, platform):
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

def do_restore(dest, proxy_name, original_name, platform):
    backup_path = dest / original_name
    proxy_path = dest / proxy_name
    if not backup_path.exists():
        error(f"No backup ({original_name}) found — cannot restore")

    shutil.move(str(backup_path), str(proxy_path))
    info(f"Restored original {proxy_name}")

def do_deploy(dest, proxy_name, original_name, platform):
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
        error("expected {proxy_path}, but not found")

    if not backup_path.exists():
        error(f"{proxy_path} is a proxy, and there is no original at {backup_path}")
    if is_proxy(backup_path):
        error(f"{backup_path} is a proxy, verify game files")

    generate_proxy(backup_path, proxy_path, temp_dir, platform)

    # Step 4: Handle cream_api.ini
    config_src = None
    if (Path.cwd() / "cream_api.ini").exists():
        config_src = Path.cwd() / "cream_api.ini"

    dest_config = dest / "cream_api.ini"
    if config_src:
        should_copy = False
        if not dest_config.exists():
            should_copy = True
        elif config_src.stat().st_mtime > dest_config.stat().st_mtime:
            if config_src.resolve() != dest_config.resolve():
                should_copy = True

        if should_copy:
            shutil.copy(str(config_src), str(dest_config))
            info("Copied cream_api.ini")
        else:
            info("cream_api.ini already up to date")
    else:
        if not dest_config.exists():
            warn("No cream_api.ini found — generate one with: ./fetch_dlc.sh <APP_ID>")
        else:
            info("Using existing cream_api.ini")

    print("")
    info("Done! Launch the game normally.")


def main():
    parser = argparse.ArgumentParser(description="CreamySteamyLinux - Deploy/Re-deploy Script")
    parser.add_argument("game_dir", nargs="?", help="Game root folder or directory containing steam_api library")
    parser.add_argument("--status", action="store_true", help="Check if proxy is deployed")
    parser.add_argument("--restore", action="store_true", help="Restore original steam_api library")

    args = parser.parse_args()
    script_dir = Path(__file__).parent.resolve()

    if args.game_dir:
        args.game_dir = Path.absolute(Path(args.game_dir))
        dest, proxy_name = find_steam_api_dir(args.game_dir)
        if not dest:
            error(f"No steam_api library found in {args.game_dir} (searched recursively)")
        info(f"Found {proxy_name} in: {dest}")
    else:
        dest, proxy_name = auto_detect_game()
        if not dest:
            error("Could not find steam_api library. Specify the game directory: deploy.py /path/to/game")
        info(f"Auto-detected {proxy_name} in: {dest}")

    platform = detect_platform(proxy_name)
    original_name = LIBNAME_LINUX if platform == PLATFORM_LINUX else LIBNAME_WINDOWS

    if args.status:
        do_status(dest, proxy_name, original_name, platform)
    elif args.restore:
        do_restore(dest, proxy_name, original_name, platform)
    else:
        do_deploy(dest, proxy_name, original_name, platform)


if __name__ == "__main__":
    main()
