import sys

# Read forwarded functions
with open('/tmp/steam_api_forward.txt') as f:
    forward_fns = [line.strip() for line in f if line.strip()]

# Read DLC override functions  
override_fns = [
    "SteamAPI_ISteamApps_BIsDlcInstalled",
    "SteamAPI_ISteamApps_BIsSubscribedApp",
    "SteamAPI_ISteamApps_BIsSubscribed",
    "SteamAPI_ISteamApps_GetDLCCount",
    "SteamAPI_ISteamApps_BGetDLCDataByIndex",
    "SteamAPI_ISteamApps_BIsAppInstalled",
    "SteamAPI_ISteamUser_UserHasLicenseForApp",
    "SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime",
]

print("""/*
 * CreamySteamyLinux - Steam API Proxy for DLC Unlocking
 * 
 * This replaces libsteam_api.so. It loads the real library (libsteam_api_o.so)
 * and forwards all calls, overriding DLC-related functions.
 *
 * Build: gcc -shared -fPIC -o libsteam_api.so proxy.c -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* ========================================================================= */
/* Logging                                                                    */
/* ========================================================================= */

static FILE *g_logfile = NULL;
static int g_log_enabled = -1;

static void log_init(void) {
    if (g_log_enabled != -1) return;
    g_log_enabled = 1;
    const char *logpath = getenv("CREAMY_LOG_PATH");
    if (!logpath) {
        /* Write log next to the .so */
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-32);
        if (len > 0) {
            buf[len] = 0;
            char *slash = strrchr(buf, '/');
            if (slash) {
                strcpy(slash+1, "creamy_log.txt");
                logpath = buf;
            }
        }
        if (!logpath) logpath = "/tmp/creamy_log.txt";
    }
    g_logfile = fopen(logpath, "w");
    if (!g_logfile) { g_log_enabled = 0; return; }
    fprintf(g_logfile, "[CreamySteamy] Log started, PID=%d\\n", getpid());
    fflush(g_logfile);
}

#define LOG(...) do { \\
    log_init(); \\
    if (g_log_enabled && g_logfile) { \\
        fprintf(g_logfile, "[CreamySteamy] "); \\
        fprintf(g_logfile, __VA_ARGS__); \\
        fprintf(g_logfile, "\\n"); \\
        fflush(g_logfile); \\
    } \\
} while(0)

/* ========================================================================= */
/* DLC Configuration                                                          */
/* ========================================================================= */

#define MAX_DLCS 512

typedef struct {
    uint32_t app_id;
    char name[256];
} DlcEntry;

static DlcEntry g_dlcs[MAX_DLCS];
static int g_dlc_count = 0;
static bool g_config_loaded = false;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\\t' || *end == '\\n' || *end == '\\r'))
        *end-- = '\\0';
    return s;
}

static void find_config_path(char *out, size_t outsize) {
    /* Try next to the proxy library first */
    Dl_info info;
    if (dladdr((void*)find_config_path, &info) && info.dli_fname) {
        char buf[4096];
        strncpy(buf, info.dli_fname, sizeof(buf)-1);
        buf[sizeof(buf)-1] = 0;
        char *slash = strrchr(buf, '/');
        if (slash) {
            strcpy(slash+1, "cream_api.ini");
            if (access(buf, R_OK) == 0) {
                strncpy(out, buf, outsize);
                return;
            }
            /* Try two dirs up (game root) */
            *slash = 0;
            slash = strrchr(buf, '/');
            if (slash) {
                *slash = 0;
                slash = strrchr(buf, '/');
                if (slash) {
                    strcpy(slash+1, "cream_api.ini");
                    if (access(buf, R_OK) == 0) {
                        strncpy(out, buf, outsize);
                        return;
                    }
                }
            }
        }
    }
    /* Try CWD */
    if (access("cream_api.ini", R_OK) == 0) {
        strncpy(out, "cream_api.ini", outsize);
        return;
    }
    /* Try env */
    const char *env = getenv("CREAM_CONFIG_PATH");
    if (env) { strncpy(out, env, outsize); return; }
    out[0] = 0;
}

static void load_config(void) {
    if (g_config_loaded) return;
    g_config_loaded = true;

    char path[4096] = {0};
    const char *env = getenv("CREAM_CONFIG_PATH");
    if (env) {
        strncpy(path, env, sizeof(path)-1);
    } else {
        find_config_path(path, sizeof(path));
    }

    if (!path[0]) {
        LOG("ERROR: Cannot find cream_api.ini");
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG("ERROR: Cannot open config file: %s", path);
        return;
    }

    LOG("Loading config from: %s", path);

    char line[1024];
    int in_dlc_section = 0;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (s[0] == '#' || s[0] == ';' || s[0] == '\\0') continue;

        if (s[0] == '[') {
            in_dlc_section = (strncasecmp(s, "[dlc]", 5) == 0);
            continue;
        }

        if (in_dlc_section && g_dlc_count < MAX_DLCS) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\\0';
                char *id_str = trim(s);
                char *name = trim(eq + 1);
                uint32_t id = (uint32_t)strtoul(id_str, NULL, 10);
                if (id > 0) {
                    g_dlcs[g_dlc_count].app_id = id;
                    strncpy(g_dlcs[g_dlc_count].name, name, sizeof(g_dlcs[0].name) - 1);
                    g_dlcs[g_dlc_count].name[sizeof(g_dlcs[0].name) - 1] = '\\0';
                    LOG("Added DLC: %u = %s", id, name);
                    g_dlc_count++;
                }
            }
        }
    }

    fclose(f);
    LOG("Loaded %d DLCs", g_dlc_count);
}

static bool is_dlc_owned(uint32_t app_id) {
    for (int i = 0; i < g_dlc_count; i++) {
        if (g_dlcs[i].app_id == app_id) return true;
    }
    return false;
}

/* ========================================================================= */
/* Real library handle                                                        */
/* ========================================================================= */

static void *g_real_lib = NULL;

static void ensure_real_lib(void) {
    if (g_real_lib) return;
    
    /* Find the real library - renamed to libsteam_api_o.so */
    Dl_info info;
    char real_path[4096] = {0};
    
    if (dladdr((void*)ensure_real_lib, &info) && info.dli_fname) {
        strncpy(real_path, info.dli_fname, sizeof(real_path)-1);
        char *slash = strrchr(real_path, '/');
        if (slash) {
            strcpy(slash+1, "libsteam_api_o.so");
        }
    }
    
    if (real_path[0]) {
        g_real_lib = dlopen(real_path, RTLD_NOW | RTLD_LOCAL);
    }
    
    if (!g_real_lib) {
        /* Fallback: try relative */
        g_real_lib = dlopen("./libsteam_api_o.so", RTLD_NOW | RTLD_LOCAL);
    }
    
    if (!g_real_lib) {
        LOG("FATAL: Cannot load real libsteam_api_o.so: %s", dlerror());
    } else {
        LOG("Loaded real library: %s", real_path);
    }
}

static void *get_real_fn(const char *name) {
    ensure_real_lib();
    if (!g_real_lib) return NULL;
    return dlsym(g_real_lib, name);
}

/* ========================================================================= */
/* DLC Override Functions                                                      */
/* ========================================================================= */

typedef uint32_t AppId_t;
typedef uint64_t CSteamID_flat;

bool SteamAPI_ISteamApps_BIsDlcInstalled(void *self, AppId_t appID) {
    load_config();
    if (is_dlc_owned(appID)) {
        LOG("BIsDlcInstalled(%u) -> true (UNLOCKED)", appID);
        return true;
    }
    typedef bool (*fn_t)(void*, AppId_t);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamApps_BIsDlcInstalled");
    bool r = real ? real(self, appID) : false;
    LOG("BIsDlcInstalled(%u) -> %s (real)", appID, r ? "true" : "false");
    return r;
}

bool SteamAPI_ISteamApps_BIsSubscribedApp(void *self, AppId_t appID) {
    load_config();
    if (is_dlc_owned(appID)) {
        LOG("BIsSubscribedApp(%u) -> true (UNLOCKED)", appID);
        return true;
    }
    typedef bool (*fn_t)(void*, AppId_t);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamApps_BIsSubscribedApp");
    bool r = real ? real(self, appID) : false;
    LOG("BIsSubscribedApp(%u) -> %s (real)", appID, r ? "true" : "false");
    return r;
}

bool SteamAPI_ISteamApps_BIsSubscribed(void *self) {
    LOG("BIsSubscribed -> true");
    return true;
}

int SteamAPI_ISteamApps_GetDLCCount(void *self) {
    load_config();
    LOG("GetDLCCount -> %d", g_dlc_count);
    return g_dlc_count;
}

bool SteamAPI_ISteamApps_BGetDLCDataByIndex(void *self, int iDLC, AppId_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize) {
    load_config();
    if (iDLC >= 0 && iDLC < g_dlc_count) {
        *pAppID = g_dlcs[iDLC].app_id;
        *pbAvailable = true;
        strncpy(pchName, g_dlcs[iDLC].name, cchNameBufferSize - 1);
        pchName[cchNameBufferSize - 1] = '\\0';
        LOG("BGetDLCDataByIndex(%d) -> %u '%s'", iDLC, g_dlcs[iDLC].app_id, g_dlcs[iDLC].name);
        return true;
    }
    typedef bool (*fn_t)(void*, int, AppId_t*, bool*, char*, int);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamApps_BGetDLCDataByIndex");
    return real ? real(self, iDLC, pAppID, pbAvailable, pchName, cchNameBufferSize) : false;
}

bool SteamAPI_ISteamApps_BIsAppInstalled(void *self, AppId_t appID) {
    load_config();
    if (is_dlc_owned(appID)) {
        LOG("BIsAppInstalled(%u) -> true (UNLOCKED)", appID);
        return true;
    }
    typedef bool (*fn_t)(void*, AppId_t);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamApps_BIsAppInstalled");
    return real ? real(self, appID) : false;
}

uint32_t SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime(void *self, AppId_t appID) {
    load_config();
    if (is_dlc_owned(appID)) {
        LOG("GetEarliestPurchaseUnixTime(%u) -> 1577836800 (UNLOCKED)", appID);
        return 1577836800; /* 2020-01-01 */
    }
    typedef uint32_t (*fn_t)(void*, AppId_t);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamApps_GetEarliestPurchaseUnixTime");
    return real ? real(self, appID) : 0;
}

int SteamAPI_ISteamUser_UserHasLicenseForApp(void *self, CSteamID_flat steamID, AppId_t appID) {
    load_config();
    if (is_dlc_owned(appID)) {
        LOG("UserHasLicenseForApp(%u) -> HasLicense (UNLOCKED)", appID);
        return 0; /* k_EUserHasLicenseResultHasLicense */
    }
    typedef int (*fn_t)(void*, CSteamID_flat, AppId_t);
    fn_t real = (fn_t)get_real_fn("SteamAPI_ISteamUser_UserHasLicenseForApp");
    return real ? real(self, steamID, appID) : 2;
}
""")

