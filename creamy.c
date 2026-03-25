/*
 * CreamySteamyLinux - Pure C Steam DLC Unlocker for Linux
 *
 * Hooks Steam API functions via LD_PRELOAD to report configured DLCs as owned.
 * Works with native Linux Steam games that use libsteam_api.so.
 *
 * Build: gcc -shared -fPIC -o lib64CreamySteamy.so creamy.c -ldl
 *
 * MIT License
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

/* ========================================================================= */
/* Logging                                                                    */
/* ========================================================================= */

static FILE *g_logfile = NULL;
static int g_log_enabled = -1; /* -1 = not checked yet */

static void log_init(void) {
    if (g_log_enabled != -1) return;
    const char *env = getenv("CREAMY_LOG");
    if (env && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')) {
        g_log_enabled = 1;
        g_logfile = fopen("creamy_log.txt", "w");
        if (!g_logfile) g_log_enabled = 0;
    } else {
        g_log_enabled = 0;
    }
}

#define LOG(...) do { \
    log_init(); \
    if (g_log_enabled && g_logfile) { \
        fprintf(g_logfile, "[CreamySteamy] "); \
        fprintf(g_logfile, __VA_ARGS__); \
        fprintf(g_logfile, "\n"); \
        fflush(g_logfile); \
    } \
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
static bool g_issubscribedapp_use_real = false;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

static void load_config(void) {
    if (g_config_loaded) return;
    g_config_loaded = true;

    const char *path = getenv("CREAM_CONFIG_PATH");
    if (!path) path = "cream_api.ini";

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG("ERROR: Cannot open config file: %s", path);
        return;
    }

    LOG("Loading config from: %s", path);

    char line[1024];
    int in_dlc_section = 0;
    int in_config_section = 0;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (s[0] == '#' || s[0] == ';' || s[0] == '\0') continue;

        if (s[0] == '[') {
            in_dlc_section = (strncasecmp(s, "[dlc]", 5) == 0);
            in_config_section = (strncasecmp(s, "[config]", 8) == 0);
            continue;
        }

        if (in_config_section) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                char *key = trim(s);
                char *val = trim(eq + 1);
                if (strcasecmp(key, "issubscribedapp_on_false_use_real") == 0) {
                    g_issubscribedapp_use_real = (strcasecmp(val, "true") == 0);
                    LOG("Config: issubscribedapp_on_false_use_real = %s", val);
                }
            }
        }

        if (in_dlc_section && g_dlc_count < MAX_DLCS) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                char *id_str = trim(s);
                char *name = trim(eq + 1);
                uint32_t id = (uint32_t)strtoul(id_str, NULL, 10);
                if (id > 0) {
                    g_dlcs[g_dlc_count].app_id = id;
                    strncpy(g_dlcs[g_dlc_count].name, name, sizeof(g_dlcs[0].name) - 1);
                    g_dlcs[g_dlc_count].name[sizeof(g_dlcs[0].name) - 1] = '\0';
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
/* Real dlsym via dlvsym                                                      */
/* ========================================================================= */

typedef void *(*dlsym_fn)(void *handle, const char *name);
static dlsym_fn real_dlsym_ptr = NULL;

static void ensure_real_dlsym(void) {
    if (!real_dlsym_ptr) {
        real_dlsym_ptr = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
}

/* ========================================================================= */
/* Steam type definitions (C-compatible)                                      */
/* ========================================================================= */

typedef uint32_t AppId_t;
typedef uint32_t DepotId_t;
typedef int32_t HSteamUser;
typedef int32_t HSteamPipe;
typedef uint64_t SteamAPICall_t;
typedef uint64_t CSteamID_flat;

/*
 * Steam interfaces use C++ vtables. In memory, a C++ object with virtual
 * functions is just a pointer to a vtable (array of function pointers).
 *
 * For ISteamApps, the vtable layout is:
 *   [0]  BIsSubscribed
 *   [1]  BIsLowViolence
 *   [2]  BIsCybercafe
 *   [3]  BIsVACBanned
 *   [4]  GetCurrentGameLanguage
 *   [5]  GetAvailableGameLanguages
 *   [6]  BIsSubscribedApp
 *   [7]  BIsDlcInstalled
 *   [8]  GetEarliestPurchaseUnixTime
 *   [9]  BIsSubscribedFromFreeWeekend
 *   [10] GetDLCCount
 *   [11] BGetDLCDataByIndex
 *   ...
 *
 * We create a fake vtable that overrides the DLC-related entries
 * and forwards everything else to the real implementation.
 */

/* A Steam interface object is just: struct { void **vtable; } */
typedef struct {
    void **vtable;
} SteamInterface;

/* ========================================================================= */
/* ISteamApps hook via vtable patching                                        */
/* ========================================================================= */

/* ISteamApps vtable indices */
#define VTIDX_BIsSubscribed             0
#define VTIDX_BIsLowViolence            1
#define VTIDX_BIsCybercafe              2
#define VTIDX_BIsVACBanned              3
#define VTIDX_GetCurrentGameLanguage    4
#define VTIDX_GetAvailableGameLanguages 5
#define VTIDX_BIsSubscribedApp          6
#define VTIDX_BIsDlcInstalled           7
#define VTIDX_GetEarliestPurchaseUnixTime 8
#define VTIDX_BIsSubscribedFromFreeWeekend 9
#define VTIDX_GetDLCCount               10
#define VTIDX_BGetDLCDataByIndex        11
/* ... more entries follow but we don't need to override them */

/* We need enough vtable slots to cover all ISteamApps methods */
#define STEAMAPPS_VTABLE_SIZE 32

static void *g_real_steamapps = NULL;
static void **g_real_steamapps_vtable = NULL;
static void *g_fake_steamapps_vtable[STEAMAPPS_VTABLE_SIZE];
static SteamInterface g_fake_steamapps_obj;
static bool g_steamapps_hooked = false;

/* Hook functions — first arg is 'this' pointer (the interface object) */
typedef bool (*fn_bool_this)(void *self);
typedef bool (*fn_bool_this_u32)(void *self, uint32_t);
typedef int (*fn_int_this)(void *self);
typedef bool (*fn_bgetdlcdata)(void *self, int iDLC, uint32_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize);

static bool hook_BIsSubscribed(void *self) {
    LOG("ISteamApps::BIsSubscribed -> true");
    return true;
}

static int hook_GetDLCCount(void *self) {
    LOG("ISteamApps::GetDLCCount -> %d", g_dlc_count);
    return g_dlc_count;
}

static bool hook_BIsDlcInstalled(void *self, uint32_t appID) {
    bool owned = is_dlc_owned(appID);
    LOG("ISteamApps::BIsDlcInstalled(%u) -> %s", appID, owned ? "true" : "false");
    return owned;
}

static bool hook_BIsSubscribedApp(void *self, uint32_t appID) {
    bool owned = is_dlc_owned(appID);
    if (owned) {
        LOG("ISteamApps::BIsSubscribedApp(%u) -> true (unlocked)", appID);
        return true;
    }
    if (g_issubscribedapp_use_real && g_real_steamapps_vtable) {
        fn_bool_this_u32 real_fn = (fn_bool_this_u32)g_real_steamapps_vtable[VTIDX_BIsSubscribedApp];
        bool result = real_fn(g_real_steamapps, appID);
        LOG("ISteamApps::BIsSubscribedApp(%u) -> %s (real)", appID, result ? "true" : "false");
        return result;
    }
    LOG("ISteamApps::BIsSubscribedApp(%u) -> false", appID);
    return false;
}

static bool hook_BGetDLCDataByIndex(void *self, int iDLC, uint32_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize) {
    if (iDLC < 0 || iDLC >= g_dlc_count) {
        LOG("ISteamApps::BGetDLCDataByIndex(%d) -> false (out of range)", iDLC);
        return false;
    }
    *pAppID = g_dlcs[iDLC].app_id;
    *pbAvailable = true;
    size_t slen = strlen(g_dlcs[iDLC].name);
    if (slen >= (size_t)cchNameBufferSize) slen = cchNameBufferSize - 1;
    memcpy(pchName, g_dlcs[iDLC].name, slen);
    pchName[slen] = '\0';
    LOG("ISteamApps::BGetDLCDataByIndex(%d) -> %u '%s'", iDLC, g_dlcs[iDLC].app_id, g_dlcs[iDLC].name);
    return true;
}

static void *hook_steamapps(void *real_apps) {
    if (!real_apps) return real_apps;
    if (g_steamapps_hooked) return &g_fake_steamapps_obj;

    load_config();

    g_real_steamapps = real_apps;
    g_real_steamapps_vtable = *(void ***)real_apps;

    /* Copy real vtable */
    for (int i = 0; i < STEAMAPPS_VTABLE_SIZE; i++) {
        g_fake_steamapps_vtable[i] = g_real_steamapps_vtable[i];
    }

    /* Override DLC-related entries */
    g_fake_steamapps_vtable[VTIDX_BIsSubscribed] = (void *)hook_BIsSubscribed;
    g_fake_steamapps_vtable[VTIDX_GetDLCCount] = (void *)hook_GetDLCCount;
    g_fake_steamapps_vtable[VTIDX_BIsDlcInstalled] = (void *)hook_BIsDlcInstalled;
    g_fake_steamapps_vtable[VTIDX_BIsSubscribedApp] = (void *)hook_BIsSubscribedApp;
    g_fake_steamapps_vtable[VTIDX_BGetDLCDataByIndex] = (void *)hook_BGetDLCDataByIndex;

    g_fake_steamapps_obj.vtable = g_fake_steamapps_vtable;
    g_steamapps_hooked = true;

    LOG("ISteamApps hooked successfully (real=%p, fake=%p)", real_apps, &g_fake_steamapps_obj);
    return &g_fake_steamapps_obj;
}

/* ========================================================================= */
/* ISteamUser hook — UserHasLicenseForApp                                     */
/* ========================================================================= */

/*
 * ISteamUser vtable — we only need to hook UserHasLicenseForApp.
 * Vtable index for UserHasLicenseForApp:
 *   Counting from isteamuser.h virtual methods:
 *   [0]  GetHSteamUser
 *   [1]  BLoggedOn
 *   [2]  GetSteamID
 *   [3]  InitiateGameConnection_DEPRECATED
 *   [4]  TerminateGameConnection_DEPRECATED
 *   [5]  TrackAppUsageEvent
 *   [6]  GetUserDataFolder
 *   [7]  StartVoiceRecording
 *   [8]  StopVoiceRecording
 *   [9]  GetAvailableVoice
 *   [10] GetVoice
 *   [11] DecompressVoice
 *   [12] GetVoiceOptimalSampleRate
 *   [13] GetAuthSessionTicket
 *   [14] BeginAuthSession
 *   [15] EndAuthSession
 *   [16] CancelAuthTicket
 *   [17] UserHasLicenseForApp
 */

#define VTIDX_UserHasLicenseForApp 17
#define STEAMUSER_VTABLE_SIZE 40

static void *g_real_steamuser = NULL;
static void **g_real_steamuser_vtable = NULL;
static void *g_fake_steamuser_vtable[STEAMUSER_VTABLE_SIZE];
static SteamInterface g_fake_steamuser_obj;
static bool g_steamuser_hooked = false;

/* EUserHasLicenseForAppResult: 0 = HasLicense, 2 = DoesNotHaveLicense */
typedef int (*fn_userlicense)(void *self, uint64_t steamID, uint32_t appID);

static int hook_UserHasLicenseForApp(void *self, uint64_t steamID, uint32_t appID) {
    bool owned = is_dlc_owned(appID);
    if (owned) {
        LOG("ISteamUser::UserHasLicenseForApp(%u) -> HasLicense", appID);
        return 0; /* k_EUserHasLicenseResultHasLicense */
    }
    LOG("ISteamUser::UserHasLicenseForApp(%u) -> DoesNotHaveLicense", appID);
    return 2; /* k_EUserHasLicenseResultDoesNotHaveLicense */
}

static void *hook_steamuser(void *real_user) {
    if (!real_user) return real_user;
    if (g_steamuser_hooked) return &g_fake_steamuser_obj;

    load_config();

    g_real_steamuser = real_user;
    g_real_steamuser_vtable = *(void ***)real_user;

    for (int i = 0; i < STEAMUSER_VTABLE_SIZE; i++) {
        g_fake_steamuser_vtable[i] = g_real_steamuser_vtable[i];
    }

    g_fake_steamuser_vtable[VTIDX_UserHasLicenseForApp] = (void *)hook_UserHasLicenseForApp;

    g_fake_steamuser_obj.vtable = g_fake_steamuser_vtable;
    g_steamuser_hooked = true;

    LOG("ISteamUser hooked successfully");
    return &g_fake_steamuser_obj;
}

/* ========================================================================= */
/* Flat API hooks (SteamAPI_ISteamApps_*)                                     */
/* ========================================================================= */

bool SteamAPI_ISteamApps_BIsDlcInstalled(void *self, uint32_t appID) {
    load_config();
    bool owned = is_dlc_owned(appID);
    LOG("SteamAPI_ISteamApps_BIsDlcInstalled(%u) -> %s", appID, owned ? "true" : "false");
    return owned;
}

bool SteamAPI_ISteamApps_BIsSubscribedApp(void *self, uint32_t appID) {
    load_config();
    bool owned = is_dlc_owned(appID);
    LOG("SteamAPI_ISteamApps_BIsSubscribedApp(%u) -> %s", appID, owned ? "true" : "false");
    return owned;
}

int SteamAPI_ISteamApps_GetDLCCount(void *self) {
    load_config();
    LOG("SteamAPI_ISteamApps_GetDLCCount -> %d", g_dlc_count);
    return g_dlc_count;
}

bool SteamAPI_ISteamApps_BGetDLCDataByIndex(void *self, int iDLC, uint32_t *pAppID, bool *pbAvailable, char *pchName, int cchNameBufferSize) {
    load_config();
    if (iDLC < 0 || iDLC >= g_dlc_count) return false;
    *pAppID = g_dlcs[iDLC].app_id;
    *pbAvailable = true;
    size_t slen = strlen(g_dlcs[iDLC].name);
    if (slen >= (size_t)cchNameBufferSize) slen = cchNameBufferSize - 1;
    memcpy(pchName, g_dlcs[iDLC].name, slen);
    pchName[slen] = '\0';
    LOG("SteamAPI_ISteamApps_BGetDLCDataByIndex(%d) -> %u", iDLC, g_dlcs[iDLC].app_id);
    return true;
}

/* ========================================================================= */
/* Interface creation hooks                                                   */
/* ========================================================================= */

#define STEAMAPPS_IFACE_PREFIX "STEAMAPPS_INTERFACE_VERSION"
#define STEAMUSER_IFACE_PREFIX "SteamUser"

void *SteamInternal_FindOrCreateUserInterface(HSteamUser hSteamUser, const char *pszVersion) {
    ensure_real_dlsym();
    typedef void *(*real_fn_t)(HSteamUser, const char *);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamInternal_FindOrCreateUserInterface");
    void *result = real_fn(hSteamUser, pszVersion);
    LOG("SteamInternal_FindOrCreateUserInterface(%s) called", pszVersion);

    if (strstr(pszVersion, STEAMAPPS_IFACE_PREFIX) == pszVersion) {
        LOG("  -> hooking ISteamApps");
        return hook_steamapps(result);
    }
    if (strstr(pszVersion, STEAMUSER_IFACE_PREFIX) == pszVersion) {
        LOG("  -> hooking ISteamUser");
        return hook_steamuser(result);
    }
    return result;
}

void *SteamInternal_CreateInterface(const char *pszVersion) {
    ensure_real_dlsym();
    typedef void *(*real_fn_t)(const char *);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamInternal_CreateInterface");
    void *result = real_fn(pszVersion);
    LOG("SteamInternal_CreateInterface(%s) called", pszVersion);

    if (strstr(pszVersion, STEAMAPPS_IFACE_PREFIX) == pszVersion) {
        LOG("  -> hooking ISteamApps");
        return hook_steamapps(result);
    }
    if (strstr(pszVersion, STEAMUSER_IFACE_PREFIX) == pszVersion) {
        LOG("  -> hooking ISteamUser");
        return hook_steamuser(result);
    }
    return result;
}

/* Hook SteamApps() and SteamUser() — older API style */
void *SteamApps(void) {
    ensure_real_dlsym();
    typedef void *(*real_fn_t)(void);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamApps");
    void *result = real_fn();
    LOG("SteamApps() called");
    return hook_steamapps(result);
}

void *SteamUser(void) {
    ensure_real_dlsym();
    typedef void *(*real_fn_t)(void);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamUser");
    void *result = real_fn();
    LOG("SteamUser() called");
    return hook_steamuser(result);
}

/* ========================================================================= */
/* SteamAPI_Init hook — entry point, loads config                             */
/* ========================================================================= */

bool SteamAPI_Init(void) {
    ensure_real_dlsym();
    load_config();

    LOG("SteamAPI_Init called (PID %d)", getpid());

    typedef bool (*real_fn_t)(void);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamAPI_Init");
    if (!real_fn) {
        LOG("ERROR: Could not find real SteamAPI_Init");
        return false;
    }

    bool result = real_fn();
    LOG("Real SteamAPI_Init returned %d", result);
    return result;
}

/* Also hook SteamAPI_InitSafe for games that use it */
bool SteamAPI_InitSafe(void) {
    ensure_real_dlsym();
    load_config();

    LOG("SteamAPI_InitSafe called (PID %d)", getpid());

    typedef bool (*real_fn_t)(void);
    real_fn_t real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamAPI_InitSafe");
    if (!real_fn) {
        LOG("ERROR: Could not find real SteamAPI_InitSafe, trying SteamAPI_Init");
        real_fn = (real_fn_t)real_dlsym_ptr(RTLD_NEXT, "SteamAPI_Init");
        if (!real_fn) return false;
    }

    bool result = real_fn();
    LOG("Real SteamAPI_InitSafe returned %d", result);
    return result;
}

/* ========================================================================= */
/* dlsym hook — the master interceptor                                        */
/* ========================================================================= */

/*
 * This is the key hook. Many games (especially Unity/Mono) load Steam API
 * functions dynamically via dlsym(). By intercepting dlsym itself, we can
 * redirect any Steam API function lookup to our hooks.
 */
void *dlsym(void *handle, const char *name) {
    ensure_real_dlsym();

    if (!strcmp(name, "dlsym"))
        return (void *)dlsym;

    if (!strcmp(name, "SteamAPI_Init") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_Init");
        return (void *)SteamAPI_Init;
    }
    if (!strcmp(name, "SteamAPI_InitSafe") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_InitSafe");
        return (void *)SteamAPI_InitSafe;
    }
    if (!strcmp(name, "SteamInternal_CreateInterface") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamInternal_CreateInterface");
        return (void *)SteamInternal_CreateInterface;
    }
    if (!strcmp(name, "SteamInternal_FindOrCreateUserInterface") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamInternal_FindOrCreateUserInterface");
        return (void *)SteamInternal_FindOrCreateUserInterface;
    }
    if (!strcmp(name, "SteamApps") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamApps");
        return (void *)SteamApps;
    }
    if (!strcmp(name, "SteamUser") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamUser");
        return (void *)SteamUser;
    }

    /* Flat API hooks */
    if (!strcmp(name, "SteamAPI_ISteamApps_BIsDlcInstalled") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_ISteamApps_BIsDlcInstalled");
        return (void *)SteamAPI_ISteamApps_BIsDlcInstalled;
    }
    if (!strcmp(name, "SteamAPI_ISteamApps_BIsSubscribedApp") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_ISteamApps_BIsSubscribedApp");
        return (void *)SteamAPI_ISteamApps_BIsSubscribedApp;
    }
    if (!strcmp(name, "SteamAPI_ISteamApps_GetDLCCount") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_ISteamApps_GetDLCCount");
        return (void *)SteamAPI_ISteamApps_GetDLCCount;
    }
    if (!strcmp(name, "SteamAPI_ISteamApps_BGetDLCDataByIndex") && handle != RTLD_NEXT) {
        LOG("dlsym hook: redirecting SteamAPI_ISteamApps_BGetDLCDataByIndex");
        return (void *)SteamAPI_ISteamApps_BGetDLCDataByIndex;
    }

    return real_dlsym_ptr(handle, name);
}

/* ========================================================================= */
/* Anti-debugger bypass                                                       */
/* ========================================================================= */

long ptrace(int request, int pid, void *addr, void *data) {
    return 0;
}