# Now generate forwarding stubs for all other functions
print("/* ========================================================================= */")
print("/* Auto-generated forwarding stubs                                            */")
print("/* ========================================================================= */")
print()
print("/* These use a generic variadic forwarding approach via function pointers */")
print()

for fn in forward_fns:
    # Use assembly-level forwarding via a trampoline
    print(f'/* {fn} */')
    print(f'__asm__(".globl {fn}");')
    print(f'__asm__("{fn}: jmp *_fwd_{fn}(%rip)");')
    print(f'static void *_fwd_{fn} = NULL;')
    print()

# Constructor to resolve all forwarded symbols
print("""
/* ========================================================================= */
/* Constructor - resolve all forwarded symbols at load time                    */
/* ========================================================================= */

__attribute__((constructor))
static void creamy_init(void) {
    log_init();
    ensure_real_lib();
    load_config();
    
    if (!g_real_lib) {
        LOG("FATAL: No real library loaded, forwarding will fail!");
        return;
    }
    
    LOG("Resolving forwarded symbols...");
""")

for fn in forward_fns:
    print(f'    _fwd_{fn} = dlsym(g_real_lib, "{fn}");')

print("""
    LOG("All symbols resolved. CreamySteamy proxy active with %d DLCs.", g_dlc_count);
}
""")

