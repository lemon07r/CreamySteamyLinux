/*
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
    fprintf(g_logfile, "[CreamySteamy] Log started, PID=%d\n", getpid());
    fflush(g_logfile);
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

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
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
            char *slash2 = strrchr(buf, '/');
            if (slash2) {
                *slash2 = 0;
                char *slash3 = strrchr(buf, '/');
                if (slash3) {
                    strcpy(slash3+1, "cream_api.ini");
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
        if (s[0] == '#' || s[0] == ';' || s[0] == '\0') continue;

        if (s[0] == '[') {
            in_dlc_section = (strncasecmp(s, "[dlc]", 5) == 0);
            continue;
        }

        if (in_dlc_section && g_dlc_count < MAX_DLCS) {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                const char *id_str = trim(s);
                const char *name = trim(eq + 1);
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
        if (cchNameBufferSize > 0) {
            strncpy(pchName, g_dlcs[iDLC].name, (size_t)(cchNameBufferSize - 1));
            pchName[cchNameBufferSize - 1] = '\0';
        }
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

/* ========================================================================= */
/* Auto-generated forwarding stubs                                            */
/* ========================================================================= */

/* These use a generic variadic forwarding approach via function pointers */

/* GetHSteamPipe */
__asm__(".globl GetHSteamPipe");
__asm__("GetHSteamPipe: jmp *_fwd_GetHSteamPipe(%rip)");
static void *_fwd_GetHSteamPipe = NULL;

/* GetHSteamUser */
__asm__(".globl GetHSteamUser");
__asm__("GetHSteamUser: jmp *_fwd_GetHSteamUser(%rip)");
static void *_fwd_GetHSteamUser = NULL;

/* SteamAPI_gameserveritem_t_Construct */
__asm__(".globl SteamAPI_gameserveritem_t_Construct");
__asm__("SteamAPI_gameserveritem_t_Construct: jmp *_fwd_SteamAPI_gameserveritem_t_Construct(%rip)");
static void *_fwd_SteamAPI_gameserveritem_t_Construct = NULL;

/* SteamAPI_gameserveritem_t_GetName */
__asm__(".globl SteamAPI_gameserveritem_t_GetName");
__asm__("SteamAPI_gameserveritem_t_GetName: jmp *_fwd_SteamAPI_gameserveritem_t_GetName(%rip)");
static void *_fwd_SteamAPI_gameserveritem_t_GetName = NULL;

/* SteamAPI_gameserveritem_t_SetName */
__asm__(".globl SteamAPI_gameserveritem_t_SetName");
__asm__("SteamAPI_gameserveritem_t_SetName: jmp *_fwd_SteamAPI_gameserveritem_t_SetName(%rip)");
static void *_fwd_SteamAPI_gameserveritem_t_SetName = NULL;

/* SteamAPI_GetHSteamPipe */
__asm__(".globl SteamAPI_GetHSteamPipe");
__asm__("SteamAPI_GetHSteamPipe: jmp *_fwd_SteamAPI_GetHSteamPipe(%rip)");
static void *_fwd_SteamAPI_GetHSteamPipe = NULL;

/* SteamAPI_GetHSteamUser */
__asm__(".globl SteamAPI_GetHSteamUser");
__asm__("SteamAPI_GetHSteamUser: jmp *_fwd_SteamAPI_GetHSteamUser(%rip)");
static void *_fwd_SteamAPI_GetHSteamUser = NULL;

/* SteamAPI_GetSteamInstallPath */
__asm__(".globl SteamAPI_GetSteamInstallPath");
__asm__("SteamAPI_GetSteamInstallPath: jmp *_fwd_SteamAPI_GetSteamInstallPath(%rip)");
static void *_fwd_SteamAPI_GetSteamInstallPath = NULL;

/* SteamAPI_InitAnonymousUser */
__asm__(".globl SteamAPI_InitAnonymousUser");
__asm__("SteamAPI_InitAnonymousUser: jmp *_fwd_SteamAPI_InitAnonymousUser(%rip)");
static void *_fwd_SteamAPI_InitAnonymousUser = NULL;

/* SteamAPI_InitFlat */
__asm__(".globl SteamAPI_InitFlat");
__asm__("SteamAPI_InitFlat: jmp *_fwd_SteamAPI_InitFlat(%rip)");
static void *_fwd_SteamAPI_InitFlat = NULL;

/* SteamAPI_InitSafe */
__asm__(".globl SteamAPI_InitSafe");
__asm__("SteamAPI_InitSafe: jmp *_fwd_SteamAPI_InitSafe(%rip)");
static void *_fwd_SteamAPI_InitSafe = NULL;

/* SteamAPI_IsSteamRunning */
__asm__(".globl SteamAPI_IsSteamRunning");
__asm__("SteamAPI_IsSteamRunning: jmp *_fwd_SteamAPI_IsSteamRunning(%rip)");
static void *_fwd_SteamAPI_IsSteamRunning = NULL;

/* SteamAPI_ISteamApps_BIsCybercafe */
__asm__(".globl SteamAPI_ISteamApps_BIsCybercafe");
__asm__("SteamAPI_ISteamApps_BIsCybercafe: jmp *_fwd_SteamAPI_ISteamApps_BIsCybercafe(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsCybercafe = NULL;

/* SteamAPI_ISteamApps_BIsLowViolence */
__asm__(".globl SteamAPI_ISteamApps_BIsLowViolence");
__asm__("SteamAPI_ISteamApps_BIsLowViolence: jmp *_fwd_SteamAPI_ISteamApps_BIsLowViolence(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsLowViolence = NULL;

/* SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing */
__asm__(".globl SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing");
__asm__("SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing: jmp *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing = NULL;

/* SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend */
__asm__(".globl SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend");
__asm__("SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend: jmp *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend = NULL;

/* SteamAPI_ISteamApps_BIsTimedTrial */
__asm__(".globl SteamAPI_ISteamApps_BIsTimedTrial");
__asm__("SteamAPI_ISteamApps_BIsTimedTrial: jmp *_fwd_SteamAPI_ISteamApps_BIsTimedTrial(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsTimedTrial = NULL;

/* SteamAPI_ISteamApps_BIsVACBanned */
__asm__(".globl SteamAPI_ISteamApps_BIsVACBanned");
__asm__("SteamAPI_ISteamApps_BIsVACBanned: jmp *_fwd_SteamAPI_ISteamApps_BIsVACBanned(%rip)");
static void *_fwd_SteamAPI_ISteamApps_BIsVACBanned = NULL;

/* SteamAPI_ISteamApps_GetAppBuildId */
__asm__(".globl SteamAPI_ISteamApps_GetAppBuildId");
__asm__("SteamAPI_ISteamApps_GetAppBuildId: jmp *_fwd_SteamAPI_ISteamApps_GetAppBuildId(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetAppBuildId = NULL;

/* SteamAPI_ISteamApps_GetAppInstallDir */
__asm__(".globl SteamAPI_ISteamApps_GetAppInstallDir");
__asm__("SteamAPI_ISteamApps_GetAppInstallDir: jmp *_fwd_SteamAPI_ISteamApps_GetAppInstallDir(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetAppInstallDir = NULL;

/* SteamAPI_ISteamApps_GetAppOwner */
__asm__(".globl SteamAPI_ISteamApps_GetAppOwner");
__asm__("SteamAPI_ISteamApps_GetAppOwner: jmp *_fwd_SteamAPI_ISteamApps_GetAppOwner(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetAppOwner = NULL;

/* SteamAPI_ISteamApps_GetAvailableGameLanguages */
__asm__(".globl SteamAPI_ISteamApps_GetAvailableGameLanguages");
__asm__("SteamAPI_ISteamApps_GetAvailableGameLanguages: jmp *_fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages = NULL;

/* SteamAPI_ISteamApps_GetBetaInfo */
__asm__(".globl SteamAPI_ISteamApps_GetBetaInfo");
__asm__("SteamAPI_ISteamApps_GetBetaInfo: jmp *_fwd_SteamAPI_ISteamApps_GetBetaInfo(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetBetaInfo = NULL;

/* SteamAPI_ISteamApps_GetCurrentBetaName */
__asm__(".globl SteamAPI_ISteamApps_GetCurrentBetaName");
__asm__("SteamAPI_ISteamApps_GetCurrentBetaName: jmp *_fwd_SteamAPI_ISteamApps_GetCurrentBetaName(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetCurrentBetaName = NULL;

/* SteamAPI_ISteamApps_GetCurrentGameLanguage */
__asm__(".globl SteamAPI_ISteamApps_GetCurrentGameLanguage");
__asm__("SteamAPI_ISteamApps_GetCurrentGameLanguage: jmp *_fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage = NULL;

/* SteamAPI_ISteamApps_GetDlcDownloadProgress */
__asm__(".globl SteamAPI_ISteamApps_GetDlcDownloadProgress");
__asm__("SteamAPI_ISteamApps_GetDlcDownloadProgress: jmp *_fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress = NULL;

/* SteamAPI_ISteamApps_GetFileDetails */
__asm__(".globl SteamAPI_ISteamApps_GetFileDetails");
__asm__("SteamAPI_ISteamApps_GetFileDetails: jmp *_fwd_SteamAPI_ISteamApps_GetFileDetails(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetFileDetails = NULL;

/* SteamAPI_ISteamApps_GetInstalledDepots */
__asm__(".globl SteamAPI_ISteamApps_GetInstalledDepots");
__asm__("SteamAPI_ISteamApps_GetInstalledDepots: jmp *_fwd_SteamAPI_ISteamApps_GetInstalledDepots(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetInstalledDepots = NULL;

/* SteamAPI_ISteamApps_GetLaunchCommandLine */
__asm__(".globl SteamAPI_ISteamApps_GetLaunchCommandLine");
__asm__("SteamAPI_ISteamApps_GetLaunchCommandLine: jmp *_fwd_SteamAPI_ISteamApps_GetLaunchCommandLine(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetLaunchCommandLine = NULL;

/* SteamAPI_ISteamApps_GetLaunchQueryParam */
__asm__(".globl SteamAPI_ISteamApps_GetLaunchQueryParam");
__asm__("SteamAPI_ISteamApps_GetLaunchQueryParam: jmp *_fwd_SteamAPI_ISteamApps_GetLaunchQueryParam(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetLaunchQueryParam = NULL;

/* SteamAPI_ISteamApps_GetNumBetas */
__asm__(".globl SteamAPI_ISteamApps_GetNumBetas");
__asm__("SteamAPI_ISteamApps_GetNumBetas: jmp *_fwd_SteamAPI_ISteamApps_GetNumBetas(%rip)");
static void *_fwd_SteamAPI_ISteamApps_GetNumBetas = NULL;

/* SteamAPI_ISteamApps_InstallDLC */
__asm__(".globl SteamAPI_ISteamApps_InstallDLC");
__asm__("SteamAPI_ISteamApps_InstallDLC: jmp *_fwd_SteamAPI_ISteamApps_InstallDLC(%rip)");
static void *_fwd_SteamAPI_ISteamApps_InstallDLC = NULL;

/* SteamAPI_ISteamApps_MarkContentCorrupt */
__asm__(".globl SteamAPI_ISteamApps_MarkContentCorrupt");
__asm__("SteamAPI_ISteamApps_MarkContentCorrupt: jmp *_fwd_SteamAPI_ISteamApps_MarkContentCorrupt(%rip)");
static void *_fwd_SteamAPI_ISteamApps_MarkContentCorrupt = NULL;

/* SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys */
__asm__(".globl SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys");
__asm__("SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys: jmp *_fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys(%rip)");
static void *_fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys = NULL;

/* SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey */
__asm__(".globl SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey");
__asm__("SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey: jmp *_fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey(%rip)");
static void *_fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey = NULL;

/* SteamAPI_ISteamApps_SetActiveBeta */
__asm__(".globl SteamAPI_ISteamApps_SetActiveBeta");
__asm__("SteamAPI_ISteamApps_SetActiveBeta: jmp *_fwd_SteamAPI_ISteamApps_SetActiveBeta(%rip)");
static void *_fwd_SteamAPI_ISteamApps_SetActiveBeta = NULL;

/* SteamAPI_ISteamApps_SetDlcContext */
__asm__(".globl SteamAPI_ISteamApps_SetDlcContext");
__asm__("SteamAPI_ISteamApps_SetDlcContext: jmp *_fwd_SteamAPI_ISteamApps_SetDlcContext(%rip)");
static void *_fwd_SteamAPI_ISteamApps_SetDlcContext = NULL;

/* SteamAPI_ISteamApps_UninstallDLC */
__asm__(".globl SteamAPI_ISteamApps_UninstallDLC");
__asm__("SteamAPI_ISteamApps_UninstallDLC: jmp *_fwd_SteamAPI_ISteamApps_UninstallDLC(%rip)");
static void *_fwd_SteamAPI_ISteamApps_UninstallDLC = NULL;

/* SteamAPI_ISteamClient_BReleaseSteamPipe */
__asm__(".globl SteamAPI_ISteamClient_BReleaseSteamPipe");
__asm__("SteamAPI_ISteamClient_BReleaseSteamPipe: jmp *_fwd_SteamAPI_ISteamClient_BReleaseSteamPipe(%rip)");
static void *_fwd_SteamAPI_ISteamClient_BReleaseSteamPipe = NULL;

/* SteamAPI_ISteamClient_BShutdownIfAllPipesClosed */
__asm__(".globl SteamAPI_ISteamClient_BShutdownIfAllPipesClosed");
__asm__("SteamAPI_ISteamClient_BShutdownIfAllPipesClosed: jmp *_fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed(%rip)");
static void *_fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed = NULL;

/* SteamAPI_ISteamClient_ConnectToGlobalUser */
__asm__(".globl SteamAPI_ISteamClient_ConnectToGlobalUser");
__asm__("SteamAPI_ISteamClient_ConnectToGlobalUser: jmp *_fwd_SteamAPI_ISteamClient_ConnectToGlobalUser(%rip)");
static void *_fwd_SteamAPI_ISteamClient_ConnectToGlobalUser = NULL;

/* SteamAPI_ISteamClient_CreateLocalUser */
__asm__(".globl SteamAPI_ISteamClient_CreateLocalUser");
__asm__("SteamAPI_ISteamClient_CreateLocalUser: jmp *_fwd_SteamAPI_ISteamClient_CreateLocalUser(%rip)");
static void *_fwd_SteamAPI_ISteamClient_CreateLocalUser = NULL;

/* SteamAPI_ISteamClient_CreateSteamPipe */
__asm__(".globl SteamAPI_ISteamClient_CreateSteamPipe");
__asm__("SteamAPI_ISteamClient_CreateSteamPipe: jmp *_fwd_SteamAPI_ISteamClient_CreateSteamPipe(%rip)");
static void *_fwd_SteamAPI_ISteamClient_CreateSteamPipe = NULL;

/* SteamAPI_ISteamClient_GetIPCCallCount */
__asm__(".globl SteamAPI_ISteamClient_GetIPCCallCount");
__asm__("SteamAPI_ISteamClient_GetIPCCallCount: jmp *_fwd_SteamAPI_ISteamClient_GetIPCCallCount(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetIPCCallCount = NULL;

/* SteamAPI_ISteamClient_GetISteamApps */
__asm__(".globl SteamAPI_ISteamClient_GetISteamApps");
__asm__("SteamAPI_ISteamClient_GetISteamApps: jmp *_fwd_SteamAPI_ISteamClient_GetISteamApps(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamApps = NULL;

/* SteamAPI_ISteamClient_GetISteamController */
__asm__(".globl SteamAPI_ISteamClient_GetISteamController");
__asm__("SteamAPI_ISteamClient_GetISteamController: jmp *_fwd_SteamAPI_ISteamClient_GetISteamController(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamController = NULL;

/* SteamAPI_ISteamClient_GetISteamFriends */
__asm__(".globl SteamAPI_ISteamClient_GetISteamFriends");
__asm__("SteamAPI_ISteamClient_GetISteamFriends: jmp *_fwd_SteamAPI_ISteamClient_GetISteamFriends(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamFriends = NULL;

/* SteamAPI_ISteamClient_GetISteamGameSearch */
__asm__(".globl SteamAPI_ISteamClient_GetISteamGameSearch");
__asm__("SteamAPI_ISteamClient_GetISteamGameSearch: jmp *_fwd_SteamAPI_ISteamClient_GetISteamGameSearch(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameSearch = NULL;

/* SteamAPI_ISteamClient_GetISteamGameServer */
__asm__(".globl SteamAPI_ISteamClient_GetISteamGameServer");
__asm__("SteamAPI_ISteamClient_GetISteamGameServer: jmp *_fwd_SteamAPI_ISteamClient_GetISteamGameServer(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameServer = NULL;

/* SteamAPI_ISteamClient_GetISteamGameServerStats */
__asm__(".globl SteamAPI_ISteamClient_GetISteamGameServerStats");
__asm__("SteamAPI_ISteamClient_GetISteamGameServerStats: jmp *_fwd_SteamAPI_ISteamClient_GetISteamGameServerStats(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameServerStats = NULL;

/* SteamAPI_ISteamClient_GetISteamGenericInterface */
__asm__(".globl SteamAPI_ISteamClient_GetISteamGenericInterface");
__asm__("SteamAPI_ISteamClient_GetISteamGenericInterface: jmp *_fwd_SteamAPI_ISteamClient_GetISteamGenericInterface(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamGenericInterface = NULL;

/* SteamAPI_ISteamClient_GetISteamHTMLSurface */
__asm__(".globl SteamAPI_ISteamClient_GetISteamHTMLSurface");
__asm__("SteamAPI_ISteamClient_GetISteamHTMLSurface: jmp *_fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface = NULL;

/* SteamAPI_ISteamClient_GetISteamHTTP */
__asm__(".globl SteamAPI_ISteamClient_GetISteamHTTP");
__asm__("SteamAPI_ISteamClient_GetISteamHTTP: jmp *_fwd_SteamAPI_ISteamClient_GetISteamHTTP(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamHTTP = NULL;

/* SteamAPI_ISteamClient_GetISteamInput */
__asm__(".globl SteamAPI_ISteamClient_GetISteamInput");
__asm__("SteamAPI_ISteamClient_GetISteamInput: jmp *_fwd_SteamAPI_ISteamClient_GetISteamInput(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamInput = NULL;

/* SteamAPI_ISteamClient_GetISteamInventory */
__asm__(".globl SteamAPI_ISteamClient_GetISteamInventory");
__asm__("SteamAPI_ISteamClient_GetISteamInventory: jmp *_fwd_SteamAPI_ISteamClient_GetISteamInventory(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamInventory = NULL;

/* SteamAPI_ISteamClient_GetISteamMatchmaking */
__asm__(".globl SteamAPI_ISteamClient_GetISteamMatchmaking");
__asm__("SteamAPI_ISteamClient_GetISteamMatchmaking: jmp *_fwd_SteamAPI_ISteamClient_GetISteamMatchmaking(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamMatchmaking = NULL;

/* SteamAPI_ISteamClient_GetISteamMatchmakingServers */
__asm__(".globl SteamAPI_ISteamClient_GetISteamMatchmakingServers");
__asm__("SteamAPI_ISteamClient_GetISteamMatchmakingServers: jmp *_fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers = NULL;

/* SteamAPI_ISteamClient_GetISteamMusic */
__asm__(".globl SteamAPI_ISteamClient_GetISteamMusic");
__asm__("SteamAPI_ISteamClient_GetISteamMusic: jmp *_fwd_SteamAPI_ISteamClient_GetISteamMusic(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamMusic = NULL;

/* SteamAPI_ISteamClient_GetISteamMusicRemote */
__asm__(".globl SteamAPI_ISteamClient_GetISteamMusicRemote");
__asm__("SteamAPI_ISteamClient_GetISteamMusicRemote: jmp *_fwd_SteamAPI_ISteamClient_GetISteamMusicRemote(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamMusicRemote = NULL;

/* SteamAPI_ISteamClient_GetISteamNetworking */
__asm__(".globl SteamAPI_ISteamClient_GetISteamNetworking");
__asm__("SteamAPI_ISteamClient_GetISteamNetworking: jmp *_fwd_SteamAPI_ISteamClient_GetISteamNetworking(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamNetworking = NULL;

/* SteamAPI_ISteamClient_GetISteamParentalSettings */
__asm__(".globl SteamAPI_ISteamClient_GetISteamParentalSettings");
__asm__("SteamAPI_ISteamClient_GetISteamParentalSettings: jmp *_fwd_SteamAPI_ISteamClient_GetISteamParentalSettings(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamParentalSettings = NULL;

/* SteamAPI_ISteamClient_GetISteamParties */
__asm__(".globl SteamAPI_ISteamClient_GetISteamParties");
__asm__("SteamAPI_ISteamClient_GetISteamParties: jmp *_fwd_SteamAPI_ISteamClient_GetISteamParties(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamParties = NULL;

/* SteamAPI_ISteamClient_GetISteamRemotePlay */
__asm__(".globl SteamAPI_ISteamClient_GetISteamRemotePlay");
__asm__("SteamAPI_ISteamClient_GetISteamRemotePlay: jmp *_fwd_SteamAPI_ISteamClient_GetISteamRemotePlay(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamRemotePlay = NULL;

/* SteamAPI_ISteamClient_GetISteamRemoteStorage */
__asm__(".globl SteamAPI_ISteamClient_GetISteamRemoteStorage");
__asm__("SteamAPI_ISteamClient_GetISteamRemoteStorage: jmp *_fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage = NULL;

/* SteamAPI_ISteamClient_GetISteamScreenshots */
__asm__(".globl SteamAPI_ISteamClient_GetISteamScreenshots");
__asm__("SteamAPI_ISteamClient_GetISteamScreenshots: jmp *_fwd_SteamAPI_ISteamClient_GetISteamScreenshots(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamScreenshots = NULL;

/* SteamAPI_ISteamClient_GetISteamUGC */
__asm__(".globl SteamAPI_ISteamClient_GetISteamUGC");
__asm__("SteamAPI_ISteamClient_GetISteamUGC: jmp *_fwd_SteamAPI_ISteamClient_GetISteamUGC(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamUGC = NULL;

/* SteamAPI_ISteamClient_GetISteamUser */
__asm__(".globl SteamAPI_ISteamClient_GetISteamUser");
__asm__("SteamAPI_ISteamClient_GetISteamUser: jmp *_fwd_SteamAPI_ISteamClient_GetISteamUser(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamUser = NULL;

/* SteamAPI_ISteamClient_GetISteamUserStats */
__asm__(".globl SteamAPI_ISteamClient_GetISteamUserStats");
__asm__("SteamAPI_ISteamClient_GetISteamUserStats: jmp *_fwd_SteamAPI_ISteamClient_GetISteamUserStats(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamUserStats = NULL;

/* SteamAPI_ISteamClient_GetISteamUtils */
__asm__(".globl SteamAPI_ISteamClient_GetISteamUtils");
__asm__("SteamAPI_ISteamClient_GetISteamUtils: jmp *_fwd_SteamAPI_ISteamClient_GetISteamUtils(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamUtils = NULL;

/* SteamAPI_ISteamClient_GetISteamVideo */
__asm__(".globl SteamAPI_ISteamClient_GetISteamVideo");
__asm__("SteamAPI_ISteamClient_GetISteamVideo: jmp *_fwd_SteamAPI_ISteamClient_GetISteamVideo(%rip)");
static void *_fwd_SteamAPI_ISteamClient_GetISteamVideo = NULL;

/* SteamAPI_ISteamClient_ReleaseUser */
__asm__(".globl SteamAPI_ISteamClient_ReleaseUser");
__asm__("SteamAPI_ISteamClient_ReleaseUser: jmp *_fwd_SteamAPI_ISteamClient_ReleaseUser(%rip)");
static void *_fwd_SteamAPI_ISteamClient_ReleaseUser = NULL;

/* SteamAPI_ISteamClient_SetLocalIPBinding */
__asm__(".globl SteamAPI_ISteamClient_SetLocalIPBinding");
__asm__("SteamAPI_ISteamClient_SetLocalIPBinding: jmp *_fwd_SteamAPI_ISteamClient_SetLocalIPBinding(%rip)");
static void *_fwd_SteamAPI_ISteamClient_SetLocalIPBinding = NULL;

/* SteamAPI_ISteamClient_SetWarningMessageHook */
__asm__(".globl SteamAPI_ISteamClient_SetWarningMessageHook");
__asm__("SteamAPI_ISteamClient_SetWarningMessageHook: jmp *_fwd_SteamAPI_ISteamClient_SetWarningMessageHook(%rip)");
static void *_fwd_SteamAPI_ISteamClient_SetWarningMessageHook = NULL;

/* SteamAPI_ISteamController_ActivateActionSet */
__asm__(".globl SteamAPI_ISteamController_ActivateActionSet");
__asm__("SteamAPI_ISteamController_ActivateActionSet: jmp *_fwd_SteamAPI_ISteamController_ActivateActionSet(%rip)");
static void *_fwd_SteamAPI_ISteamController_ActivateActionSet = NULL;

/* SteamAPI_ISteamController_ActivateActionSetLayer */
__asm__(".globl SteamAPI_ISteamController_ActivateActionSetLayer");
__asm__("SteamAPI_ISteamController_ActivateActionSetLayer: jmp *_fwd_SteamAPI_ISteamController_ActivateActionSetLayer(%rip)");
static void *_fwd_SteamAPI_ISteamController_ActivateActionSetLayer = NULL;

/* SteamAPI_ISteamController_DeactivateActionSetLayer */
__asm__(".globl SteamAPI_ISteamController_DeactivateActionSetLayer");
__asm__("SteamAPI_ISteamController_DeactivateActionSetLayer: jmp *_fwd_SteamAPI_ISteamController_DeactivateActionSetLayer(%rip)");
static void *_fwd_SteamAPI_ISteamController_DeactivateActionSetLayer = NULL;

/* SteamAPI_ISteamController_DeactivateAllActionSetLayers */
__asm__(".globl SteamAPI_ISteamController_DeactivateAllActionSetLayers");
__asm__("SteamAPI_ISteamController_DeactivateAllActionSetLayers: jmp *_fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers(%rip)");
static void *_fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers = NULL;

/* SteamAPI_ISteamController_GetActionOriginFromXboxOrigin */
__asm__(".globl SteamAPI_ISteamController_GetActionOriginFromXboxOrigin");
__asm__("SteamAPI_ISteamController_GetActionOriginFromXboxOrigin: jmp *_fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin = NULL;

/* SteamAPI_ISteamController_GetActionSetHandle */
__asm__(".globl SteamAPI_ISteamController_GetActionSetHandle");
__asm__("SteamAPI_ISteamController_GetActionSetHandle: jmp *_fwd_SteamAPI_ISteamController_GetActionSetHandle(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetActionSetHandle = NULL;

/* SteamAPI_ISteamController_GetActiveActionSetLayers */
__asm__(".globl SteamAPI_ISteamController_GetActiveActionSetLayers");
__asm__("SteamAPI_ISteamController_GetActiveActionSetLayers: jmp *_fwd_SteamAPI_ISteamController_GetActiveActionSetLayers(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetActiveActionSetLayers = NULL;

/* SteamAPI_ISteamController_GetAnalogActionData */
__asm__(".globl SteamAPI_ISteamController_GetAnalogActionData");
__asm__("SteamAPI_ISteamController_GetAnalogActionData: jmp *_fwd_SteamAPI_ISteamController_GetAnalogActionData(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionData = NULL;

/* SteamAPI_ISteamController_GetAnalogActionHandle */
__asm__(".globl SteamAPI_ISteamController_GetAnalogActionHandle");
__asm__("SteamAPI_ISteamController_GetAnalogActionHandle: jmp *_fwd_SteamAPI_ISteamController_GetAnalogActionHandle(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionHandle = NULL;

/* SteamAPI_ISteamController_GetAnalogActionOrigins */
__asm__(".globl SteamAPI_ISteamController_GetAnalogActionOrigins");
__asm__("SteamAPI_ISteamController_GetAnalogActionOrigins: jmp *_fwd_SteamAPI_ISteamController_GetAnalogActionOrigins(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionOrigins = NULL;

/* SteamAPI_ISteamController_GetConnectedControllers */
__asm__(".globl SteamAPI_ISteamController_GetConnectedControllers");
__asm__("SteamAPI_ISteamController_GetConnectedControllers: jmp *_fwd_SteamAPI_ISteamController_GetConnectedControllers(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetConnectedControllers = NULL;

/* SteamAPI_ISteamController_GetControllerBindingRevision */
__asm__(".globl SteamAPI_ISteamController_GetControllerBindingRevision");
__asm__("SteamAPI_ISteamController_GetControllerBindingRevision: jmp *_fwd_SteamAPI_ISteamController_GetControllerBindingRevision(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetControllerBindingRevision = NULL;

/* SteamAPI_ISteamController_GetControllerForGamepadIndex */
__asm__(".globl SteamAPI_ISteamController_GetControllerForGamepadIndex");
__asm__("SteamAPI_ISteamController_GetControllerForGamepadIndex: jmp *_fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex = NULL;

/* SteamAPI_ISteamController_GetCurrentActionSet */
__asm__(".globl SteamAPI_ISteamController_GetCurrentActionSet");
__asm__("SteamAPI_ISteamController_GetCurrentActionSet: jmp *_fwd_SteamAPI_ISteamController_GetCurrentActionSet(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetCurrentActionSet = NULL;

/* SteamAPI_ISteamController_GetDigitalActionData */
__asm__(".globl SteamAPI_ISteamController_GetDigitalActionData");
__asm__("SteamAPI_ISteamController_GetDigitalActionData: jmp *_fwd_SteamAPI_ISteamController_GetDigitalActionData(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionData = NULL;

/* SteamAPI_ISteamController_GetDigitalActionHandle */
__asm__(".globl SteamAPI_ISteamController_GetDigitalActionHandle");
__asm__("SteamAPI_ISteamController_GetDigitalActionHandle: jmp *_fwd_SteamAPI_ISteamController_GetDigitalActionHandle(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionHandle = NULL;

/* SteamAPI_ISteamController_GetDigitalActionOrigins */
__asm__(".globl SteamAPI_ISteamController_GetDigitalActionOrigins");
__asm__("SteamAPI_ISteamController_GetDigitalActionOrigins: jmp *_fwd_SteamAPI_ISteamController_GetDigitalActionOrigins(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionOrigins = NULL;

/* SteamAPI_ISteamController_GetGamepadIndexForController */
__asm__(".globl SteamAPI_ISteamController_GetGamepadIndexForController");
__asm__("SteamAPI_ISteamController_GetGamepadIndexForController: jmp *_fwd_SteamAPI_ISteamController_GetGamepadIndexForController(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetGamepadIndexForController = NULL;

/* SteamAPI_ISteamController_GetGlyphForActionOrigin */
__asm__(".globl SteamAPI_ISteamController_GetGlyphForActionOrigin");
__asm__("SteamAPI_ISteamController_GetGlyphForActionOrigin: jmp *_fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin = NULL;

/* SteamAPI_ISteamController_GetGlyphForXboxOrigin */
__asm__(".globl SteamAPI_ISteamController_GetGlyphForXboxOrigin");
__asm__("SteamAPI_ISteamController_GetGlyphForXboxOrigin: jmp *_fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin = NULL;

/* SteamAPI_ISteamController_GetInputTypeForHandle */
__asm__(".globl SteamAPI_ISteamController_GetInputTypeForHandle");
__asm__("SteamAPI_ISteamController_GetInputTypeForHandle: jmp *_fwd_SteamAPI_ISteamController_GetInputTypeForHandle(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetInputTypeForHandle = NULL;

/* SteamAPI_ISteamController_GetMotionData */
__asm__(".globl SteamAPI_ISteamController_GetMotionData");
__asm__("SteamAPI_ISteamController_GetMotionData: jmp *_fwd_SteamAPI_ISteamController_GetMotionData(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetMotionData = NULL;

/* SteamAPI_ISteamController_GetStringForActionOrigin */
__asm__(".globl SteamAPI_ISteamController_GetStringForActionOrigin");
__asm__("SteamAPI_ISteamController_GetStringForActionOrigin: jmp *_fwd_SteamAPI_ISteamController_GetStringForActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetStringForActionOrigin = NULL;

/* SteamAPI_ISteamController_GetStringForXboxOrigin */
__asm__(".globl SteamAPI_ISteamController_GetStringForXboxOrigin");
__asm__("SteamAPI_ISteamController_GetStringForXboxOrigin: jmp *_fwd_SteamAPI_ISteamController_GetStringForXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_GetStringForXboxOrigin = NULL;

/* SteamAPI_ISteamController_Init */
__asm__(".globl SteamAPI_ISteamController_Init");
__asm__("SteamAPI_ISteamController_Init: jmp *_fwd_SteamAPI_ISteamController_Init(%rip)");
static void *_fwd_SteamAPI_ISteamController_Init = NULL;

/* SteamAPI_ISteamController_RunFrame */
__asm__(".globl SteamAPI_ISteamController_RunFrame");
__asm__("SteamAPI_ISteamController_RunFrame: jmp *_fwd_SteamAPI_ISteamController_RunFrame(%rip)");
static void *_fwd_SteamAPI_ISteamController_RunFrame = NULL;

/* SteamAPI_ISteamController_SetLEDColor */
__asm__(".globl SteamAPI_ISteamController_SetLEDColor");
__asm__("SteamAPI_ISteamController_SetLEDColor: jmp *_fwd_SteamAPI_ISteamController_SetLEDColor(%rip)");
static void *_fwd_SteamAPI_ISteamController_SetLEDColor = NULL;

/* SteamAPI_ISteamController_ShowBindingPanel */
__asm__(".globl SteamAPI_ISteamController_ShowBindingPanel");
__asm__("SteamAPI_ISteamController_ShowBindingPanel: jmp *_fwd_SteamAPI_ISteamController_ShowBindingPanel(%rip)");
static void *_fwd_SteamAPI_ISteamController_ShowBindingPanel = NULL;

/* SteamAPI_ISteamController_Shutdown */
__asm__(".globl SteamAPI_ISteamController_Shutdown");
__asm__("SteamAPI_ISteamController_Shutdown: jmp *_fwd_SteamAPI_ISteamController_Shutdown(%rip)");
static void *_fwd_SteamAPI_ISteamController_Shutdown = NULL;

/* SteamAPI_ISteamController_StopAnalogActionMomentum */
__asm__(".globl SteamAPI_ISteamController_StopAnalogActionMomentum");
__asm__("SteamAPI_ISteamController_StopAnalogActionMomentum: jmp *_fwd_SteamAPI_ISteamController_StopAnalogActionMomentum(%rip)");
static void *_fwd_SteamAPI_ISteamController_StopAnalogActionMomentum = NULL;

/* SteamAPI_ISteamController_TranslateActionOrigin */
__asm__(".globl SteamAPI_ISteamController_TranslateActionOrigin");
__asm__("SteamAPI_ISteamController_TranslateActionOrigin: jmp *_fwd_SteamAPI_ISteamController_TranslateActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamController_TranslateActionOrigin = NULL;

/* SteamAPI_ISteamController_TriggerHapticPulse */
__asm__(".globl SteamAPI_ISteamController_TriggerHapticPulse");
__asm__("SteamAPI_ISteamController_TriggerHapticPulse: jmp *_fwd_SteamAPI_ISteamController_TriggerHapticPulse(%rip)");
static void *_fwd_SteamAPI_ISteamController_TriggerHapticPulse = NULL;

/* SteamAPI_ISteamController_TriggerRepeatedHapticPulse */
__asm__(".globl SteamAPI_ISteamController_TriggerRepeatedHapticPulse");
__asm__("SteamAPI_ISteamController_TriggerRepeatedHapticPulse: jmp *_fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse(%rip)");
static void *_fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse = NULL;

/* SteamAPI_ISteamController_TriggerVibration */
__asm__(".globl SteamAPI_ISteamController_TriggerVibration");
__asm__("SteamAPI_ISteamController_TriggerVibration: jmp *_fwd_SteamAPI_ISteamController_TriggerVibration(%rip)");
static void *_fwd_SteamAPI_ISteamController_TriggerVibration = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlay */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlay");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlay: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlay(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlay = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayToStore */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayToStore");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayToStore: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayToUser */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayToUser: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser = NULL;

/* SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage */
__asm__(".globl SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage");
__asm__("SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage: jmp *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage = NULL;

/* SteamAPI_ISteamFriends_BHasEquippedProfileItem */
__asm__(".globl SteamAPI_ISteamFriends_BHasEquippedProfileItem");
__asm__("SteamAPI_ISteamFriends_BHasEquippedProfileItem: jmp *_fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem = NULL;

/* SteamAPI_ISteamFriends_ClearRichPresence */
__asm__(".globl SteamAPI_ISteamFriends_ClearRichPresence");
__asm__("SteamAPI_ISteamFriends_ClearRichPresence: jmp *_fwd_SteamAPI_ISteamFriends_ClearRichPresence(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ClearRichPresence = NULL;

/* SteamAPI_ISteamFriends_CloseClanChatWindowInSteam */
__asm__(".globl SteamAPI_ISteamFriends_CloseClanChatWindowInSteam");
__asm__("SteamAPI_ISteamFriends_CloseClanChatWindowInSteam: jmp *_fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam = NULL;

/* SteamAPI_ISteamFriends_DownloadClanActivityCounts */
__asm__(".globl SteamAPI_ISteamFriends_DownloadClanActivityCounts");
__asm__("SteamAPI_ISteamFriends_DownloadClanActivityCounts: jmp *_fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts = NULL;

/* SteamAPI_ISteamFriends_EnumerateFollowingList */
__asm__(".globl SteamAPI_ISteamFriends_EnumerateFollowingList");
__asm__("SteamAPI_ISteamFriends_EnumerateFollowingList: jmp *_fwd_SteamAPI_ISteamFriends_EnumerateFollowingList(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_EnumerateFollowingList = NULL;

/* SteamAPI_ISteamFriends_GetChatMemberByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetChatMemberByIndex");
__asm__("SteamAPI_ISteamFriends_GetChatMemberByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex = NULL;

/* SteamAPI_ISteamFriends_GetClanActivityCounts */
__asm__(".globl SteamAPI_ISteamFriends_GetClanActivityCounts");
__asm__("SteamAPI_ISteamFriends_GetClanActivityCounts: jmp *_fwd_SteamAPI_ISteamFriends_GetClanActivityCounts(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanActivityCounts = NULL;

/* SteamAPI_ISteamFriends_GetClanByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetClanByIndex");
__asm__("SteamAPI_ISteamFriends_GetClanByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetClanByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanByIndex = NULL;

/* SteamAPI_ISteamFriends_GetClanChatMemberCount */
__asm__(".globl SteamAPI_ISteamFriends_GetClanChatMemberCount");
__asm__("SteamAPI_ISteamFriends_GetClanChatMemberCount: jmp *_fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount = NULL;

/* SteamAPI_ISteamFriends_GetClanChatMessage */
__asm__(".globl SteamAPI_ISteamFriends_GetClanChatMessage");
__asm__("SteamAPI_ISteamFriends_GetClanChatMessage: jmp *_fwd_SteamAPI_ISteamFriends_GetClanChatMessage(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanChatMessage = NULL;

/* SteamAPI_ISteamFriends_GetClanCount */
__asm__(".globl SteamAPI_ISteamFriends_GetClanCount");
__asm__("SteamAPI_ISteamFriends_GetClanCount: jmp *_fwd_SteamAPI_ISteamFriends_GetClanCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanCount = NULL;

/* SteamAPI_ISteamFriends_GetClanName */
__asm__(".globl SteamAPI_ISteamFriends_GetClanName");
__asm__("SteamAPI_ISteamFriends_GetClanName: jmp *_fwd_SteamAPI_ISteamFriends_GetClanName(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanName = NULL;

/* SteamAPI_ISteamFriends_GetClanOfficerByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetClanOfficerByIndex");
__asm__("SteamAPI_ISteamFriends_GetClanOfficerByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex = NULL;

/* SteamAPI_ISteamFriends_GetClanOfficerCount */
__asm__(".globl SteamAPI_ISteamFriends_GetClanOfficerCount");
__asm__("SteamAPI_ISteamFriends_GetClanOfficerCount: jmp *_fwd_SteamAPI_ISteamFriends_GetClanOfficerCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanOfficerCount = NULL;

/* SteamAPI_ISteamFriends_GetClanOwner */
__asm__(".globl SteamAPI_ISteamFriends_GetClanOwner");
__asm__("SteamAPI_ISteamFriends_GetClanOwner: jmp *_fwd_SteamAPI_ISteamFriends_GetClanOwner(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanOwner = NULL;

/* SteamAPI_ISteamFriends_GetClanTag */
__asm__(".globl SteamAPI_ISteamFriends_GetClanTag");
__asm__("SteamAPI_ISteamFriends_GetClanTag: jmp *_fwd_SteamAPI_ISteamFriends_GetClanTag(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetClanTag = NULL;

/* SteamAPI_ISteamFriends_GetCoplayFriend */
__asm__(".globl SteamAPI_ISteamFriends_GetCoplayFriend");
__asm__("SteamAPI_ISteamFriends_GetCoplayFriend: jmp *_fwd_SteamAPI_ISteamFriends_GetCoplayFriend(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetCoplayFriend = NULL;

/* SteamAPI_ISteamFriends_GetCoplayFriendCount */
__asm__(".globl SteamAPI_ISteamFriends_GetCoplayFriendCount");
__asm__("SteamAPI_ISteamFriends_GetCoplayFriendCount: jmp *_fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount = NULL;

/* SteamAPI_ISteamFriends_GetFollowerCount */
__asm__(".globl SteamAPI_ISteamFriends_GetFollowerCount");
__asm__("SteamAPI_ISteamFriends_GetFollowerCount: jmp *_fwd_SteamAPI_ISteamFriends_GetFollowerCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFollowerCount = NULL;

/* SteamAPI_ISteamFriends_GetFriendByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendByIndex");
__asm__("SteamAPI_ISteamFriends_GetFriendByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendByIndex = NULL;

/* SteamAPI_ISteamFriends_GetFriendCoplayGame */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendCoplayGame");
__asm__("SteamAPI_ISteamFriends_GetFriendCoplayGame: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame = NULL;

/* SteamAPI_ISteamFriends_GetFriendCoplayTime */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendCoplayTime");
__asm__("SteamAPI_ISteamFriends_GetFriendCoplayTime: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime = NULL;

/* SteamAPI_ISteamFriends_GetFriendCount */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendCount");
__asm__("SteamAPI_ISteamFriends_GetFriendCount: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCount = NULL;

/* SteamAPI_ISteamFriends_GetFriendCountFromSource */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendCountFromSource");
__asm__("SteamAPI_ISteamFriends_GetFriendCountFromSource: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource = NULL;

/* SteamAPI_ISteamFriends_GetFriendFromSourceByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendFromSourceByIndex");
__asm__("SteamAPI_ISteamFriends_GetFriendFromSourceByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex = NULL;

/* SteamAPI_ISteamFriends_GetFriendGamePlayed */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendGamePlayed");
__asm__("SteamAPI_ISteamFriends_GetFriendGamePlayed: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed = NULL;

/* SteamAPI_ISteamFriends_GetFriendMessage */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendMessage");
__asm__("SteamAPI_ISteamFriends_GetFriendMessage: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendMessage(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendMessage = NULL;

/* SteamAPI_ISteamFriends_GetFriendPersonaName */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendPersonaName");
__asm__("SteamAPI_ISteamFriends_GetFriendPersonaName: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaName(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaName = NULL;

/* SteamAPI_ISteamFriends_GetFriendPersonaNameHistory */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendPersonaNameHistory");
__asm__("SteamAPI_ISteamFriends_GetFriendPersonaNameHistory: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory = NULL;

/* SteamAPI_ISteamFriends_GetFriendPersonaState */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendPersonaState");
__asm__("SteamAPI_ISteamFriends_GetFriendPersonaState: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaState(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaState = NULL;

/* SteamAPI_ISteamFriends_GetFriendRelationship */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendRelationship");
__asm__("SteamAPI_ISteamFriends_GetFriendRelationship: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendRelationship(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRelationship = NULL;

/* SteamAPI_ISteamFriends_GetFriendRichPresence */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendRichPresence");
__asm__("SteamAPI_ISteamFriends_GetFriendRichPresence: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresence(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresence = NULL;

/* SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex");
__asm__("SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex = NULL;

/* SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount");
__asm__("SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount = NULL;

/* SteamAPI_ISteamFriends_GetFriendsGroupCount */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendsGroupCount");
__asm__("SteamAPI_ISteamFriends_GetFriendsGroupCount: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount = NULL;

/* SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex");
__asm__("SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex = NULL;

/* SteamAPI_ISteamFriends_GetFriendsGroupMembersCount */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendsGroupMembersCount");
__asm__("SteamAPI_ISteamFriends_GetFriendsGroupMembersCount: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount = NULL;

/* SteamAPI_ISteamFriends_GetFriendsGroupMembersList */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendsGroupMembersList");
__asm__("SteamAPI_ISteamFriends_GetFriendsGroupMembersList: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList = NULL;

/* SteamAPI_ISteamFriends_GetFriendsGroupName */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendsGroupName");
__asm__("SteamAPI_ISteamFriends_GetFriendsGroupName: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupName(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupName = NULL;

/* SteamAPI_ISteamFriends_GetFriendSteamLevel */
__asm__(".globl SteamAPI_ISteamFriends_GetFriendSteamLevel");
__asm__("SteamAPI_ISteamFriends_GetFriendSteamLevel: jmp *_fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel = NULL;

/* SteamAPI_ISteamFriends_GetLargeFriendAvatar */
__asm__(".globl SteamAPI_ISteamFriends_GetLargeFriendAvatar");
__asm__("SteamAPI_ISteamFriends_GetLargeFriendAvatar: jmp *_fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar = NULL;

/* SteamAPI_ISteamFriends_GetMediumFriendAvatar */
__asm__(".globl SteamAPI_ISteamFriends_GetMediumFriendAvatar");
__asm__("SteamAPI_ISteamFriends_GetMediumFriendAvatar: jmp *_fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar = NULL;

/* SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages */
__asm__(".globl SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages");
__asm__("SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages: jmp *_fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages = NULL;

/* SteamAPI_ISteamFriends_GetPersonaName */
__asm__(".globl SteamAPI_ISteamFriends_GetPersonaName");
__asm__("SteamAPI_ISteamFriends_GetPersonaName: jmp *_fwd_SteamAPI_ISteamFriends_GetPersonaName(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetPersonaName = NULL;

/* SteamAPI_ISteamFriends_GetPersonaState */
__asm__(".globl SteamAPI_ISteamFriends_GetPersonaState");
__asm__("SteamAPI_ISteamFriends_GetPersonaState: jmp *_fwd_SteamAPI_ISteamFriends_GetPersonaState(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetPersonaState = NULL;

/* SteamAPI_ISteamFriends_GetPlayerNickname */
__asm__(".globl SteamAPI_ISteamFriends_GetPlayerNickname");
__asm__("SteamAPI_ISteamFriends_GetPlayerNickname: jmp *_fwd_SteamAPI_ISteamFriends_GetPlayerNickname(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetPlayerNickname = NULL;

/* SteamAPI_ISteamFriends_GetProfileItemPropertyString */
__asm__(".globl SteamAPI_ISteamFriends_GetProfileItemPropertyString");
__asm__("SteamAPI_ISteamFriends_GetProfileItemPropertyString: jmp *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString = NULL;

/* SteamAPI_ISteamFriends_GetProfileItemPropertyUint */
__asm__(".globl SteamAPI_ISteamFriends_GetProfileItemPropertyUint");
__asm__("SteamAPI_ISteamFriends_GetProfileItemPropertyUint: jmp *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint = NULL;

/* SteamAPI_ISteamFriends_GetSmallFriendAvatar */
__asm__(".globl SteamAPI_ISteamFriends_GetSmallFriendAvatar");
__asm__("SteamAPI_ISteamFriends_GetSmallFriendAvatar: jmp *_fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar = NULL;

/* SteamAPI_ISteamFriends_GetUserRestrictions */
__asm__(".globl SteamAPI_ISteamFriends_GetUserRestrictions");
__asm__("SteamAPI_ISteamFriends_GetUserRestrictions: jmp *_fwd_SteamAPI_ISteamFriends_GetUserRestrictions(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_GetUserRestrictions = NULL;

/* SteamAPI_ISteamFriends_HasFriend */
__asm__(".globl SteamAPI_ISteamFriends_HasFriend");
__asm__("SteamAPI_ISteamFriends_HasFriend: jmp *_fwd_SteamAPI_ISteamFriends_HasFriend(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_HasFriend = NULL;

/* SteamAPI_ISteamFriends_InviteUserToGame */
__asm__(".globl SteamAPI_ISteamFriends_InviteUserToGame");
__asm__("SteamAPI_ISteamFriends_InviteUserToGame: jmp *_fwd_SteamAPI_ISteamFriends_InviteUserToGame(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_InviteUserToGame = NULL;

/* SteamAPI_ISteamFriends_IsClanChatAdmin */
__asm__(".globl SteamAPI_ISteamFriends_IsClanChatAdmin");
__asm__("SteamAPI_ISteamFriends_IsClanChatAdmin: jmp *_fwd_SteamAPI_ISteamFriends_IsClanChatAdmin(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsClanChatAdmin = NULL;

/* SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam */
__asm__(".globl SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam");
__asm__("SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam: jmp *_fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam = NULL;

/* SteamAPI_ISteamFriends_IsClanOfficialGameGroup */
__asm__(".globl SteamAPI_ISteamFriends_IsClanOfficialGameGroup");
__asm__("SteamAPI_ISteamFriends_IsClanOfficialGameGroup: jmp *_fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup = NULL;

/* SteamAPI_ISteamFriends_IsClanPublic */
__asm__(".globl SteamAPI_ISteamFriends_IsClanPublic");
__asm__("SteamAPI_ISteamFriends_IsClanPublic: jmp *_fwd_SteamAPI_ISteamFriends_IsClanPublic(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsClanPublic = NULL;

/* SteamAPI_ISteamFriends_IsFollowing */
__asm__(".globl SteamAPI_ISteamFriends_IsFollowing");
__asm__("SteamAPI_ISteamFriends_IsFollowing: jmp *_fwd_SteamAPI_ISteamFriends_IsFollowing(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsFollowing = NULL;

/* SteamAPI_ISteamFriends_IsUserInSource */
__asm__(".globl SteamAPI_ISteamFriends_IsUserInSource");
__asm__("SteamAPI_ISteamFriends_IsUserInSource: jmp *_fwd_SteamAPI_ISteamFriends_IsUserInSource(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_IsUserInSource = NULL;

/* SteamAPI_ISteamFriends_JoinClanChatRoom */
__asm__(".globl SteamAPI_ISteamFriends_JoinClanChatRoom");
__asm__("SteamAPI_ISteamFriends_JoinClanChatRoom: jmp *_fwd_SteamAPI_ISteamFriends_JoinClanChatRoom(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_JoinClanChatRoom = NULL;

/* SteamAPI_ISteamFriends_LeaveClanChatRoom */
__asm__(".globl SteamAPI_ISteamFriends_LeaveClanChatRoom");
__asm__("SteamAPI_ISteamFriends_LeaveClanChatRoom: jmp *_fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom = NULL;

/* SteamAPI_ISteamFriends_OpenClanChatWindowInSteam */
__asm__(".globl SteamAPI_ISteamFriends_OpenClanChatWindowInSteam");
__asm__("SteamAPI_ISteamFriends_OpenClanChatWindowInSteam: jmp *_fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam = NULL;

/* SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser */
__asm__(".globl SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser");
__asm__("SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser: jmp *_fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser = NULL;

/* SteamAPI_ISteamFriends_ReplyToFriendMessage */
__asm__(".globl SteamAPI_ISteamFriends_ReplyToFriendMessage");
__asm__("SteamAPI_ISteamFriends_ReplyToFriendMessage: jmp *_fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage = NULL;

/* SteamAPI_ISteamFriends_RequestClanOfficerList */
__asm__(".globl SteamAPI_ISteamFriends_RequestClanOfficerList");
__asm__("SteamAPI_ISteamFriends_RequestClanOfficerList: jmp *_fwd_SteamAPI_ISteamFriends_RequestClanOfficerList(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_RequestClanOfficerList = NULL;

/* SteamAPI_ISteamFriends_RequestEquippedProfileItems */
__asm__(".globl SteamAPI_ISteamFriends_RequestEquippedProfileItems");
__asm__("SteamAPI_ISteamFriends_RequestEquippedProfileItems: jmp *_fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems = NULL;

/* SteamAPI_ISteamFriends_RequestFriendRichPresence */
__asm__(".globl SteamAPI_ISteamFriends_RequestFriendRichPresence");
__asm__("SteamAPI_ISteamFriends_RequestFriendRichPresence: jmp *_fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence = NULL;

/* SteamAPI_ISteamFriends_RequestUserInformation */
__asm__(".globl SteamAPI_ISteamFriends_RequestUserInformation");
__asm__("SteamAPI_ISteamFriends_RequestUserInformation: jmp *_fwd_SteamAPI_ISteamFriends_RequestUserInformation(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_RequestUserInformation = NULL;

/* SteamAPI_ISteamFriends_SendClanChatMessage */
__asm__(".globl SteamAPI_ISteamFriends_SendClanChatMessage");
__asm__("SteamAPI_ISteamFriends_SendClanChatMessage: jmp *_fwd_SteamAPI_ISteamFriends_SendClanChatMessage(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SendClanChatMessage = NULL;

/* SteamAPI_ISteamFriends_SetInGameVoiceSpeaking */
__asm__(".globl SteamAPI_ISteamFriends_SetInGameVoiceSpeaking");
__asm__("SteamAPI_ISteamFriends_SetInGameVoiceSpeaking: jmp *_fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking = NULL;

/* SteamAPI_ISteamFriends_SetListenForFriendsMessages */
__asm__(".globl SteamAPI_ISteamFriends_SetListenForFriendsMessages");
__asm__("SteamAPI_ISteamFriends_SetListenForFriendsMessages: jmp *_fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages = NULL;

/* SteamAPI_ISteamFriends_SetPersonaName */
__asm__(".globl SteamAPI_ISteamFriends_SetPersonaName");
__asm__("SteamAPI_ISteamFriends_SetPersonaName: jmp *_fwd_SteamAPI_ISteamFriends_SetPersonaName(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SetPersonaName = NULL;

/* SteamAPI_ISteamFriends_SetPlayedWith */
__asm__(".globl SteamAPI_ISteamFriends_SetPlayedWith");
__asm__("SteamAPI_ISteamFriends_SetPlayedWith: jmp *_fwd_SteamAPI_ISteamFriends_SetPlayedWith(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SetPlayedWith = NULL;

/* SteamAPI_ISteamFriends_SetRichPresence */
__asm__(".globl SteamAPI_ISteamFriends_SetRichPresence");
__asm__("SteamAPI_ISteamFriends_SetRichPresence: jmp *_fwd_SteamAPI_ISteamFriends_SetRichPresence(%rip)");
static void *_fwd_SteamAPI_ISteamFriends_SetRichPresence = NULL;

/* SteamAPI_ISteamGameSearch_AcceptGame */
__asm__(".globl SteamAPI_ISteamGameSearch_AcceptGame");
__asm__("SteamAPI_ISteamGameSearch_AcceptGame: jmp *_fwd_SteamAPI_ISteamGameSearch_AcceptGame(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_AcceptGame = NULL;

/* SteamAPI_ISteamGameSearch_AddGameSearchParams */
__asm__(".globl SteamAPI_ISteamGameSearch_AddGameSearchParams");
__asm__("SteamAPI_ISteamGameSearch_AddGameSearchParams: jmp *_fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams = NULL;

/* SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame */
__asm__(".globl SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame");
__asm__("SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame: jmp *_fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame = NULL;

/* SteamAPI_ISteamGameSearch_DeclineGame */
__asm__(".globl SteamAPI_ISteamGameSearch_DeclineGame");
__asm__("SteamAPI_ISteamGameSearch_DeclineGame: jmp *_fwd_SteamAPI_ISteamGameSearch_DeclineGame(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_DeclineGame = NULL;

/* SteamAPI_ISteamGameSearch_EndGame */
__asm__(".globl SteamAPI_ISteamGameSearch_EndGame");
__asm__("SteamAPI_ISteamGameSearch_EndGame: jmp *_fwd_SteamAPI_ISteamGameSearch_EndGame(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_EndGame = NULL;

/* SteamAPI_ISteamGameSearch_EndGameSearch */
__asm__(".globl SteamAPI_ISteamGameSearch_EndGameSearch");
__asm__("SteamAPI_ISteamGameSearch_EndGameSearch: jmp *_fwd_SteamAPI_ISteamGameSearch_EndGameSearch(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_EndGameSearch = NULL;

/* SteamAPI_ISteamGameSearch_HostConfirmGameStart */
__asm__(".globl SteamAPI_ISteamGameSearch_HostConfirmGameStart");
__asm__("SteamAPI_ISteamGameSearch_HostConfirmGameStart: jmp *_fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart = NULL;

/* SteamAPI_ISteamGameSearch_RequestPlayersForGame */
__asm__(".globl SteamAPI_ISteamGameSearch_RequestPlayersForGame");
__asm__("SteamAPI_ISteamGameSearch_RequestPlayersForGame: jmp *_fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame = NULL;

/* SteamAPI_ISteamGameSearch_RetrieveConnectionDetails */
__asm__(".globl SteamAPI_ISteamGameSearch_RetrieveConnectionDetails");
__asm__("SteamAPI_ISteamGameSearch_RetrieveConnectionDetails: jmp *_fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails = NULL;

/* SteamAPI_ISteamGameSearch_SearchForGameSolo */
__asm__(".globl SteamAPI_ISteamGameSearch_SearchForGameSolo");
__asm__("SteamAPI_ISteamGameSearch_SearchForGameSolo: jmp *_fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo = NULL;

/* SteamAPI_ISteamGameSearch_SearchForGameWithLobby */
__asm__(".globl SteamAPI_ISteamGameSearch_SearchForGameWithLobby");
__asm__("SteamAPI_ISteamGameSearch_SearchForGameWithLobby: jmp *_fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby = NULL;

/* SteamAPI_ISteamGameSearch_SetConnectionDetails */
__asm__(".globl SteamAPI_ISteamGameSearch_SetConnectionDetails");
__asm__("SteamAPI_ISteamGameSearch_SetConnectionDetails: jmp *_fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails = NULL;

/* SteamAPI_ISteamGameSearch_SetGameHostParams */
__asm__(".globl SteamAPI_ISteamGameSearch_SetGameHostParams");
__asm__("SteamAPI_ISteamGameSearch_SetGameHostParams: jmp *_fwd_SteamAPI_ISteamGameSearch_SetGameHostParams(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_SetGameHostParams = NULL;

/* SteamAPI_ISteamGameSearch_SubmitPlayerResult */
__asm__(".globl SteamAPI_ISteamGameSearch_SubmitPlayerResult");
__asm__("SteamAPI_ISteamGameSearch_SubmitPlayerResult: jmp *_fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult(%rip)");
static void *_fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult = NULL;

/* SteamAPI_ISteamGameServer_AssociateWithClan */
__asm__(".globl SteamAPI_ISteamGameServer_AssociateWithClan");
__asm__("SteamAPI_ISteamGameServer_AssociateWithClan: jmp *_fwd_SteamAPI_ISteamGameServer_AssociateWithClan(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_AssociateWithClan = NULL;

/* SteamAPI_ISteamGameServer_BeginAuthSession */
__asm__(".globl SteamAPI_ISteamGameServer_BeginAuthSession");
__asm__("SteamAPI_ISteamGameServer_BeginAuthSession: jmp *_fwd_SteamAPI_ISteamGameServer_BeginAuthSession(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_BeginAuthSession = NULL;

/* SteamAPI_ISteamGameServer_BLoggedOn */
__asm__(".globl SteamAPI_ISteamGameServer_BLoggedOn");
__asm__("SteamAPI_ISteamGameServer_BLoggedOn: jmp *_fwd_SteamAPI_ISteamGameServer_BLoggedOn(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_BLoggedOn = NULL;

/* SteamAPI_ISteamGameServer_BSecure */
__asm__(".globl SteamAPI_ISteamGameServer_BSecure");
__asm__("SteamAPI_ISteamGameServer_BSecure: jmp *_fwd_SteamAPI_ISteamGameServer_BSecure(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_BSecure = NULL;

/* SteamAPI_ISteamGameServer_BUpdateUserData */
__asm__(".globl SteamAPI_ISteamGameServer_BUpdateUserData");
__asm__("SteamAPI_ISteamGameServer_BUpdateUserData: jmp *_fwd_SteamAPI_ISteamGameServer_BUpdateUserData(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_BUpdateUserData = NULL;

/* SteamAPI_ISteamGameServer_CancelAuthTicket */
__asm__(".globl SteamAPI_ISteamGameServer_CancelAuthTicket");
__asm__("SteamAPI_ISteamGameServer_CancelAuthTicket: jmp *_fwd_SteamAPI_ISteamGameServer_CancelAuthTicket(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_CancelAuthTicket = NULL;

/* SteamAPI_ISteamGameServer_ClearAllKeyValues */
__asm__(".globl SteamAPI_ISteamGameServer_ClearAllKeyValues");
__asm__("SteamAPI_ISteamGameServer_ClearAllKeyValues: jmp *_fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues = NULL;

/* SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility */
__asm__(".globl SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility");
__asm__("SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility: jmp *_fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility = NULL;

/* SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection */
__asm__(".globl SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection");
__asm__("SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection: jmp *_fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection = NULL;

/* SteamAPI_ISteamGameServer_EndAuthSession */
__asm__(".globl SteamAPI_ISteamGameServer_EndAuthSession");
__asm__("SteamAPI_ISteamGameServer_EndAuthSession: jmp *_fwd_SteamAPI_ISteamGameServer_EndAuthSession(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_EndAuthSession = NULL;

/* SteamAPI_ISteamGameServer_GetAuthSessionTicket */
__asm__(".globl SteamAPI_ISteamGameServer_GetAuthSessionTicket");
__asm__("SteamAPI_ISteamGameServer_GetAuthSessionTicket: jmp *_fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket = NULL;

/* SteamAPI_ISteamGameServer_GetGameplayStats */
__asm__(".globl SteamAPI_ISteamGameServer_GetGameplayStats");
__asm__("SteamAPI_ISteamGameServer_GetGameplayStats: jmp *_fwd_SteamAPI_ISteamGameServer_GetGameplayStats(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetGameplayStats = NULL;

/* SteamAPI_ISteamGameServer_GetNextOutgoingPacket */
__asm__(".globl SteamAPI_ISteamGameServer_GetNextOutgoingPacket");
__asm__("SteamAPI_ISteamGameServer_GetNextOutgoingPacket: jmp *_fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket = NULL;

/* SteamAPI_ISteamGameServer_GetPublicIP */
__asm__(".globl SteamAPI_ISteamGameServer_GetPublicIP");
__asm__("SteamAPI_ISteamGameServer_GetPublicIP: jmp *_fwd_SteamAPI_ISteamGameServer_GetPublicIP(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetPublicIP = NULL;

/* SteamAPI_ISteamGameServer_GetServerReputation */
__asm__(".globl SteamAPI_ISteamGameServer_GetServerReputation");
__asm__("SteamAPI_ISteamGameServer_GetServerReputation: jmp *_fwd_SteamAPI_ISteamGameServer_GetServerReputation(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetServerReputation = NULL;

/* SteamAPI_ISteamGameServer_GetSteamID */
__asm__(".globl SteamAPI_ISteamGameServer_GetSteamID");
__asm__("SteamAPI_ISteamGameServer_GetSteamID: jmp *_fwd_SteamAPI_ISteamGameServer_GetSteamID(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_GetSteamID = NULL;

/* SteamAPI_ISteamGameServer_HandleIncomingPacket */
__asm__(".globl SteamAPI_ISteamGameServer_HandleIncomingPacket");
__asm__("SteamAPI_ISteamGameServer_HandleIncomingPacket: jmp *_fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket = NULL;

/* SteamAPI_ISteamGameServer_LogOff */
__asm__(".globl SteamAPI_ISteamGameServer_LogOff");
__asm__("SteamAPI_ISteamGameServer_LogOff: jmp *_fwd_SteamAPI_ISteamGameServer_LogOff(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_LogOff = NULL;

/* SteamAPI_ISteamGameServer_LogOn */
__asm__(".globl SteamAPI_ISteamGameServer_LogOn");
__asm__("SteamAPI_ISteamGameServer_LogOn: jmp *_fwd_SteamAPI_ISteamGameServer_LogOn(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_LogOn = NULL;

/* SteamAPI_ISteamGameServer_LogOnAnonymous */
__asm__(".globl SteamAPI_ISteamGameServer_LogOnAnonymous");
__asm__("SteamAPI_ISteamGameServer_LogOnAnonymous: jmp *_fwd_SteamAPI_ISteamGameServer_LogOnAnonymous(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_LogOnAnonymous = NULL;

/* SteamAPI_ISteamGameServer_RequestUserGroupStatus */
__asm__(".globl SteamAPI_ISteamGameServer_RequestUserGroupStatus");
__asm__("SteamAPI_ISteamGameServer_RequestUserGroupStatus: jmp *_fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus = NULL;

/* SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED */
__asm__(".globl SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED");
__asm__("SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED: jmp *_fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED = NULL;

/* SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED */
__asm__(".globl SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED");
__asm__("SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED: jmp *_fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED = NULL;

/* SteamAPI_ISteamGameServer_SetAdvertiseServerActive */
__asm__(".globl SteamAPI_ISteamGameServer_SetAdvertiseServerActive");
__asm__("SteamAPI_ISteamGameServer_SetAdvertiseServerActive: jmp *_fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive = NULL;

/* SteamAPI_ISteamGameServer_SetBotPlayerCount */
__asm__(".globl SteamAPI_ISteamGameServer_SetBotPlayerCount");
__asm__("SteamAPI_ISteamGameServer_SetBotPlayerCount: jmp *_fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount = NULL;

/* SteamAPI_ISteamGameServer_SetDedicatedServer */
__asm__(".globl SteamAPI_ISteamGameServer_SetDedicatedServer");
__asm__("SteamAPI_ISteamGameServer_SetDedicatedServer: jmp *_fwd_SteamAPI_ISteamGameServer_SetDedicatedServer(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetDedicatedServer = NULL;

/* SteamAPI_ISteamGameServer_SetGameData */
__asm__(".globl SteamAPI_ISteamGameServer_SetGameData");
__asm__("SteamAPI_ISteamGameServer_SetGameData: jmp *_fwd_SteamAPI_ISteamGameServer_SetGameData(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetGameData = NULL;

/* SteamAPI_ISteamGameServer_SetGameDescription */
__asm__(".globl SteamAPI_ISteamGameServer_SetGameDescription");
__asm__("SteamAPI_ISteamGameServer_SetGameDescription: jmp *_fwd_SteamAPI_ISteamGameServer_SetGameDescription(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetGameDescription = NULL;

/* SteamAPI_ISteamGameServer_SetGameTags */
__asm__(".globl SteamAPI_ISteamGameServer_SetGameTags");
__asm__("SteamAPI_ISteamGameServer_SetGameTags: jmp *_fwd_SteamAPI_ISteamGameServer_SetGameTags(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetGameTags = NULL;

/* SteamAPI_ISteamGameServer_SetKeyValue */
__asm__(".globl SteamAPI_ISteamGameServer_SetKeyValue");
__asm__("SteamAPI_ISteamGameServer_SetKeyValue: jmp *_fwd_SteamAPI_ISteamGameServer_SetKeyValue(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetKeyValue = NULL;

/* SteamAPI_ISteamGameServer_SetMapName */
__asm__(".globl SteamAPI_ISteamGameServer_SetMapName");
__asm__("SteamAPI_ISteamGameServer_SetMapName: jmp *_fwd_SteamAPI_ISteamGameServer_SetMapName(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetMapName = NULL;

/* SteamAPI_ISteamGameServer_SetMaxPlayerCount */
__asm__(".globl SteamAPI_ISteamGameServer_SetMaxPlayerCount");
__asm__("SteamAPI_ISteamGameServer_SetMaxPlayerCount: jmp *_fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount = NULL;

/* SteamAPI_ISteamGameServer_SetModDir */
__asm__(".globl SteamAPI_ISteamGameServer_SetModDir");
__asm__("SteamAPI_ISteamGameServer_SetModDir: jmp *_fwd_SteamAPI_ISteamGameServer_SetModDir(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetModDir = NULL;

/* SteamAPI_ISteamGameServer_SetPasswordProtected */
__asm__(".globl SteamAPI_ISteamGameServer_SetPasswordProtected");
__asm__("SteamAPI_ISteamGameServer_SetPasswordProtected: jmp *_fwd_SteamAPI_ISteamGameServer_SetPasswordProtected(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetPasswordProtected = NULL;

/* SteamAPI_ISteamGameServer_SetProduct */
__asm__(".globl SteamAPI_ISteamGameServer_SetProduct");
__asm__("SteamAPI_ISteamGameServer_SetProduct: jmp *_fwd_SteamAPI_ISteamGameServer_SetProduct(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetProduct = NULL;

/* SteamAPI_ISteamGameServer_SetRegion */
__asm__(".globl SteamAPI_ISteamGameServer_SetRegion");
__asm__("SteamAPI_ISteamGameServer_SetRegion: jmp *_fwd_SteamAPI_ISteamGameServer_SetRegion(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetRegion = NULL;

/* SteamAPI_ISteamGameServer_SetServerName */
__asm__(".globl SteamAPI_ISteamGameServer_SetServerName");
__asm__("SteamAPI_ISteamGameServer_SetServerName: jmp *_fwd_SteamAPI_ISteamGameServer_SetServerName(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetServerName = NULL;

/* SteamAPI_ISteamGameServer_SetSpectatorPort */
__asm__(".globl SteamAPI_ISteamGameServer_SetSpectatorPort");
__asm__("SteamAPI_ISteamGameServer_SetSpectatorPort: jmp *_fwd_SteamAPI_ISteamGameServer_SetSpectatorPort(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetSpectatorPort = NULL;

/* SteamAPI_ISteamGameServer_SetSpectatorServerName */
__asm__(".globl SteamAPI_ISteamGameServer_SetSpectatorServerName");
__asm__("SteamAPI_ISteamGameServer_SetSpectatorServerName: jmp *_fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName = NULL;

/* SteamAPI_ISteamGameServerStats_ClearUserAchievement */
__asm__(".globl SteamAPI_ISteamGameServerStats_ClearUserAchievement");
__asm__("SteamAPI_ISteamGameServerStats_ClearUserAchievement: jmp *_fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement = NULL;

/* SteamAPI_ISteamGameServerStats_GetUserAchievement */
__asm__(".globl SteamAPI_ISteamGameServerStats_GetUserAchievement");
__asm__("SteamAPI_ISteamGameServerStats_GetUserAchievement: jmp *_fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement = NULL;

/* SteamAPI_ISteamGameServerStats_GetUserStatFloat */
__asm__(".globl SteamAPI_ISteamGameServerStats_GetUserStatFloat");
__asm__("SteamAPI_ISteamGameServerStats_GetUserStatFloat: jmp *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat = NULL;

/* SteamAPI_ISteamGameServerStats_GetUserStatInt32 */
__asm__(".globl SteamAPI_ISteamGameServerStats_GetUserStatInt32");
__asm__("SteamAPI_ISteamGameServerStats_GetUserStatInt32: jmp *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32 = NULL;

/* SteamAPI_ISteamGameServerStats_RequestUserStats */
__asm__(".globl SteamAPI_ISteamGameServerStats_RequestUserStats");
__asm__("SteamAPI_ISteamGameServerStats_RequestUserStats: jmp *_fwd_SteamAPI_ISteamGameServerStats_RequestUserStats(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_RequestUserStats = NULL;

/* SteamAPI_ISteamGameServerStats_SetUserAchievement */
__asm__(".globl SteamAPI_ISteamGameServerStats_SetUserAchievement");
__asm__("SteamAPI_ISteamGameServerStats_SetUserAchievement: jmp *_fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement = NULL;

/* SteamAPI_ISteamGameServerStats_SetUserStatFloat */
__asm__(".globl SteamAPI_ISteamGameServerStats_SetUserStatFloat");
__asm__("SteamAPI_ISteamGameServerStats_SetUserStatFloat: jmp *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat = NULL;

/* SteamAPI_ISteamGameServerStats_SetUserStatInt32 */
__asm__(".globl SteamAPI_ISteamGameServerStats_SetUserStatInt32");
__asm__("SteamAPI_ISteamGameServerStats_SetUserStatInt32: jmp *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32 = NULL;

/* SteamAPI_ISteamGameServerStats_StoreUserStats */
__asm__(".globl SteamAPI_ISteamGameServerStats_StoreUserStats");
__asm__("SteamAPI_ISteamGameServerStats_StoreUserStats: jmp *_fwd_SteamAPI_ISteamGameServerStats_StoreUserStats(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_StoreUserStats = NULL;

/* SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat */
__asm__(".globl SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat");
__asm__("SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat: jmp *_fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat(%rip)");
static void *_fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat = NULL;

/* SteamAPI_ISteamGameServer_UserHasLicenseForApp */
__asm__(".globl SteamAPI_ISteamGameServer_UserHasLicenseForApp");
__asm__("SteamAPI_ISteamGameServer_UserHasLicenseForApp: jmp *_fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp = NULL;

/* SteamAPI_ISteamGameServer_WasRestartRequested */
__asm__(".globl SteamAPI_ISteamGameServer_WasRestartRequested");
__asm__("SteamAPI_ISteamGameServer_WasRestartRequested: jmp *_fwd_SteamAPI_ISteamGameServer_WasRestartRequested(%rip)");
static void *_fwd_SteamAPI_ISteamGameServer_WasRestartRequested = NULL;

/* SteamAPI_ISteamHTMLSurface_AddHeader */
__asm__(".globl SteamAPI_ISteamHTMLSurface_AddHeader");
__asm__("SteamAPI_ISteamHTMLSurface_AddHeader: jmp *_fwd_SteamAPI_ISteamHTMLSurface_AddHeader(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_AddHeader = NULL;

/* SteamAPI_ISteamHTMLSurface_AllowStartRequest */
__asm__(".globl SteamAPI_ISteamHTMLSurface_AllowStartRequest");
__asm__("SteamAPI_ISteamHTMLSurface_AllowStartRequest: jmp *_fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest = NULL;

/* SteamAPI_ISteamHTMLSurface_CopyToClipboard */
__asm__(".globl SteamAPI_ISteamHTMLSurface_CopyToClipboard");
__asm__("SteamAPI_ISteamHTMLSurface_CopyToClipboard: jmp *_fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard = NULL;

/* SteamAPI_ISteamHTMLSurface_CreateBrowser */
__asm__(".globl SteamAPI_ISteamHTMLSurface_CreateBrowser");
__asm__("SteamAPI_ISteamHTMLSurface_CreateBrowser: jmp *_fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser = NULL;

/* SteamAPI_ISteamHTMLSurface_ExecuteJavascript */
__asm__(".globl SteamAPI_ISteamHTMLSurface_ExecuteJavascript");
__asm__("SteamAPI_ISteamHTMLSurface_ExecuteJavascript: jmp *_fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript = NULL;

/* SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse */
__asm__(".globl SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse");
__asm__("SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse: jmp *_fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse = NULL;

/* SteamAPI_ISteamHTMLSurface_Find */
__asm__(".globl SteamAPI_ISteamHTMLSurface_Find");
__asm__("SteamAPI_ISteamHTMLSurface_Find: jmp *_fwd_SteamAPI_ISteamHTMLSurface_Find(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_Find = NULL;

/* SteamAPI_ISteamHTMLSurface_GetLinkAtPosition */
__asm__(".globl SteamAPI_ISteamHTMLSurface_GetLinkAtPosition");
__asm__("SteamAPI_ISteamHTMLSurface_GetLinkAtPosition: jmp *_fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition = NULL;

/* SteamAPI_ISteamHTMLSurface_GoBack */
__asm__(".globl SteamAPI_ISteamHTMLSurface_GoBack");
__asm__("SteamAPI_ISteamHTMLSurface_GoBack: jmp *_fwd_SteamAPI_ISteamHTMLSurface_GoBack(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_GoBack = NULL;

/* SteamAPI_ISteamHTMLSurface_GoForward */
__asm__(".globl SteamAPI_ISteamHTMLSurface_GoForward");
__asm__("SteamAPI_ISteamHTMLSurface_GoForward: jmp *_fwd_SteamAPI_ISteamHTMLSurface_GoForward(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_GoForward = NULL;

/* SteamAPI_ISteamHTMLSurface_Init */
__asm__(".globl SteamAPI_ISteamHTMLSurface_Init");
__asm__("SteamAPI_ISteamHTMLSurface_Init: jmp *_fwd_SteamAPI_ISteamHTMLSurface_Init(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_Init = NULL;

/* SteamAPI_ISteamHTMLSurface_JSDialogResponse */
__asm__(".globl SteamAPI_ISteamHTMLSurface_JSDialogResponse");
__asm__("SteamAPI_ISteamHTMLSurface_JSDialogResponse: jmp *_fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse = NULL;

/* SteamAPI_ISteamHTMLSurface_KeyChar */
__asm__(".globl SteamAPI_ISteamHTMLSurface_KeyChar");
__asm__("SteamAPI_ISteamHTMLSurface_KeyChar: jmp *_fwd_SteamAPI_ISteamHTMLSurface_KeyChar(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyChar = NULL;

/* SteamAPI_ISteamHTMLSurface_KeyDown */
__asm__(".globl SteamAPI_ISteamHTMLSurface_KeyDown");
__asm__("SteamAPI_ISteamHTMLSurface_KeyDown: jmp *_fwd_SteamAPI_ISteamHTMLSurface_KeyDown(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyDown = NULL;

/* SteamAPI_ISteamHTMLSurface_KeyUp */
__asm__(".globl SteamAPI_ISteamHTMLSurface_KeyUp");
__asm__("SteamAPI_ISteamHTMLSurface_KeyUp: jmp *_fwd_SteamAPI_ISteamHTMLSurface_KeyUp(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyUp = NULL;

/* SteamAPI_ISteamHTMLSurface_LoadURL */
__asm__(".globl SteamAPI_ISteamHTMLSurface_LoadURL");
__asm__("SteamAPI_ISteamHTMLSurface_LoadURL: jmp *_fwd_SteamAPI_ISteamHTMLSurface_LoadURL(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_LoadURL = NULL;

/* SteamAPI_ISteamHTMLSurface_MouseDoubleClick */
__asm__(".globl SteamAPI_ISteamHTMLSurface_MouseDoubleClick");
__asm__("SteamAPI_ISteamHTMLSurface_MouseDoubleClick: jmp *_fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick = NULL;

/* SteamAPI_ISteamHTMLSurface_MouseDown */
__asm__(".globl SteamAPI_ISteamHTMLSurface_MouseDown");
__asm__("SteamAPI_ISteamHTMLSurface_MouseDown: jmp *_fwd_SteamAPI_ISteamHTMLSurface_MouseDown(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseDown = NULL;

/* SteamAPI_ISteamHTMLSurface_MouseMove */
__asm__(".globl SteamAPI_ISteamHTMLSurface_MouseMove");
__asm__("SteamAPI_ISteamHTMLSurface_MouseMove: jmp *_fwd_SteamAPI_ISteamHTMLSurface_MouseMove(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseMove = NULL;

/* SteamAPI_ISteamHTMLSurface_MouseUp */
__asm__(".globl SteamAPI_ISteamHTMLSurface_MouseUp");
__asm__("SteamAPI_ISteamHTMLSurface_MouseUp: jmp *_fwd_SteamAPI_ISteamHTMLSurface_MouseUp(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseUp = NULL;

/* SteamAPI_ISteamHTMLSurface_MouseWheel */
__asm__(".globl SteamAPI_ISteamHTMLSurface_MouseWheel");
__asm__("SteamAPI_ISteamHTMLSurface_MouseWheel: jmp *_fwd_SteamAPI_ISteamHTMLSurface_MouseWheel(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseWheel = NULL;

/* SteamAPI_ISteamHTMLSurface_OpenDeveloperTools */
__asm__(".globl SteamAPI_ISteamHTMLSurface_OpenDeveloperTools");
__asm__("SteamAPI_ISteamHTMLSurface_OpenDeveloperTools: jmp *_fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools = NULL;

/* SteamAPI_ISteamHTMLSurface_PasteFromClipboard */
__asm__(".globl SteamAPI_ISteamHTMLSurface_PasteFromClipboard");
__asm__("SteamAPI_ISteamHTMLSurface_PasteFromClipboard: jmp *_fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard = NULL;

/* SteamAPI_ISteamHTMLSurface_Reload */
__asm__(".globl SteamAPI_ISteamHTMLSurface_Reload");
__asm__("SteamAPI_ISteamHTMLSurface_Reload: jmp *_fwd_SteamAPI_ISteamHTMLSurface_Reload(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_Reload = NULL;

/* SteamAPI_ISteamHTMLSurface_RemoveBrowser */
__asm__(".globl SteamAPI_ISteamHTMLSurface_RemoveBrowser");
__asm__("SteamAPI_ISteamHTMLSurface_RemoveBrowser: jmp *_fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser = NULL;

/* SteamAPI_ISteamHTMLSurface_SetBackgroundMode */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetBackgroundMode");
__asm__("SteamAPI_ISteamHTMLSurface_SetBackgroundMode: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode = NULL;

/* SteamAPI_ISteamHTMLSurface_SetCookie */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetCookie");
__asm__("SteamAPI_ISteamHTMLSurface_SetCookie: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetCookie(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetCookie = NULL;

/* SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor");
__asm__("SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor = NULL;

/* SteamAPI_ISteamHTMLSurface_SetHorizontalScroll */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetHorizontalScroll");
__asm__("SteamAPI_ISteamHTMLSurface_SetHorizontalScroll: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll = NULL;

/* SteamAPI_ISteamHTMLSurface_SetKeyFocus */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetKeyFocus");
__asm__("SteamAPI_ISteamHTMLSurface_SetKeyFocus: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus = NULL;

/* SteamAPI_ISteamHTMLSurface_SetPageScaleFactor */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetPageScaleFactor");
__asm__("SteamAPI_ISteamHTMLSurface_SetPageScaleFactor: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor = NULL;

/* SteamAPI_ISteamHTMLSurface_SetSize */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetSize");
__asm__("SteamAPI_ISteamHTMLSurface_SetSize: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetSize(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetSize = NULL;

/* SteamAPI_ISteamHTMLSurface_SetVerticalScroll */
__asm__(".globl SteamAPI_ISteamHTMLSurface_SetVerticalScroll");
__asm__("SteamAPI_ISteamHTMLSurface_SetVerticalScroll: jmp *_fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll = NULL;

/* SteamAPI_ISteamHTMLSurface_Shutdown */
__asm__(".globl SteamAPI_ISteamHTMLSurface_Shutdown");
__asm__("SteamAPI_ISteamHTMLSurface_Shutdown: jmp *_fwd_SteamAPI_ISteamHTMLSurface_Shutdown(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_Shutdown = NULL;

/* SteamAPI_ISteamHTMLSurface_StopFind */
__asm__(".globl SteamAPI_ISteamHTMLSurface_StopFind");
__asm__("SteamAPI_ISteamHTMLSurface_StopFind: jmp *_fwd_SteamAPI_ISteamHTMLSurface_StopFind(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_StopFind = NULL;

/* SteamAPI_ISteamHTMLSurface_StopLoad */
__asm__(".globl SteamAPI_ISteamHTMLSurface_StopLoad");
__asm__("SteamAPI_ISteamHTMLSurface_StopLoad: jmp *_fwd_SteamAPI_ISteamHTMLSurface_StopLoad(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_StopLoad = NULL;

/* SteamAPI_ISteamHTMLSurface_ViewSource */
__asm__(".globl SteamAPI_ISteamHTMLSurface_ViewSource");
__asm__("SteamAPI_ISteamHTMLSurface_ViewSource: jmp *_fwd_SteamAPI_ISteamHTMLSurface_ViewSource(%rip)");
static void *_fwd_SteamAPI_ISteamHTMLSurface_ViewSource = NULL;

/* SteamAPI_ISteamHTTP_CreateCookieContainer */
__asm__(".globl SteamAPI_ISteamHTTP_CreateCookieContainer");
__asm__("SteamAPI_ISteamHTTP_CreateCookieContainer: jmp *_fwd_SteamAPI_ISteamHTTP_CreateCookieContainer(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_CreateCookieContainer = NULL;

/* SteamAPI_ISteamHTTP_CreateHTTPRequest */
__asm__(".globl SteamAPI_ISteamHTTP_CreateHTTPRequest");
__asm__("SteamAPI_ISteamHTTP_CreateHTTPRequest: jmp *_fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest = NULL;

/* SteamAPI_ISteamHTTP_DeferHTTPRequest */
__asm__(".globl SteamAPI_ISteamHTTP_DeferHTTPRequest");
__asm__("SteamAPI_ISteamHTTP_DeferHTTPRequest: jmp *_fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct");
__asm__("SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut");
__asm__("SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPResponseBodyData */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
__asm__("SteamAPI_ISteamHTTP_GetHTTPResponseBodyData: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPResponseBodySize */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
__asm__("SteamAPI_ISteamHTTP_GetHTTPResponseBodySize: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize");
__asm__("SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue");
__asm__("SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue = NULL;

/* SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData */
__asm__(".globl SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData");
__asm__("SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData: jmp *_fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData = NULL;

/* SteamAPI_ISteamHTTP_PrioritizeHTTPRequest */
__asm__(".globl SteamAPI_ISteamHTTP_PrioritizeHTTPRequest");
__asm__("SteamAPI_ISteamHTTP_PrioritizeHTTPRequest: jmp *_fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest = NULL;

/* SteamAPI_ISteamHTTP_ReleaseCookieContainer */
__asm__(".globl SteamAPI_ISteamHTTP_ReleaseCookieContainer");
__asm__("SteamAPI_ISteamHTTP_ReleaseCookieContainer: jmp *_fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer = NULL;

/* SteamAPI_ISteamHTTP_ReleaseHTTPRequest */
__asm__(".globl SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
__asm__("SteamAPI_ISteamHTTP_ReleaseHTTPRequest: jmp *_fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest = NULL;

/* SteamAPI_ISteamHTTP_SendHTTPRequest */
__asm__(".globl SteamAPI_ISteamHTTP_SendHTTPRequest");
__asm__("SteamAPI_ISteamHTTP_SendHTTPRequest: jmp *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequest(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequest = NULL;

/* SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse */
__asm__(".globl SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse");
__asm__("SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse: jmp *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse = NULL;

/* SteamAPI_ISteamHTTP_SetCookie */
__asm__(".globl SteamAPI_ISteamHTTP_SetCookie");
__asm__("SteamAPI_ISteamHTTP_SetCookie: jmp *_fwd_SteamAPI_ISteamHTTP_SetCookie(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetCookie = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestContextValue */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestContextValue");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestContextValue: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate = NULL;

/* SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo */
__asm__(".globl SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo");
__asm__("SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo: jmp *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo(%rip)");
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo = NULL;

/* SteamAPI_ISteamInput_ActivateActionSet */
__asm__(".globl SteamAPI_ISteamInput_ActivateActionSet");
__asm__("SteamAPI_ISteamInput_ActivateActionSet: jmp *_fwd_SteamAPI_ISteamInput_ActivateActionSet(%rip)");
static void *_fwd_SteamAPI_ISteamInput_ActivateActionSet = NULL;

/* SteamAPI_ISteamInput_ActivateActionSetLayer */
__asm__(".globl SteamAPI_ISteamInput_ActivateActionSetLayer");
__asm__("SteamAPI_ISteamInput_ActivateActionSetLayer: jmp *_fwd_SteamAPI_ISteamInput_ActivateActionSetLayer(%rip)");
static void *_fwd_SteamAPI_ISteamInput_ActivateActionSetLayer = NULL;

/* SteamAPI_ISteamInput_BNewDataAvailable */
__asm__(".globl SteamAPI_ISteamInput_BNewDataAvailable");
__asm__("SteamAPI_ISteamInput_BNewDataAvailable: jmp *_fwd_SteamAPI_ISteamInput_BNewDataAvailable(%rip)");
static void *_fwd_SteamAPI_ISteamInput_BNewDataAvailable = NULL;

/* SteamAPI_ISteamInput_BWaitForData */
__asm__(".globl SteamAPI_ISteamInput_BWaitForData");
__asm__("SteamAPI_ISteamInput_BWaitForData: jmp *_fwd_SteamAPI_ISteamInput_BWaitForData(%rip)");
static void *_fwd_SteamAPI_ISteamInput_BWaitForData = NULL;

/* SteamAPI_ISteamInput_DeactivateActionSetLayer */
__asm__(".globl SteamAPI_ISteamInput_DeactivateActionSetLayer");
__asm__("SteamAPI_ISteamInput_DeactivateActionSetLayer: jmp *_fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer(%rip)");
static void *_fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer = NULL;

/* SteamAPI_ISteamInput_DeactivateAllActionSetLayers */
__asm__(".globl SteamAPI_ISteamInput_DeactivateAllActionSetLayers");
__asm__("SteamAPI_ISteamInput_DeactivateAllActionSetLayers: jmp *_fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers(%rip)");
static void *_fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers = NULL;

/* SteamAPI_ISteamInput_EnableActionEventCallbacks */
__asm__(".globl SteamAPI_ISteamInput_EnableActionEventCallbacks");
__asm__("SteamAPI_ISteamInput_EnableActionEventCallbacks: jmp *_fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks(%rip)");
static void *_fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks = NULL;

/* SteamAPI_ISteamInput_EnableDeviceCallbacks */
__asm__(".globl SteamAPI_ISteamInput_EnableDeviceCallbacks");
__asm__("SteamAPI_ISteamInput_EnableDeviceCallbacks: jmp *_fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks(%rip)");
static void *_fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks = NULL;

/* SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin");
__asm__("SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin = NULL;

/* SteamAPI_ISteamInput_GetActionSetHandle */
__asm__(".globl SteamAPI_ISteamInput_GetActionSetHandle");
__asm__("SteamAPI_ISteamInput_GetActionSetHandle: jmp *_fwd_SteamAPI_ISteamInput_GetActionSetHandle(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetActionSetHandle = NULL;

/* SteamAPI_ISteamInput_GetActiveActionSetLayers */
__asm__(".globl SteamAPI_ISteamInput_GetActiveActionSetLayers");
__asm__("SteamAPI_ISteamInput_GetActiveActionSetLayers: jmp *_fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers = NULL;

/* SteamAPI_ISteamInput_GetAnalogActionData */
__asm__(".globl SteamAPI_ISteamInput_GetAnalogActionData");
__asm__("SteamAPI_ISteamInput_GetAnalogActionData: jmp *_fwd_SteamAPI_ISteamInput_GetAnalogActionData(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionData = NULL;

/* SteamAPI_ISteamInput_GetAnalogActionHandle */
__asm__(".globl SteamAPI_ISteamInput_GetAnalogActionHandle");
__asm__("SteamAPI_ISteamInput_GetAnalogActionHandle: jmp *_fwd_SteamAPI_ISteamInput_GetAnalogActionHandle(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionHandle = NULL;

/* SteamAPI_ISteamInput_GetAnalogActionOrigins */
__asm__(".globl SteamAPI_ISteamInput_GetAnalogActionOrigins");
__asm__("SteamAPI_ISteamInput_GetAnalogActionOrigins: jmp *_fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins = NULL;

/* SteamAPI_ISteamInput_GetConnectedControllers */
__asm__(".globl SteamAPI_ISteamInput_GetConnectedControllers");
__asm__("SteamAPI_ISteamInput_GetConnectedControllers: jmp *_fwd_SteamAPI_ISteamInput_GetConnectedControllers(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetConnectedControllers = NULL;

/* SteamAPI_ISteamInput_GetControllerForGamepadIndex */
__asm__(".globl SteamAPI_ISteamInput_GetControllerForGamepadIndex");
__asm__("SteamAPI_ISteamInput_GetControllerForGamepadIndex: jmp *_fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex = NULL;

/* SteamAPI_ISteamInput_GetCurrentActionSet */
__asm__(".globl SteamAPI_ISteamInput_GetCurrentActionSet");
__asm__("SteamAPI_ISteamInput_GetCurrentActionSet: jmp *_fwd_SteamAPI_ISteamInput_GetCurrentActionSet(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetCurrentActionSet = NULL;

/* SteamAPI_ISteamInput_GetDeviceBindingRevision */
__asm__(".globl SteamAPI_ISteamInput_GetDeviceBindingRevision");
__asm__("SteamAPI_ISteamInput_GetDeviceBindingRevision: jmp *_fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision = NULL;

/* SteamAPI_ISteamInput_GetDigitalActionData */
__asm__(".globl SteamAPI_ISteamInput_GetDigitalActionData");
__asm__("SteamAPI_ISteamInput_GetDigitalActionData: jmp *_fwd_SteamAPI_ISteamInput_GetDigitalActionData(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionData = NULL;

/* SteamAPI_ISteamInput_GetDigitalActionHandle */
__asm__(".globl SteamAPI_ISteamInput_GetDigitalActionHandle");
__asm__("SteamAPI_ISteamInput_GetDigitalActionHandle: jmp *_fwd_SteamAPI_ISteamInput_GetDigitalActionHandle(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionHandle = NULL;

/* SteamAPI_ISteamInput_GetDigitalActionOrigins */
__asm__(".globl SteamAPI_ISteamInput_GetDigitalActionOrigins");
__asm__("SteamAPI_ISteamInput_GetDigitalActionOrigins: jmp *_fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins = NULL;

/* SteamAPI_ISteamInput_GetGamepadIndexForController */
__asm__(".globl SteamAPI_ISteamInput_GetGamepadIndexForController");
__asm__("SteamAPI_ISteamInput_GetGamepadIndexForController: jmp *_fwd_SteamAPI_ISteamInput_GetGamepadIndexForController(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetGamepadIndexForController = NULL;

/* SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy */
__asm__(".globl SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy");
__asm__("SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy: jmp *_fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy = NULL;

/* SteamAPI_ISteamInput_GetGlyphForXboxOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetGlyphForXboxOrigin");
__asm__("SteamAPI_ISteamInput_GetGlyphForXboxOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin = NULL;

/* SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin");
__asm__("SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin = NULL;

/* SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin");
__asm__("SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin = NULL;

/* SteamAPI_ISteamInput_GetInputTypeForHandle */
__asm__(".globl SteamAPI_ISteamInput_GetInputTypeForHandle");
__asm__("SteamAPI_ISteamInput_GetInputTypeForHandle: jmp *_fwd_SteamAPI_ISteamInput_GetInputTypeForHandle(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetInputTypeForHandle = NULL;

/* SteamAPI_ISteamInput_GetMotionData */
__asm__(".globl SteamAPI_ISteamInput_GetMotionData");
__asm__("SteamAPI_ISteamInput_GetMotionData: jmp *_fwd_SteamAPI_ISteamInput_GetMotionData(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetMotionData = NULL;

/* SteamAPI_ISteamInput_GetRemotePlaySessionID */
__asm__(".globl SteamAPI_ISteamInput_GetRemotePlaySessionID");
__asm__("SteamAPI_ISteamInput_GetRemotePlaySessionID: jmp *_fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID = NULL;

/* SteamAPI_ISteamInput_GetSessionInputConfigurationSettings */
__asm__(".globl SteamAPI_ISteamInput_GetSessionInputConfigurationSettings");
__asm__("SteamAPI_ISteamInput_GetSessionInputConfigurationSettings: jmp *_fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings = NULL;

/* SteamAPI_ISteamInput_GetStringForActionOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetStringForActionOrigin");
__asm__("SteamAPI_ISteamInput_GetStringForActionOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetStringForActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetStringForActionOrigin = NULL;

/* SteamAPI_ISteamInput_GetStringForAnalogActionName */
__asm__(".globl SteamAPI_ISteamInput_GetStringForAnalogActionName");
__asm__("SteamAPI_ISteamInput_GetStringForAnalogActionName: jmp *_fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName = NULL;

/* SteamAPI_ISteamInput_GetStringForDigitalActionName */
__asm__(".globl SteamAPI_ISteamInput_GetStringForDigitalActionName");
__asm__("SteamAPI_ISteamInput_GetStringForDigitalActionName: jmp *_fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName = NULL;

/* SteamAPI_ISteamInput_GetStringForXboxOrigin */
__asm__(".globl SteamAPI_ISteamInput_GetStringForXboxOrigin");
__asm__("SteamAPI_ISteamInput_GetStringForXboxOrigin: jmp *_fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin = NULL;

/* SteamAPI_ISteamInput_Init */
__asm__(".globl SteamAPI_ISteamInput_Init");
__asm__("SteamAPI_ISteamInput_Init: jmp *_fwd_SteamAPI_ISteamInput_Init(%rip)");
static void *_fwd_SteamAPI_ISteamInput_Init = NULL;

/* SteamAPI_ISteamInput_Legacy_TriggerHapticPulse */
__asm__(".globl SteamAPI_ISteamInput_Legacy_TriggerHapticPulse");
__asm__("SteamAPI_ISteamInput_Legacy_TriggerHapticPulse: jmp *_fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse(%rip)");
static void *_fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse = NULL;

/* SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse */
__asm__(".globl SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse");
__asm__("SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse: jmp *_fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse(%rip)");
static void *_fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse = NULL;

/* SteamAPI_ISteamInput_RunFrame */
__asm__(".globl SteamAPI_ISteamInput_RunFrame");
__asm__("SteamAPI_ISteamInput_RunFrame: jmp *_fwd_SteamAPI_ISteamInput_RunFrame(%rip)");
static void *_fwd_SteamAPI_ISteamInput_RunFrame = NULL;

/* SteamAPI_ISteamInput_SetDualSenseTriggerEffect */
__asm__(".globl SteamAPI_ISteamInput_SetDualSenseTriggerEffect");
__asm__("SteamAPI_ISteamInput_SetDualSenseTriggerEffect: jmp *_fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect(%rip)");
static void *_fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect = NULL;

/* SteamAPI_ISteamInput_SetInputActionManifestFilePath */
__asm__(".globl SteamAPI_ISteamInput_SetInputActionManifestFilePath");
__asm__("SteamAPI_ISteamInput_SetInputActionManifestFilePath: jmp *_fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath(%rip)");
static void *_fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath = NULL;

/* SteamAPI_ISteamInput_SetLEDColor */
__asm__(".globl SteamAPI_ISteamInput_SetLEDColor");
__asm__("SteamAPI_ISteamInput_SetLEDColor: jmp *_fwd_SteamAPI_ISteamInput_SetLEDColor(%rip)");
static void *_fwd_SteamAPI_ISteamInput_SetLEDColor = NULL;

/* SteamAPI_ISteamInput_ShowBindingPanel */
__asm__(".globl SteamAPI_ISteamInput_ShowBindingPanel");
__asm__("SteamAPI_ISteamInput_ShowBindingPanel: jmp *_fwd_SteamAPI_ISteamInput_ShowBindingPanel(%rip)");
static void *_fwd_SteamAPI_ISteamInput_ShowBindingPanel = NULL;

/* SteamAPI_ISteamInput_Shutdown */
__asm__(".globl SteamAPI_ISteamInput_Shutdown");
__asm__("SteamAPI_ISteamInput_Shutdown: jmp *_fwd_SteamAPI_ISteamInput_Shutdown(%rip)");
static void *_fwd_SteamAPI_ISteamInput_Shutdown = NULL;

/* SteamAPI_ISteamInput_StopAnalogActionMomentum */
__asm__(".globl SteamAPI_ISteamInput_StopAnalogActionMomentum");
__asm__("SteamAPI_ISteamInput_StopAnalogActionMomentum: jmp *_fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum(%rip)");
static void *_fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum = NULL;

/* SteamAPI_ISteamInput_TranslateActionOrigin */
__asm__(".globl SteamAPI_ISteamInput_TranslateActionOrigin");
__asm__("SteamAPI_ISteamInput_TranslateActionOrigin: jmp *_fwd_SteamAPI_ISteamInput_TranslateActionOrigin(%rip)");
static void *_fwd_SteamAPI_ISteamInput_TranslateActionOrigin = NULL;

/* SteamAPI_ISteamInput_TriggerSimpleHapticEvent */
__asm__(".globl SteamAPI_ISteamInput_TriggerSimpleHapticEvent");
__asm__("SteamAPI_ISteamInput_TriggerSimpleHapticEvent: jmp *_fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent(%rip)");
static void *_fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent = NULL;

/* SteamAPI_ISteamInput_TriggerVibration */
__asm__(".globl SteamAPI_ISteamInput_TriggerVibration");
__asm__("SteamAPI_ISteamInput_TriggerVibration: jmp *_fwd_SteamAPI_ISteamInput_TriggerVibration(%rip)");
static void *_fwd_SteamAPI_ISteamInput_TriggerVibration = NULL;

/* SteamAPI_ISteamInput_TriggerVibrationExtended */
__asm__(".globl SteamAPI_ISteamInput_TriggerVibrationExtended");
__asm__("SteamAPI_ISteamInput_TriggerVibrationExtended: jmp *_fwd_SteamAPI_ISteamInput_TriggerVibrationExtended(%rip)");
static void *_fwd_SteamAPI_ISteamInput_TriggerVibrationExtended = NULL;

/* SteamAPI_ISteamInventory_AddPromoItem */
__asm__(".globl SteamAPI_ISteamInventory_AddPromoItem");
__asm__("SteamAPI_ISteamInventory_AddPromoItem: jmp *_fwd_SteamAPI_ISteamInventory_AddPromoItem(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_AddPromoItem = NULL;

/* SteamAPI_ISteamInventory_AddPromoItems */
__asm__(".globl SteamAPI_ISteamInventory_AddPromoItems");
__asm__("SteamAPI_ISteamInventory_AddPromoItems: jmp *_fwd_SteamAPI_ISteamInventory_AddPromoItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_AddPromoItems = NULL;

/* SteamAPI_ISteamInventory_CheckResultSteamID */
__asm__(".globl SteamAPI_ISteamInventory_CheckResultSteamID");
__asm__("SteamAPI_ISteamInventory_CheckResultSteamID: jmp *_fwd_SteamAPI_ISteamInventory_CheckResultSteamID(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_CheckResultSteamID = NULL;

/* SteamAPI_ISteamInventory_ConsumeItem */
__asm__(".globl SteamAPI_ISteamInventory_ConsumeItem");
__asm__("SteamAPI_ISteamInventory_ConsumeItem: jmp *_fwd_SteamAPI_ISteamInventory_ConsumeItem(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_ConsumeItem = NULL;

/* SteamAPI_ISteamInventory_DeserializeResult */
__asm__(".globl SteamAPI_ISteamInventory_DeserializeResult");
__asm__("SteamAPI_ISteamInventory_DeserializeResult: jmp *_fwd_SteamAPI_ISteamInventory_DeserializeResult(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_DeserializeResult = NULL;

/* SteamAPI_ISteamInventory_DestroyResult */
__asm__(".globl SteamAPI_ISteamInventory_DestroyResult");
__asm__("SteamAPI_ISteamInventory_DestroyResult: jmp *_fwd_SteamAPI_ISteamInventory_DestroyResult(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_DestroyResult = NULL;

/* SteamAPI_ISteamInventory_ExchangeItems */
__asm__(".globl SteamAPI_ISteamInventory_ExchangeItems");
__asm__("SteamAPI_ISteamInventory_ExchangeItems: jmp *_fwd_SteamAPI_ISteamInventory_ExchangeItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_ExchangeItems = NULL;

/* SteamAPI_ISteamInventory_GenerateItems */
__asm__(".globl SteamAPI_ISteamInventory_GenerateItems");
__asm__("SteamAPI_ISteamInventory_GenerateItems: jmp *_fwd_SteamAPI_ISteamInventory_GenerateItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GenerateItems = NULL;

/* SteamAPI_ISteamInventory_GetAllItems */
__asm__(".globl SteamAPI_ISteamInventory_GetAllItems");
__asm__("SteamAPI_ISteamInventory_GetAllItems: jmp *_fwd_SteamAPI_ISteamInventory_GetAllItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetAllItems = NULL;

/* SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs */
__asm__(".globl SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs");
__asm__("SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs: jmp *_fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs = NULL;

/* SteamAPI_ISteamInventory_GetItemDefinitionIDs */
__asm__(".globl SteamAPI_ISteamInventory_GetItemDefinitionIDs");
__asm__("SteamAPI_ISteamInventory_GetItemDefinitionIDs: jmp *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs = NULL;

/* SteamAPI_ISteamInventory_GetItemDefinitionProperty */
__asm__(".globl SteamAPI_ISteamInventory_GetItemDefinitionProperty");
__asm__("SteamAPI_ISteamInventory_GetItemDefinitionProperty: jmp *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty = NULL;

/* SteamAPI_ISteamInventory_GetItemPrice */
__asm__(".globl SteamAPI_ISteamInventory_GetItemPrice");
__asm__("SteamAPI_ISteamInventory_GetItemPrice: jmp *_fwd_SteamAPI_ISteamInventory_GetItemPrice(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetItemPrice = NULL;

/* SteamAPI_ISteamInventory_GetItemsByID */
__asm__(".globl SteamAPI_ISteamInventory_GetItemsByID");
__asm__("SteamAPI_ISteamInventory_GetItemsByID: jmp *_fwd_SteamAPI_ISteamInventory_GetItemsByID(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetItemsByID = NULL;

/* SteamAPI_ISteamInventory_GetItemsWithPrices */
__asm__(".globl SteamAPI_ISteamInventory_GetItemsWithPrices");
__asm__("SteamAPI_ISteamInventory_GetItemsWithPrices: jmp *_fwd_SteamAPI_ISteamInventory_GetItemsWithPrices(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetItemsWithPrices = NULL;

/* SteamAPI_ISteamInventory_GetNumItemsWithPrices */
__asm__(".globl SteamAPI_ISteamInventory_GetNumItemsWithPrices");
__asm__("SteamAPI_ISteamInventory_GetNumItemsWithPrices: jmp *_fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices = NULL;

/* SteamAPI_ISteamInventory_GetResultItemProperty */
__asm__(".globl SteamAPI_ISteamInventory_GetResultItemProperty");
__asm__("SteamAPI_ISteamInventory_GetResultItemProperty: jmp *_fwd_SteamAPI_ISteamInventory_GetResultItemProperty(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetResultItemProperty = NULL;

/* SteamAPI_ISteamInventory_GetResultItems */
__asm__(".globl SteamAPI_ISteamInventory_GetResultItems");
__asm__("SteamAPI_ISteamInventory_GetResultItems: jmp *_fwd_SteamAPI_ISteamInventory_GetResultItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetResultItems = NULL;

/* SteamAPI_ISteamInventory_GetResultStatus */
__asm__(".globl SteamAPI_ISteamInventory_GetResultStatus");
__asm__("SteamAPI_ISteamInventory_GetResultStatus: jmp *_fwd_SteamAPI_ISteamInventory_GetResultStatus(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetResultStatus = NULL;

/* SteamAPI_ISteamInventory_GetResultTimestamp */
__asm__(".globl SteamAPI_ISteamInventory_GetResultTimestamp");
__asm__("SteamAPI_ISteamInventory_GetResultTimestamp: jmp *_fwd_SteamAPI_ISteamInventory_GetResultTimestamp(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GetResultTimestamp = NULL;

/* SteamAPI_ISteamInventory_GrantPromoItems */
__asm__(".globl SteamAPI_ISteamInventory_GrantPromoItems");
__asm__("SteamAPI_ISteamInventory_GrantPromoItems: jmp *_fwd_SteamAPI_ISteamInventory_GrantPromoItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_GrantPromoItems = NULL;

/* SteamAPI_ISteamInventory_InspectItem */
__asm__(".globl SteamAPI_ISteamInventory_InspectItem");
__asm__("SteamAPI_ISteamInventory_InspectItem: jmp *_fwd_SteamAPI_ISteamInventory_InspectItem(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_InspectItem = NULL;

/* SteamAPI_ISteamInventory_LoadItemDefinitions */
__asm__(".globl SteamAPI_ISteamInventory_LoadItemDefinitions");
__asm__("SteamAPI_ISteamInventory_LoadItemDefinitions: jmp *_fwd_SteamAPI_ISteamInventory_LoadItemDefinitions(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_LoadItemDefinitions = NULL;

/* SteamAPI_ISteamInventory_RemoveProperty */
__asm__(".globl SteamAPI_ISteamInventory_RemoveProperty");
__asm__("SteamAPI_ISteamInventory_RemoveProperty: jmp *_fwd_SteamAPI_ISteamInventory_RemoveProperty(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_RemoveProperty = NULL;

/* SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs */
__asm__(".globl SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs");
__asm__("SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs: jmp *_fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs = NULL;

/* SteamAPI_ISteamInventory_RequestPrices */
__asm__(".globl SteamAPI_ISteamInventory_RequestPrices");
__asm__("SteamAPI_ISteamInventory_RequestPrices: jmp *_fwd_SteamAPI_ISteamInventory_RequestPrices(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_RequestPrices = NULL;

/* SteamAPI_ISteamInventory_SendItemDropHeartbeat */
__asm__(".globl SteamAPI_ISteamInventory_SendItemDropHeartbeat");
__asm__("SteamAPI_ISteamInventory_SendItemDropHeartbeat: jmp *_fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat = NULL;

/* SteamAPI_ISteamInventory_SerializeResult */
__asm__(".globl SteamAPI_ISteamInventory_SerializeResult");
__asm__("SteamAPI_ISteamInventory_SerializeResult: jmp *_fwd_SteamAPI_ISteamInventory_SerializeResult(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SerializeResult = NULL;

/* SteamAPI_ISteamInventory_SetPropertyBool */
__asm__(".globl SteamAPI_ISteamInventory_SetPropertyBool");
__asm__("SteamAPI_ISteamInventory_SetPropertyBool: jmp *_fwd_SteamAPI_ISteamInventory_SetPropertyBool(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyBool = NULL;

/* SteamAPI_ISteamInventory_SetPropertyFloat */
__asm__(".globl SteamAPI_ISteamInventory_SetPropertyFloat");
__asm__("SteamAPI_ISteamInventory_SetPropertyFloat: jmp *_fwd_SteamAPI_ISteamInventory_SetPropertyFloat(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyFloat = NULL;

/* SteamAPI_ISteamInventory_SetPropertyInt64 */
__asm__(".globl SteamAPI_ISteamInventory_SetPropertyInt64");
__asm__("SteamAPI_ISteamInventory_SetPropertyInt64: jmp *_fwd_SteamAPI_ISteamInventory_SetPropertyInt64(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyInt64 = NULL;

/* SteamAPI_ISteamInventory_SetPropertyString */
__asm__(".globl SteamAPI_ISteamInventory_SetPropertyString");
__asm__("SteamAPI_ISteamInventory_SetPropertyString: jmp *_fwd_SteamAPI_ISteamInventory_SetPropertyString(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyString = NULL;

/* SteamAPI_ISteamInventory_StartPurchase */
__asm__(".globl SteamAPI_ISteamInventory_StartPurchase");
__asm__("SteamAPI_ISteamInventory_StartPurchase: jmp *_fwd_SteamAPI_ISteamInventory_StartPurchase(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_StartPurchase = NULL;

/* SteamAPI_ISteamInventory_StartUpdateProperties */
__asm__(".globl SteamAPI_ISteamInventory_StartUpdateProperties");
__asm__("SteamAPI_ISteamInventory_StartUpdateProperties: jmp *_fwd_SteamAPI_ISteamInventory_StartUpdateProperties(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_StartUpdateProperties = NULL;

/* SteamAPI_ISteamInventory_SubmitUpdateProperties */
__asm__(".globl SteamAPI_ISteamInventory_SubmitUpdateProperties");
__asm__("SteamAPI_ISteamInventory_SubmitUpdateProperties: jmp *_fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties = NULL;

/* SteamAPI_ISteamInventory_TradeItems */
__asm__(".globl SteamAPI_ISteamInventory_TradeItems");
__asm__("SteamAPI_ISteamInventory_TradeItems: jmp *_fwd_SteamAPI_ISteamInventory_TradeItems(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_TradeItems = NULL;

/* SteamAPI_ISteamInventory_TransferItemQuantity */
__asm__(".globl SteamAPI_ISteamInventory_TransferItemQuantity");
__asm__("SteamAPI_ISteamInventory_TransferItemQuantity: jmp *_fwd_SteamAPI_ISteamInventory_TransferItemQuantity(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_TransferItemQuantity = NULL;

/* SteamAPI_ISteamInventory_TriggerItemDrop */
__asm__(".globl SteamAPI_ISteamInventory_TriggerItemDrop");
__asm__("SteamAPI_ISteamInventory_TriggerItemDrop: jmp *_fwd_SteamAPI_ISteamInventory_TriggerItemDrop(%rip)");
static void *_fwd_SteamAPI_ISteamInventory_TriggerItemDrop = NULL;

/* SteamAPI_ISteamMatchmaking_AddFavoriteGame */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddFavoriteGame");
__asm__("SteamAPI_ISteamMatchmaking_AddFavoriteGame: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter = NULL;

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter */
__asm__(".globl SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
__asm__("SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter: jmp *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter = NULL;

/* SteamAPI_ISteamMatchmaking_CreateLobby */
__asm__(".globl SteamAPI_ISteamMatchmaking_CreateLobby");
__asm__("SteamAPI_ISteamMatchmaking_CreateLobby: jmp *_fwd_SteamAPI_ISteamMatchmaking_CreateLobby(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_CreateLobby = NULL;

/* SteamAPI_ISteamMatchmaking_DeleteLobbyData */
__asm__(".globl SteamAPI_ISteamMatchmaking_DeleteLobbyData");
__asm__("SteamAPI_ISteamMatchmaking_DeleteLobbyData: jmp *_fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData = NULL;

/* SteamAPI_ISteamMatchmaking_GetFavoriteGame */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetFavoriteGame");
__asm__("SteamAPI_ISteamMatchmaking_GetFavoriteGame: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame = NULL;

/* SteamAPI_ISteamMatchmaking_GetFavoriteGameCount */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetFavoriteGameCount");
__asm__("SteamAPI_ISteamMatchmaking_GetFavoriteGameCount: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyByIndex */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyByIndex: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyChatEntry */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyChatEntry: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyData */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyData");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyData: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyData = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyDataCount */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyDataCount");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyDataCount: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyGameServer */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyGameServer");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyGameServer: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberData */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyMemberData: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit = NULL;

/* SteamAPI_ISteamMatchmaking_GetLobbyOwner */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetLobbyOwner");
__asm__("SteamAPI_ISteamMatchmaking_GetLobbyOwner: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner = NULL;

/* SteamAPI_ISteamMatchmaking_GetNumLobbyMembers */
__asm__(".globl SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
__asm__("SteamAPI_ISteamMatchmaking_GetNumLobbyMembers: jmp *_fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers = NULL;

/* SteamAPI_ISteamMatchmaking_InviteUserToLobby */
__asm__(".globl SteamAPI_ISteamMatchmaking_InviteUserToLobby");
__asm__("SteamAPI_ISteamMatchmaking_InviteUserToLobby: jmp *_fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby = NULL;

/* SteamAPI_ISteamMatchmaking_JoinLobby */
__asm__(".globl SteamAPI_ISteamMatchmaking_JoinLobby");
__asm__("SteamAPI_ISteamMatchmaking_JoinLobby: jmp *_fwd_SteamAPI_ISteamMatchmaking_JoinLobby(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_JoinLobby = NULL;

/* SteamAPI_ISteamMatchmaking_LeaveLobby */
__asm__(".globl SteamAPI_ISteamMatchmaking_LeaveLobby");
__asm__("SteamAPI_ISteamMatchmaking_LeaveLobby: jmp *_fwd_SteamAPI_ISteamMatchmaking_LeaveLobby(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_LeaveLobby = NULL;

/* SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond */
__asm__(".globl SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond");
__asm__("SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond: jmp *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond = NULL;

/* SteamAPI_ISteamMatchmakingPingResponse_ServerResponded */
__asm__(".globl SteamAPI_ISteamMatchmakingPingResponse_ServerResponded");
__asm__("SteamAPI_ISteamMatchmakingPingResponse_ServerResponded: jmp *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded = NULL;

/* SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList */
__asm__(".globl SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList");
__asm__("SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList: jmp *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList = NULL;

/* SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond */
__asm__(".globl SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond");
__asm__("SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond: jmp *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond = NULL;

/* SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete */
__asm__(".globl SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete");
__asm__("SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete: jmp *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete = NULL;

/* SteamAPI_ISteamMatchmaking_RemoveFavoriteGame */
__asm__(".globl SteamAPI_ISteamMatchmaking_RemoveFavoriteGame");
__asm__("SteamAPI_ISteamMatchmaking_RemoveFavoriteGame: jmp *_fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame = NULL;

/* SteamAPI_ISteamMatchmaking_RequestLobbyData */
__asm__(".globl SteamAPI_ISteamMatchmaking_RequestLobbyData");
__asm__("SteamAPI_ISteamMatchmaking_RequestLobbyData: jmp *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData = NULL;

/* SteamAPI_ISteamMatchmaking_RequestLobbyList */
__asm__(".globl SteamAPI_ISteamMatchmaking_RequestLobbyList");
__asm__("SteamAPI_ISteamMatchmaking_RequestLobbyList: jmp *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList = NULL;

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond */
__asm__(".globl SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond");
__asm__("SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond: jmp *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond = NULL;

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete */
__asm__(".globl SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete");
__asm__("SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete: jmp *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete = NULL;

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded */
__asm__(".globl SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded");
__asm__("SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded: jmp *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded = NULL;

/* SteamAPI_ISteamMatchmaking_SendLobbyChatMsg */
__asm__(".globl SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
__asm__("SteamAPI_ISteamMatchmaking_SendLobbyChatMsg: jmp *_fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg = NULL;

/* SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete */
__asm__(".globl SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete");
__asm__("SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete: jmp *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete = NULL;

/* SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond */
__asm__(".globl SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond");
__asm__("SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond: jmp *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond = NULL;

/* SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded */
__asm__(".globl SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded");
__asm__("SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded: jmp *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded = NULL;

/* SteamAPI_ISteamMatchmakingServers_CancelQuery */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_CancelQuery");
__asm__("SteamAPI_ISteamMatchmakingServers_CancelQuery: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery = NULL;

/* SteamAPI_ISteamMatchmakingServers_CancelServerQuery */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_CancelServerQuery");
__asm__("SteamAPI_ISteamMatchmakingServers_CancelServerQuery: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery = NULL;

/* SteamAPI_ISteamMatchmakingServers_GetServerCount */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_GetServerCount");
__asm__("SteamAPI_ISteamMatchmakingServers_GetServerCount: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount = NULL;

/* SteamAPI_ISteamMatchmakingServers_GetServerDetails */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_GetServerDetails");
__asm__("SteamAPI_ISteamMatchmakingServers_GetServerDetails: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails = NULL;

/* SteamAPI_ISteamMatchmakingServers_IsRefreshing */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_IsRefreshing");
__asm__("SteamAPI_ISteamMatchmakingServers_IsRefreshing: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing = NULL;

/* SteamAPI_ISteamMatchmakingServers_PingServer */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_PingServer");
__asm__("SteamAPI_ISteamMatchmakingServers_PingServer: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_PingServer(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_PingServer = NULL;

/* SteamAPI_ISteamMatchmakingServers_PlayerDetails */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_PlayerDetails");
__asm__("SteamAPI_ISteamMatchmakingServers_PlayerDetails: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails = NULL;

/* SteamAPI_ISteamMatchmakingServers_RefreshQuery */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RefreshQuery");
__asm__("SteamAPI_ISteamMatchmakingServers_RefreshQuery: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery = NULL;

/* SteamAPI_ISteamMatchmakingServers_RefreshServer */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RefreshServer");
__asm__("SteamAPI_ISteamMatchmakingServers_RefreshServer: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer = NULL;

/* SteamAPI_ISteamMatchmakingServers_ReleaseRequest */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_ReleaseRequest");
__asm__("SteamAPI_ISteamMatchmakingServers_ReleaseRequest: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestInternetServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestInternetServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestInternetServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestLANServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestLANServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestLANServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList");
__asm__("SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList = NULL;

/* SteamAPI_ISteamMatchmakingServers_ServerRules */
__asm__(".globl SteamAPI_ISteamMatchmakingServers_ServerRules");
__asm__("SteamAPI_ISteamMatchmakingServers_ServerRules: jmp *_fwd_SteamAPI_ISteamMatchmakingServers_ServerRules(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmakingServers_ServerRules = NULL;

/* SteamAPI_ISteamMatchmaking_SetLinkedLobby */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLinkedLobby");
__asm__("SteamAPI_ISteamMatchmaking_SetLinkedLobby: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyData */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyData");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyData: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyData = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyGameServer */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyGameServer");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyGameServer: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyJoinable */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyJoinable: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyMemberData */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyMemberData: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyOwner */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyOwner");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyOwner: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner = NULL;

/* SteamAPI_ISteamMatchmaking_SetLobbyType */
__asm__(".globl SteamAPI_ISteamMatchmaking_SetLobbyType");
__asm__("SteamAPI_ISteamMatchmaking_SetLobbyType: jmp *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyType(%rip)");
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyType = NULL;

/* SteamAPI_ISteamMusic_BIsEnabled */
__asm__(".globl SteamAPI_ISteamMusic_BIsEnabled");
__asm__("SteamAPI_ISteamMusic_BIsEnabled: jmp *_fwd_SteamAPI_ISteamMusic_BIsEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_BIsEnabled = NULL;

/* SteamAPI_ISteamMusic_BIsPlaying */
__asm__(".globl SteamAPI_ISteamMusic_BIsPlaying");
__asm__("SteamAPI_ISteamMusic_BIsPlaying: jmp *_fwd_SteamAPI_ISteamMusic_BIsPlaying(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_BIsPlaying = NULL;

/* SteamAPI_ISteamMusic_GetPlaybackStatus */
__asm__(".globl SteamAPI_ISteamMusic_GetPlaybackStatus");
__asm__("SteamAPI_ISteamMusic_GetPlaybackStatus: jmp *_fwd_SteamAPI_ISteamMusic_GetPlaybackStatus(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_GetPlaybackStatus = NULL;

/* SteamAPI_ISteamMusic_GetVolume */
__asm__(".globl SteamAPI_ISteamMusic_GetVolume");
__asm__("SteamAPI_ISteamMusic_GetVolume: jmp *_fwd_SteamAPI_ISteamMusic_GetVolume(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_GetVolume = NULL;

/* SteamAPI_ISteamMusic_Pause */
__asm__(".globl SteamAPI_ISteamMusic_Pause");
__asm__("SteamAPI_ISteamMusic_Pause: jmp *_fwd_SteamAPI_ISteamMusic_Pause(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_Pause = NULL;

/* SteamAPI_ISteamMusic_Play */
__asm__(".globl SteamAPI_ISteamMusic_Play");
__asm__("SteamAPI_ISteamMusic_Play: jmp *_fwd_SteamAPI_ISteamMusic_Play(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_Play = NULL;

/* SteamAPI_ISteamMusic_PlayNext */
__asm__(".globl SteamAPI_ISteamMusic_PlayNext");
__asm__("SteamAPI_ISteamMusic_PlayNext: jmp *_fwd_SteamAPI_ISteamMusic_PlayNext(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_PlayNext = NULL;

/* SteamAPI_ISteamMusic_PlayPrevious */
__asm__(".globl SteamAPI_ISteamMusic_PlayPrevious");
__asm__("SteamAPI_ISteamMusic_PlayPrevious: jmp *_fwd_SteamAPI_ISteamMusic_PlayPrevious(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_PlayPrevious = NULL;

/* SteamAPI_ISteamMusicRemote_BActivationSuccess */
__asm__(".globl SteamAPI_ISteamMusicRemote_BActivationSuccess");
__asm__("SteamAPI_ISteamMusicRemote_BActivationSuccess: jmp *_fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess = NULL;

/* SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote */
__asm__(".globl SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote");
__asm__("SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote: jmp *_fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote = NULL;

/* SteamAPI_ISteamMusicRemote_CurrentEntryDidChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_CurrentEntryDidChange");
__asm__("SteamAPI_ISteamMusicRemote_CurrentEntryDidChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange = NULL;

/* SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable */
__asm__(".globl SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable");
__asm__("SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable: jmp *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable = NULL;

/* SteamAPI_ISteamMusicRemote_CurrentEntryWillChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_CurrentEntryWillChange");
__asm__("SteamAPI_ISteamMusicRemote_CurrentEntryWillChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange = NULL;

/* SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote */
__asm__(".globl SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote");
__asm__("SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote: jmp *_fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote = NULL;

/* SteamAPI_ISteamMusicRemote_EnableLooped */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnableLooped");
__asm__("SteamAPI_ISteamMusicRemote_EnableLooped: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnableLooped(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableLooped = NULL;

/* SteamAPI_ISteamMusicRemote_EnablePlaylists */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnablePlaylists");
__asm__("SteamAPI_ISteamMusicRemote_EnablePlaylists: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists = NULL;

/* SteamAPI_ISteamMusicRemote_EnablePlayNext */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnablePlayNext");
__asm__("SteamAPI_ISteamMusicRemote_EnablePlayNext: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext = NULL;

/* SteamAPI_ISteamMusicRemote_EnablePlayPrevious */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnablePlayPrevious");
__asm__("SteamAPI_ISteamMusicRemote_EnablePlayPrevious: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious = NULL;

/* SteamAPI_ISteamMusicRemote_EnableQueue */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnableQueue");
__asm__("SteamAPI_ISteamMusicRemote_EnableQueue: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnableQueue(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableQueue = NULL;

/* SteamAPI_ISteamMusicRemote_EnableShuffled */
__asm__(".globl SteamAPI_ISteamMusicRemote_EnableShuffled");
__asm__("SteamAPI_ISteamMusicRemote_EnableShuffled: jmp *_fwd_SteamAPI_ISteamMusicRemote_EnableShuffled(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableShuffled = NULL;

/* SteamAPI_ISteamMusicRemote_PlaylistDidChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_PlaylistDidChange");
__asm__("SteamAPI_ISteamMusicRemote_PlaylistDidChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange = NULL;

/* SteamAPI_ISteamMusicRemote_PlaylistWillChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_PlaylistWillChange");
__asm__("SteamAPI_ISteamMusicRemote_PlaylistWillChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange = NULL;

/* SteamAPI_ISteamMusicRemote_QueueDidChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_QueueDidChange");
__asm__("SteamAPI_ISteamMusicRemote_QueueDidChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_QueueDidChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_QueueDidChange = NULL;

/* SteamAPI_ISteamMusicRemote_QueueWillChange */
__asm__(".globl SteamAPI_ISteamMusicRemote_QueueWillChange");
__asm__("SteamAPI_ISteamMusicRemote_QueueWillChange: jmp *_fwd_SteamAPI_ISteamMusicRemote_QueueWillChange(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_QueueWillChange = NULL;

/* SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote */
__asm__(".globl SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote");
__asm__("SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote: jmp *_fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote = NULL;

/* SteamAPI_ISteamMusicRemote_ResetPlaylistEntries */
__asm__(".globl SteamAPI_ISteamMusicRemote_ResetPlaylistEntries");
__asm__("SteamAPI_ISteamMusicRemote_ResetPlaylistEntries: jmp *_fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries = NULL;

/* SteamAPI_ISteamMusicRemote_ResetQueueEntries */
__asm__(".globl SteamAPI_ISteamMusicRemote_ResetQueueEntries");
__asm__("SteamAPI_ISteamMusicRemote_ResetQueueEntries: jmp *_fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries = NULL;

/* SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry");
__asm__("SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry = NULL;

/* SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry");
__asm__("SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry = NULL;

/* SteamAPI_ISteamMusicRemote_SetDisplayName */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetDisplayName");
__asm__("SteamAPI_ISteamMusicRemote_SetDisplayName: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetDisplayName(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetDisplayName = NULL;

/* SteamAPI_ISteamMusicRemote_SetPlaylistEntry */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetPlaylistEntry");
__asm__("SteamAPI_ISteamMusicRemote_SetPlaylistEntry: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry = NULL;

/* SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64");
__asm__("SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 = NULL;

/* SteamAPI_ISteamMusicRemote_SetQueueEntry */
__asm__(".globl SteamAPI_ISteamMusicRemote_SetQueueEntry");
__asm__("SteamAPI_ISteamMusicRemote_SetQueueEntry: jmp *_fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt");
__asm__("SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds");
__asm__("SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText");
__asm__("SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateLooped */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateLooped");
__asm__("SteamAPI_ISteamMusicRemote_UpdateLooped: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateLooped(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateLooped = NULL;

/* SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus");
__asm__("SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateShuffled */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateShuffled");
__asm__("SteamAPI_ISteamMusicRemote_UpdateShuffled: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled = NULL;

/* SteamAPI_ISteamMusicRemote_UpdateVolume */
__asm__(".globl SteamAPI_ISteamMusicRemote_UpdateVolume");
__asm__("SteamAPI_ISteamMusicRemote_UpdateVolume: jmp *_fwd_SteamAPI_ISteamMusicRemote_UpdateVolume(%rip)");
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateVolume = NULL;

/* SteamAPI_ISteamMusic_SetVolume */
__asm__(".globl SteamAPI_ISteamMusic_SetVolume");
__asm__("SteamAPI_ISteamMusic_SetVolume: jmp *_fwd_SteamAPI_ISteamMusic_SetVolume(%rip)");
static void *_fwd_SteamAPI_ISteamMusic_SetVolume = NULL;

/* SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser */
__asm__(".globl SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
__asm__("SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser: jmp *_fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser = NULL;

/* SteamAPI_ISteamNetworking_AllowP2PPacketRelay */
__asm__(".globl SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
__asm__("SteamAPI_ISteamNetworking_AllowP2PPacketRelay: jmp *_fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay = NULL;

/* SteamAPI_ISteamNetworking_CloseP2PChannelWithUser */
__asm__(".globl SteamAPI_ISteamNetworking_CloseP2PChannelWithUser");
__asm__("SteamAPI_ISteamNetworking_CloseP2PChannelWithUser: jmp *_fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser = NULL;

/* SteamAPI_ISteamNetworking_CloseP2PSessionWithUser */
__asm__(".globl SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
__asm__("SteamAPI_ISteamNetworking_CloseP2PSessionWithUser: jmp *_fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser = NULL;

/* SteamAPI_ISteamNetworking_CreateConnectionSocket */
__asm__(".globl SteamAPI_ISteamNetworking_CreateConnectionSocket");
__asm__("SteamAPI_ISteamNetworking_CreateConnectionSocket: jmp *_fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket = NULL;

/* SteamAPI_ISteamNetworking_CreateListenSocket */
__asm__(".globl SteamAPI_ISteamNetworking_CreateListenSocket");
__asm__("SteamAPI_ISteamNetworking_CreateListenSocket: jmp *_fwd_SteamAPI_ISteamNetworking_CreateListenSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_CreateListenSocket = NULL;

/* SteamAPI_ISteamNetworking_CreateP2PConnectionSocket */
__asm__(".globl SteamAPI_ISteamNetworking_CreateP2PConnectionSocket");
__asm__("SteamAPI_ISteamNetworking_CreateP2PConnectionSocket: jmp *_fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket = NULL;

/* SteamAPI_ISteamNetworking_DestroyListenSocket */
__asm__(".globl SteamAPI_ISteamNetworking_DestroyListenSocket");
__asm__("SteamAPI_ISteamNetworking_DestroyListenSocket: jmp *_fwd_SteamAPI_ISteamNetworking_DestroyListenSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_DestroyListenSocket = NULL;

/* SteamAPI_ISteamNetworking_DestroySocket */
__asm__(".globl SteamAPI_ISteamNetworking_DestroySocket");
__asm__("SteamAPI_ISteamNetworking_DestroySocket: jmp *_fwd_SteamAPI_ISteamNetworking_DestroySocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_DestroySocket = NULL;

/* SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort */
__asm__(".globl SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort");
__asm__("SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort: jmp *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort = NULL;

/* SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages */
__asm__(".globl SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages");
__asm__("SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages: jmp *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages = NULL;

/* SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup */
__asm__(".globl SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup");
__asm__("SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup: jmp *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup = NULL;

/* SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP */
__asm__(".globl SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP");
__asm__("SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP: jmp *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP = NULL;

/* SteamAPI_ISteamNetworking_GetListenSocketInfo */
__asm__(".globl SteamAPI_ISteamNetworking_GetListenSocketInfo");
__asm__("SteamAPI_ISteamNetworking_GetListenSocketInfo: jmp *_fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo = NULL;

/* SteamAPI_ISteamNetworking_GetMaxPacketSize */
__asm__(".globl SteamAPI_ISteamNetworking_GetMaxPacketSize");
__asm__("SteamAPI_ISteamNetworking_GetMaxPacketSize: jmp *_fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize = NULL;

/* SteamAPI_ISteamNetworking_GetP2PSessionState */
__asm__(".globl SteamAPI_ISteamNetworking_GetP2PSessionState");
__asm__("SteamAPI_ISteamNetworking_GetP2PSessionState: jmp *_fwd_SteamAPI_ISteamNetworking_GetP2PSessionState(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_GetP2PSessionState = NULL;

/* SteamAPI_ISteamNetworking_GetSocketConnectionType */
__asm__(".globl SteamAPI_ISteamNetworking_GetSocketConnectionType");
__asm__("SteamAPI_ISteamNetworking_GetSocketConnectionType: jmp *_fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType = NULL;

/* SteamAPI_ISteamNetworking_GetSocketInfo */
__asm__(".globl SteamAPI_ISteamNetworking_GetSocketInfo");
__asm__("SteamAPI_ISteamNetworking_GetSocketInfo: jmp *_fwd_SteamAPI_ISteamNetworking_GetSocketInfo(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_GetSocketInfo = NULL;

/* SteamAPI_ISteamNetworking_IsDataAvailable */
__asm__(".globl SteamAPI_ISteamNetworking_IsDataAvailable");
__asm__("SteamAPI_ISteamNetworking_IsDataAvailable: jmp *_fwd_SteamAPI_ISteamNetworking_IsDataAvailable(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_IsDataAvailable = NULL;

/* SteamAPI_ISteamNetworking_IsDataAvailableOnSocket */
__asm__(".globl SteamAPI_ISteamNetworking_IsDataAvailableOnSocket");
__asm__("SteamAPI_ISteamNetworking_IsDataAvailableOnSocket: jmp *_fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket = NULL;

/* SteamAPI_ISteamNetworking_IsP2PPacketAvailable */
__asm__(".globl SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
__asm__("SteamAPI_ISteamNetworking_IsP2PPacketAvailable: jmp *_fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable = NULL;

/* SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
__asm__("SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser = NULL;

/* SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser");
__asm__("SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser = NULL;

/* SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
__asm__("SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser = NULL;

/* SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
__asm__("SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo = NULL;

/* SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
__asm__("SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel = NULL;

/* SteamAPI_ISteamNetworkingMessages_SendMessageToUser */
__asm__(".globl SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
__asm__("SteamAPI_ISteamNetworkingMessages_SendMessageToUser: jmp *_fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser = NULL;

/* SteamAPI_ISteamNetworking_ReadP2PPacket */
__asm__(".globl SteamAPI_ISteamNetworking_ReadP2PPacket");
__asm__("SteamAPI_ISteamNetworking_ReadP2PPacket: jmp *_fwd_SteamAPI_ISteamNetworking_ReadP2PPacket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_ReadP2PPacket = NULL;

/* SteamAPI_ISteamNetworking_RetrieveData */
__asm__(".globl SteamAPI_ISteamNetworking_RetrieveData");
__asm__("SteamAPI_ISteamNetworking_RetrieveData: jmp *_fwd_SteamAPI_ISteamNetworking_RetrieveData(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_RetrieveData = NULL;

/* SteamAPI_ISteamNetworking_RetrieveDataFromSocket */
__asm__(".globl SteamAPI_ISteamNetworking_RetrieveDataFromSocket");
__asm__("SteamAPI_ISteamNetworking_RetrieveDataFromSocket: jmp *_fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket = NULL;

/* SteamAPI_ISteamNetworking_SendDataOnSocket */
__asm__(".globl SteamAPI_ISteamNetworking_SendDataOnSocket");
__asm__("SteamAPI_ISteamNetworking_SendDataOnSocket: jmp *_fwd_SteamAPI_ISteamNetworking_SendDataOnSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_SendDataOnSocket = NULL;

/* SteamAPI_ISteamNetworking_SendP2PPacket */
__asm__(".globl SteamAPI_ISteamNetworking_SendP2PPacket");
__asm__("SteamAPI_ISteamNetworking_SendP2PPacket: jmp *_fwd_SteamAPI_ISteamNetworking_SendP2PPacket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworking_SendP2PPacket = NULL;

/* SteamAPI_ISteamNetworkingSockets_AcceptConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_AcceptConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_AcceptConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP");
__asm__("SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP = NULL;

/* SteamAPI_ISteamNetworkingSockets_CloseConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CloseConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_CloseConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_CloseListenSocket */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
__asm__("SteamAPI_ISteamNetworkingSockets_CloseListenSocket: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket = NULL;

/* SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes");
__asm__("SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes = NULL;

/* SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress");
__asm__("SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress = NULL;

/* SteamAPI_ISteamNetworkingSockets_ConnectP2P */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ConnectP2P");
__asm__("SteamAPI_ISteamNetworkingSockets_ConnectP2P: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P = NULL;

/* SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling");
__asm__("SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling = NULL;

/* SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer");
__asm__("SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreatePollGroup */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
__asm__("SteamAPI_ISteamNetworkingSockets_CreatePollGroup: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup = NULL;

/* SteamAPI_ISteamNetworkingSockets_CreateSocketPair */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_CreateSocketPair");
__asm__("SteamAPI_ISteamNetworkingSockets_CreateSocketPair: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair = NULL;

/* SteamAPI_ISteamNetworkingSockets_DestroyPollGroup */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
__asm__("SteamAPI_ISteamNetworkingSockets_DestroyPollGroup: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup = NULL;

/* SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer");
__asm__("SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer = NULL;

/* SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");
__asm__("SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetCertificateRequest */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetCertificateRequest");
__asm__("SteamAPI_ISteamNetworkingSockets_GetCertificateRequest: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetConnectionInfo */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
__asm__("SteamAPI_ISteamNetworkingSockets_GetConnectionInfo: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetConnectionName */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetConnectionName");
__asm__("SteamAPI_ISteamNetworkingSockets_GetConnectionName: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
__asm__("SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetConnectionUserData */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetConnectionUserData");
__asm__("SteamAPI_ISteamNetworkingSockets_GetConnectionUserData: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus");
__asm__("SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetFakeIP */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetFakeIP");
__asm__("SteamAPI_ISteamNetworkingSockets_GetFakeIP: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin");
__asm__("SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress");
__asm__("SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID");
__asm__("SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort");
__asm__("SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetIdentity */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetIdentity");
__asm__("SteamAPI_ISteamNetworkingSockets_GetIdentity: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress");
__asm__("SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress = NULL;

/* SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_InitAuthentication */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_InitAuthentication");
__asm__("SteamAPI_ISteamNetworkingSockets_InitAuthentication: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication = NULL;

/* SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal");
__asm__("SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal = NULL;

/* SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket");
__asm__("SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket = NULL;

/* SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
__asm__("SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup = NULL;

/* SteamAPI_ISteamNetworkingSockets_ResetIdentity */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_ResetIdentity");
__asm__("SteamAPI_ISteamNetworkingSockets_ResetIdentity: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity = NULL;

/* SteamAPI_ISteamNetworkingSockets_RunCallbacks */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_RunCallbacks");
__asm__("SteamAPI_ISteamNetworkingSockets_RunCallbacks: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks = NULL;

/* SteamAPI_ISteamNetworkingSockets_SendMessages */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SendMessages");
__asm__("SteamAPI_ISteamNetworkingSockets_SendMessages: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessages(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessages = NULL;

/* SteamAPI_ISteamNetworkingSockets_SendMessageToConnection */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
__asm__("SteamAPI_ISteamNetworkingSockets_SendMessageToConnection: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection = NULL;

/* SteamAPI_ISteamNetworkingSockets_SetCertificate */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SetCertificate");
__asm__("SteamAPI_ISteamNetworkingSockets_SetCertificate: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate = NULL;

/* SteamAPI_ISteamNetworkingSockets_SetConnectionName */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SetConnectionName");
__asm__("SteamAPI_ISteamNetworkingSockets_SetConnectionName: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName = NULL;

/* SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
__asm__("SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup = NULL;

/* SteamAPI_ISteamNetworkingSockets_SetConnectionUserData */
__asm__(".globl SteamAPI_ISteamNetworkingSockets_SetConnectionUserData");
__asm__("SteamAPI_ISteamNetworkingSockets_SetConnectionUserData: jmp *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData = NULL;

/* SteamAPI_ISteamNetworkingUtils_AllocateMessage */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_AllocateMessage");
__asm__("SteamAPI_ISteamNetworkingUtils_AllocateMessage: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage = NULL;

/* SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate");
__asm__("SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate = NULL;

/* SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString");
__asm__("SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString = NULL;

/* SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations");
__asm__("SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations = NULL;

/* SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost");
__asm__("SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetConfigValue */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetConfigValue");
__asm__("SteamAPI_ISteamNetworkingUtils_GetConfigValue: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo");
__asm__("SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP");
__asm__("SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType");
__asm__("SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation");
__asm__("SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp");
__asm__("SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter");
__asm__("SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetPOPCount */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetPOPCount");
__asm__("SteamAPI_ISteamNetworkingUtils_GetPOPCount: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetPOPList */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetPOPList");
__asm__("SteamAPI_ISteamNetworkingUtils_GetPOPList: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP");
__asm__("SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP = NULL;

/* SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
__asm__("SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus = NULL;

/* SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
__asm__("SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess = NULL;

/* SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_IsFakeIPv4");
__asm__("SteamAPI_ISteamNetworkingUtils_IsFakeIPv4: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 = NULL;

/* SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues");
__asm__("SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues = NULL;

/* SteamAPI_ISteamNetworkingUtils_ParsePingLocationString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_ParsePingLocationString");
__asm__("SteamAPI_ISteamNetworkingUtils_ParsePingLocationString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetConfigValue */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetConfigValue");
__asm__("SteamAPI_ISteamNetworkingUtils_SetConfigValue: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct");
__asm__("SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat");
__asm__("SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32");
__asm__("SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString");
__asm__("SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction");
__asm__("SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr = NULL;

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString");
__asm__("SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString");
__asm__("SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString");
__asm__("SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType");
__asm__("SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType = NULL;

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString");
__asm__("SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString = NULL;

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString */
__asm__(".globl SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString");
__asm__("SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString: jmp *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString(%rip)");
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString = NULL;

/* SteamAPI_ISteamParentalSettings_BIsAppBlocked */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsAppBlocked");
__asm__("SteamAPI_ISteamParentalSettings_BIsAppBlocked: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked = NULL;

/* SteamAPI_ISteamParentalSettings_BIsAppInBlockList */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsAppInBlockList");
__asm__("SteamAPI_ISteamParentalSettings_BIsAppInBlockList: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList = NULL;

/* SteamAPI_ISteamParentalSettings_BIsFeatureBlocked */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsFeatureBlocked");
__asm__("SteamAPI_ISteamParentalSettings_BIsFeatureBlocked: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked = NULL;

/* SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList");
__asm__("SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList = NULL;

/* SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled");
__asm__("SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled = NULL;

/* SteamAPI_ISteamParentalSettings_BIsParentalLockLocked */
__asm__(".globl SteamAPI_ISteamParentalSettings_BIsParentalLockLocked");
__asm__("SteamAPI_ISteamParentalSettings_BIsParentalLockLocked: jmp *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked(%rip)");
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked = NULL;

/* SteamAPI_ISteamParties_CancelReservation */
__asm__(".globl SteamAPI_ISteamParties_CancelReservation");
__asm__("SteamAPI_ISteamParties_CancelReservation: jmp *_fwd_SteamAPI_ISteamParties_CancelReservation(%rip)");
static void *_fwd_SteamAPI_ISteamParties_CancelReservation = NULL;

/* SteamAPI_ISteamParties_ChangeNumOpenSlots */
__asm__(".globl SteamAPI_ISteamParties_ChangeNumOpenSlots");
__asm__("SteamAPI_ISteamParties_ChangeNumOpenSlots: jmp *_fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots(%rip)");
static void *_fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots = NULL;

/* SteamAPI_ISteamParties_CreateBeacon */
__asm__(".globl SteamAPI_ISteamParties_CreateBeacon");
__asm__("SteamAPI_ISteamParties_CreateBeacon: jmp *_fwd_SteamAPI_ISteamParties_CreateBeacon(%rip)");
static void *_fwd_SteamAPI_ISteamParties_CreateBeacon = NULL;

/* SteamAPI_ISteamParties_DestroyBeacon */
__asm__(".globl SteamAPI_ISteamParties_DestroyBeacon");
__asm__("SteamAPI_ISteamParties_DestroyBeacon: jmp *_fwd_SteamAPI_ISteamParties_DestroyBeacon(%rip)");
static void *_fwd_SteamAPI_ISteamParties_DestroyBeacon = NULL;

/* SteamAPI_ISteamParties_GetAvailableBeaconLocations */
__asm__(".globl SteamAPI_ISteamParties_GetAvailableBeaconLocations");
__asm__("SteamAPI_ISteamParties_GetAvailableBeaconLocations: jmp *_fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations = NULL;

/* SteamAPI_ISteamParties_GetBeaconByIndex */
__asm__(".globl SteamAPI_ISteamParties_GetBeaconByIndex");
__asm__("SteamAPI_ISteamParties_GetBeaconByIndex: jmp *_fwd_SteamAPI_ISteamParties_GetBeaconByIndex(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetBeaconByIndex = NULL;

/* SteamAPI_ISteamParties_GetBeaconDetails */
__asm__(".globl SteamAPI_ISteamParties_GetBeaconDetails");
__asm__("SteamAPI_ISteamParties_GetBeaconDetails: jmp *_fwd_SteamAPI_ISteamParties_GetBeaconDetails(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetBeaconDetails = NULL;

/* SteamAPI_ISteamParties_GetBeaconLocationData */
__asm__(".globl SteamAPI_ISteamParties_GetBeaconLocationData");
__asm__("SteamAPI_ISteamParties_GetBeaconLocationData: jmp *_fwd_SteamAPI_ISteamParties_GetBeaconLocationData(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetBeaconLocationData = NULL;

/* SteamAPI_ISteamParties_GetNumActiveBeacons */
__asm__(".globl SteamAPI_ISteamParties_GetNumActiveBeacons");
__asm__("SteamAPI_ISteamParties_GetNumActiveBeacons: jmp *_fwd_SteamAPI_ISteamParties_GetNumActiveBeacons(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetNumActiveBeacons = NULL;

/* SteamAPI_ISteamParties_GetNumAvailableBeaconLocations */
__asm__(".globl SteamAPI_ISteamParties_GetNumAvailableBeaconLocations");
__asm__("SteamAPI_ISteamParties_GetNumAvailableBeaconLocations: jmp *_fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations(%rip)");
static void *_fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations = NULL;

/* SteamAPI_ISteamParties_JoinParty */
__asm__(".globl SteamAPI_ISteamParties_JoinParty");
__asm__("SteamAPI_ISteamParties_JoinParty: jmp *_fwd_SteamAPI_ISteamParties_JoinParty(%rip)");
static void *_fwd_SteamAPI_ISteamParties_JoinParty = NULL;

/* SteamAPI_ISteamParties_OnReservationCompleted */
__asm__(".globl SteamAPI_ISteamParties_OnReservationCompleted");
__asm__("SteamAPI_ISteamParties_OnReservationCompleted: jmp *_fwd_SteamAPI_ISteamParties_OnReservationCompleted(%rip)");
static void *_fwd_SteamAPI_ISteamParties_OnReservationCompleted = NULL;

/* SteamAPI_ISteamRemotePlay_BGetSessionClientResolution */
__asm__(".globl SteamAPI_ISteamRemotePlay_BGetSessionClientResolution");
__asm__("SteamAPI_ISteamRemotePlay_BGetSessionClientResolution: jmp *_fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution = NULL;

/* SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite */
__asm__(".globl SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite");
__asm__("SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite: jmp *_fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite = NULL;

/* SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether */
__asm__(".globl SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether");
__asm__("SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether: jmp *_fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether = NULL;

/* SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor */
__asm__(".globl SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor");
__asm__("SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor: jmp *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor = NULL;

/* SteamAPI_ISteamRemotePlay_GetSessionClientName */
__asm__(".globl SteamAPI_ISteamRemotePlay_GetSessionClientName");
__asm__("SteamAPI_ISteamRemotePlay_GetSessionClientName: jmp *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName = NULL;

/* SteamAPI_ISteamRemotePlay_GetSessionCount */
__asm__(".globl SteamAPI_ISteamRemotePlay_GetSessionCount");
__asm__("SteamAPI_ISteamRemotePlay_GetSessionCount: jmp *_fwd_SteamAPI_ISteamRemotePlay_GetSessionCount(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionCount = NULL;

/* SteamAPI_ISteamRemotePlay_GetSessionID */
__asm__(".globl SteamAPI_ISteamRemotePlay_GetSessionID");
__asm__("SteamAPI_ISteamRemotePlay_GetSessionID: jmp *_fwd_SteamAPI_ISteamRemotePlay_GetSessionID(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionID = NULL;

/* SteamAPI_ISteamRemotePlay_GetSessionSteamID */
__asm__(".globl SteamAPI_ISteamRemotePlay_GetSessionSteamID");
__asm__("SteamAPI_ISteamRemotePlay_GetSessionSteamID: jmp *_fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID(%rip)");
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID = NULL;

/* SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch */
__asm__(".globl SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch");
__asm__("SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch: jmp *_fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch = NULL;

/* SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate */
__asm__(".globl SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate");
__asm__("SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate: jmp *_fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate = NULL;

/* SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest */
__asm__(".globl SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest");
__asm__("SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest: jmp *_fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest = NULL;

/* SteamAPI_ISteamRemoteStorage_DeletePublishedFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_DeletePublishedFile");
__asm__("SteamAPI_ISteamRemoteStorage_DeletePublishedFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile = NULL;

/* SteamAPI_ISteamRemoteStorage_EndFileWriteBatch */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EndFileWriteBatch");
__asm__("SteamAPI_ISteamRemoteStorage_EndFileWriteBatch: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch = NULL;

/* SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction");
__asm__("SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction = NULL;

/* SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles");
__asm__("SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles = NULL;

/* SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles");
__asm__("SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles = NULL;

/* SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles");
__asm__("SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles = NULL;

/* SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles */
__asm__(".globl SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles");
__asm__("SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles: jmp *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles = NULL;

/* SteamAPI_ISteamRemoteStorage_FileDelete */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileDelete");
__asm__("SteamAPI_ISteamRemoteStorage_FileDelete: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileDelete(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileDelete = NULL;

/* SteamAPI_ISteamRemoteStorage_FileExists */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileExists");
__asm__("SteamAPI_ISteamRemoteStorage_FileExists: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileExists(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileExists = NULL;

/* SteamAPI_ISteamRemoteStorage_FileForget */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileForget");
__asm__("SteamAPI_ISteamRemoteStorage_FileForget: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileForget(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileForget = NULL;

/* SteamAPI_ISteamRemoteStorage_FilePersisted */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FilePersisted");
__asm__("SteamAPI_ISteamRemoteStorage_FilePersisted: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FilePersisted(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FilePersisted = NULL;

/* SteamAPI_ISteamRemoteStorage_FileRead */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileRead");
__asm__("SteamAPI_ISteamRemoteStorage_FileRead: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileRead(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileRead = NULL;

/* SteamAPI_ISteamRemoteStorage_FileReadAsync */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileReadAsync");
__asm__("SteamAPI_ISteamRemoteStorage_FileReadAsync: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync = NULL;

/* SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete");
__asm__("SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete = NULL;

/* SteamAPI_ISteamRemoteStorage_FileShare */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileShare");
__asm__("SteamAPI_ISteamRemoteStorage_FileShare: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileShare(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileShare = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWrite */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWrite");
__asm__("SteamAPI_ISteamRemoteStorage_FileWrite: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWrite(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWrite = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWriteAsync */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWriteAsync");
__asm__("SteamAPI_ISteamRemoteStorage_FileWriteAsync: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel");
__asm__("SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamClose */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWriteStreamClose");
__asm__("SteamAPI_ISteamRemoteStorage_FileWriteStreamClose: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen");
__asm__("SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen = NULL;

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk */
__asm__(".globl SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk");
__asm__("SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk: jmp *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk = NULL;

/* SteamAPI_ISteamRemoteStorage_GetCachedUGCCount */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetCachedUGCCount");
__asm__("SteamAPI_ISteamRemoteStorage_GetCachedUGCCount: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount = NULL;

/* SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle");
__asm__("SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle = NULL;

/* SteamAPI_ISteamRemoteStorage_GetFileCount */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetFileCount");
__asm__("SteamAPI_ISteamRemoteStorage_GetFileCount: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetFileCount(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileCount = NULL;

/* SteamAPI_ISteamRemoteStorage_GetFileNameAndSize */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
__asm__("SteamAPI_ISteamRemoteStorage_GetFileNameAndSize: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize = NULL;

/* SteamAPI_ISteamRemoteStorage_GetFileSize */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetFileSize");
__asm__("SteamAPI_ISteamRemoteStorage_GetFileSize: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetFileSize(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileSize = NULL;

/* SteamAPI_ISteamRemoteStorage_GetFileTimestamp */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetFileTimestamp");
__asm__("SteamAPI_ISteamRemoteStorage_GetFileTimestamp: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp = NULL;

/* SteamAPI_ISteamRemoteStorage_GetLocalFileChange */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetLocalFileChange");
__asm__("SteamAPI_ISteamRemoteStorage_GetLocalFileChange: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange = NULL;

/* SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount");
__asm__("SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount = NULL;

/* SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails");
__asm__("SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails = NULL;

/* SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails");
__asm__("SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails = NULL;

/* SteamAPI_ISteamRemoteStorage_GetQuota */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetQuota");
__asm__("SteamAPI_ISteamRemoteStorage_GetQuota: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetQuota(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetQuota = NULL;

/* SteamAPI_ISteamRemoteStorage_GetSyncPlatforms */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetSyncPlatforms");
__asm__("SteamAPI_ISteamRemoteStorage_GetSyncPlatforms: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms = NULL;

/* SteamAPI_ISteamRemoteStorage_GetUGCDetails */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetUGCDetails");
__asm__("SteamAPI_ISteamRemoteStorage_GetUGCDetails: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails = NULL;

/* SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress");
__asm__("SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress = NULL;

/* SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails */
__asm__(".globl SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails");
__asm__("SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails: jmp *_fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails = NULL;

/* SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount */
__asm__(".globl SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
__asm__("SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount: jmp *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount = NULL;

/* SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp */
__asm__(".globl SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
__asm__("SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp: jmp *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp = NULL;

/* SteamAPI_ISteamRemoteStorage_PublishVideo */
__asm__(".globl SteamAPI_ISteamRemoteStorage_PublishVideo");
__asm__("SteamAPI_ISteamRemoteStorage_PublishVideo: jmp *_fwd_SteamAPI_ISteamRemoteStorage_PublishVideo(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_PublishVideo = NULL;

/* SteamAPI_ISteamRemoteStorage_PublishWorkshopFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_PublishWorkshopFile");
__asm__("SteamAPI_ISteamRemoteStorage_PublishWorkshopFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile = NULL;

/* SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp */
__asm__(".globl SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp");
__asm__("SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp: jmp *_fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp = NULL;

/* SteamAPI_ISteamRemoteStorage_SetSyncPlatforms */
__asm__(".globl SteamAPI_ISteamRemoteStorage_SetSyncPlatforms");
__asm__("SteamAPI_ISteamRemoteStorage_SetSyncPlatforms: jmp *_fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms = NULL;

/* SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction */
__asm__(".globl SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction");
__asm__("SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction: jmp *_fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction = NULL;

/* SteamAPI_ISteamRemoteStorage_SubscribePublishedFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_SubscribePublishedFile");
__asm__("SteamAPI_ISteamRemoteStorage_SubscribePublishedFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile = NULL;

/* SteamAPI_ISteamRemoteStorage_UGCDownload */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UGCDownload");
__asm__("SteamAPI_ISteamRemoteStorage_UGCDownload: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownload(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownload = NULL;

/* SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation");
__asm__("SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation = NULL;

/* SteamAPI_ISteamRemoteStorage_UGCRead */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UGCRead");
__asm__("SteamAPI_ISteamRemoteStorage_UGCRead: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UGCRead(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCRead = NULL;

/* SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile");
__asm__("SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility");
__asm__("SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility = NULL;

/* SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote */
__asm__(".globl SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote");
__asm__("SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote: jmp *_fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote(%rip)");
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote = NULL;

/* SteamAPI_ISteamScreenshots_AddScreenshotToLibrary */
__asm__(".globl SteamAPI_ISteamScreenshots_AddScreenshotToLibrary");
__asm__("SteamAPI_ISteamScreenshots_AddScreenshotToLibrary: jmp *_fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary = NULL;

/* SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary */
__asm__(".globl SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary");
__asm__("SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary: jmp *_fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary = NULL;

/* SteamAPI_ISteamScreenshots_HookScreenshots */
__asm__(".globl SteamAPI_ISteamScreenshots_HookScreenshots");
__asm__("SteamAPI_ISteamScreenshots_HookScreenshots: jmp *_fwd_SteamAPI_ISteamScreenshots_HookScreenshots(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_HookScreenshots = NULL;

/* SteamAPI_ISteamScreenshots_IsScreenshotsHooked */
__asm__(".globl SteamAPI_ISteamScreenshots_IsScreenshotsHooked");
__asm__("SteamAPI_ISteamScreenshots_IsScreenshotsHooked: jmp *_fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked = NULL;

/* SteamAPI_ISteamScreenshots_SetLocation */
__asm__(".globl SteamAPI_ISteamScreenshots_SetLocation");
__asm__("SteamAPI_ISteamScreenshots_SetLocation: jmp *_fwd_SteamAPI_ISteamScreenshots_SetLocation(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_SetLocation = NULL;

/* SteamAPI_ISteamScreenshots_TagPublishedFile */
__asm__(".globl SteamAPI_ISteamScreenshots_TagPublishedFile");
__asm__("SteamAPI_ISteamScreenshots_TagPublishedFile: jmp *_fwd_SteamAPI_ISteamScreenshots_TagPublishedFile(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_TagPublishedFile = NULL;

/* SteamAPI_ISteamScreenshots_TagUser */
__asm__(".globl SteamAPI_ISteamScreenshots_TagUser");
__asm__("SteamAPI_ISteamScreenshots_TagUser: jmp *_fwd_SteamAPI_ISteamScreenshots_TagUser(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_TagUser = NULL;

/* SteamAPI_ISteamScreenshots_TriggerScreenshot */
__asm__(".globl SteamAPI_ISteamScreenshots_TriggerScreenshot");
__asm__("SteamAPI_ISteamScreenshots_TriggerScreenshot: jmp *_fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot = NULL;

/* SteamAPI_ISteamScreenshots_WriteScreenshot */
__asm__(".globl SteamAPI_ISteamScreenshots_WriteScreenshot");
__asm__("SteamAPI_ISteamScreenshots_WriteScreenshot: jmp *_fwd_SteamAPI_ISteamScreenshots_WriteScreenshot(%rip)");
static void *_fwd_SteamAPI_ISteamScreenshots_WriteScreenshot = NULL;

/* SteamAPI_ISteamTimeline_AddGamePhaseTag */
__asm__(".globl SteamAPI_ISteamTimeline_AddGamePhaseTag");
__asm__("SteamAPI_ISteamTimeline_AddGamePhaseTag: jmp *_fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag = NULL;

/* SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_AddRangeTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_AddRangeTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_AddRangeTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_ClearTimelineTooltip */
__asm__(".globl SteamAPI_ISteamTimeline_ClearTimelineTooltip");
__asm__("SteamAPI_ISteamTimeline_ClearTimelineTooltip: jmp *_fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip = NULL;

/* SteamAPI_ISteamTimeline_DoesEventRecordingExist */
__asm__(".globl SteamAPI_ISteamTimeline_DoesEventRecordingExist");
__asm__("SteamAPI_ISteamTimeline_DoesEventRecordingExist: jmp *_fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist = NULL;

/* SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist */
__asm__(".globl SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist");
__asm__("SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist: jmp *_fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist = NULL;

/* SteamAPI_ISteamTimeline_EndGamePhase */
__asm__(".globl SteamAPI_ISteamTimeline_EndGamePhase");
__asm__("SteamAPI_ISteamTimeline_EndGamePhase: jmp *_fwd_SteamAPI_ISteamTimeline_EndGamePhase(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_EndGamePhase = NULL;

/* SteamAPI_ISteamTimeline_EndRangeTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_EndRangeTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_EndRangeTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_OpenOverlayToGamePhase */
__asm__(".globl SteamAPI_ISteamTimeline_OpenOverlayToGamePhase");
__asm__("SteamAPI_ISteamTimeline_OpenOverlayToGamePhase: jmp *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase = NULL;

/* SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_RemoveTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_RemoveTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_RemoveTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_SetGamePhaseAttribute */
__asm__(".globl SteamAPI_ISteamTimeline_SetGamePhaseAttribute");
__asm__("SteamAPI_ISteamTimeline_SetGamePhaseAttribute: jmp *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute = NULL;

/* SteamAPI_ISteamTimeline_SetGamePhaseID */
__asm__(".globl SteamAPI_ISteamTimeline_SetGamePhaseID");
__asm__("SteamAPI_ISteamTimeline_SetGamePhaseID: jmp *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseID(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseID = NULL;

/* SteamAPI_ISteamTimeline_SetTimelineGameMode */
__asm__(".globl SteamAPI_ISteamTimeline_SetTimelineGameMode");
__asm__("SteamAPI_ISteamTimeline_SetTimelineGameMode: jmp *_fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode = NULL;

/* SteamAPI_ISteamTimeline_SetTimelineTooltip */
__asm__(".globl SteamAPI_ISteamTimeline_SetTimelineTooltip");
__asm__("SteamAPI_ISteamTimeline_SetTimelineTooltip: jmp *_fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip = NULL;

/* SteamAPI_ISteamTimeline_StartGamePhase */
__asm__(".globl SteamAPI_ISteamTimeline_StartGamePhase");
__asm__("SteamAPI_ISteamTimeline_StartGamePhase: jmp *_fwd_SteamAPI_ISteamTimeline_StartGamePhase(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_StartGamePhase = NULL;

/* SteamAPI_ISteamTimeline_StartRangeTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_StartRangeTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_StartRangeTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent = NULL;

/* SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent */
__asm__(".globl SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent");
__asm__("SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent: jmp *_fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent(%rip)");
static void *_fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent = NULL;

/* SteamAPI_ISteamUGC_AddAppDependency */
__asm__(".globl SteamAPI_ISteamUGC_AddAppDependency");
__asm__("SteamAPI_ISteamUGC_AddAppDependency: jmp *_fwd_SteamAPI_ISteamUGC_AddAppDependency(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddAppDependency = NULL;

/* SteamAPI_ISteamUGC_AddContentDescriptor */
__asm__(".globl SteamAPI_ISteamUGC_AddContentDescriptor");
__asm__("SteamAPI_ISteamUGC_AddContentDescriptor: jmp *_fwd_SteamAPI_ISteamUGC_AddContentDescriptor(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddContentDescriptor = NULL;

/* SteamAPI_ISteamUGC_AddDependency */
__asm__(".globl SteamAPI_ISteamUGC_AddDependency");
__asm__("SteamAPI_ISteamUGC_AddDependency: jmp *_fwd_SteamAPI_ISteamUGC_AddDependency(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddDependency = NULL;

/* SteamAPI_ISteamUGC_AddExcludedTag */
__asm__(".globl SteamAPI_ISteamUGC_AddExcludedTag");
__asm__("SteamAPI_ISteamUGC_AddExcludedTag: jmp *_fwd_SteamAPI_ISteamUGC_AddExcludedTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddExcludedTag = NULL;

/* SteamAPI_ISteamUGC_AddItemKeyValueTag */
__asm__(".globl SteamAPI_ISteamUGC_AddItemKeyValueTag");
__asm__("SteamAPI_ISteamUGC_AddItemKeyValueTag: jmp *_fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag = NULL;

/* SteamAPI_ISteamUGC_AddItemPreviewFile */
__asm__(".globl SteamAPI_ISteamUGC_AddItemPreviewFile");
__asm__("SteamAPI_ISteamUGC_AddItemPreviewFile: jmp *_fwd_SteamAPI_ISteamUGC_AddItemPreviewFile(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddItemPreviewFile = NULL;

/* SteamAPI_ISteamUGC_AddItemPreviewVideo */
__asm__(".globl SteamAPI_ISteamUGC_AddItemPreviewVideo");
__asm__("SteamAPI_ISteamUGC_AddItemPreviewVideo: jmp *_fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo = NULL;

/* SteamAPI_ISteamUGC_AddItemToFavorites */
__asm__(".globl SteamAPI_ISteamUGC_AddItemToFavorites");
__asm__("SteamAPI_ISteamUGC_AddItemToFavorites: jmp *_fwd_SteamAPI_ISteamUGC_AddItemToFavorites(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddItemToFavorites = NULL;

/* SteamAPI_ISteamUGC_AddRequiredKeyValueTag */
__asm__(".globl SteamAPI_ISteamUGC_AddRequiredKeyValueTag");
__asm__("SteamAPI_ISteamUGC_AddRequiredKeyValueTag: jmp *_fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag = NULL;

/* SteamAPI_ISteamUGC_AddRequiredTag */
__asm__(".globl SteamAPI_ISteamUGC_AddRequiredTag");
__asm__("SteamAPI_ISteamUGC_AddRequiredTag: jmp *_fwd_SteamAPI_ISteamUGC_AddRequiredTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredTag = NULL;

/* SteamAPI_ISteamUGC_AddRequiredTagGroup */
__asm__(".globl SteamAPI_ISteamUGC_AddRequiredTagGroup");
__asm__("SteamAPI_ISteamUGC_AddRequiredTagGroup: jmp *_fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup = NULL;

/* SteamAPI_ISteamUGC_BInitWorkshopForGameServer */
__asm__(".globl SteamAPI_ISteamUGC_BInitWorkshopForGameServer");
__asm__("SteamAPI_ISteamUGC_BInitWorkshopForGameServer: jmp *_fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer = NULL;

/* SteamAPI_ISteamUGC_CreateItem */
__asm__(".globl SteamAPI_ISteamUGC_CreateItem");
__asm__("SteamAPI_ISteamUGC_CreateItem: jmp *_fwd_SteamAPI_ISteamUGC_CreateItem(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_CreateItem = NULL;

/* SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor */
__asm__(".globl SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor");
__asm__("SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor: jmp *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor = NULL;

/* SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage */
__asm__(".globl SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage");
__asm__("SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage: jmp *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage = NULL;

/* SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest */
__asm__(".globl SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest");
__asm__("SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest: jmp *_fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest = NULL;

/* SteamAPI_ISteamUGC_CreateQueryUserUGCRequest */
__asm__(".globl SteamAPI_ISteamUGC_CreateQueryUserUGCRequest");
__asm__("SteamAPI_ISteamUGC_CreateQueryUserUGCRequest: jmp *_fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest = NULL;

/* SteamAPI_ISteamUGC_DeleteItem */
__asm__(".globl SteamAPI_ISteamUGC_DeleteItem");
__asm__("SteamAPI_ISteamUGC_DeleteItem: jmp *_fwd_SteamAPI_ISteamUGC_DeleteItem(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_DeleteItem = NULL;

/* SteamAPI_ISteamUGC_DownloadItem */
__asm__(".globl SteamAPI_ISteamUGC_DownloadItem");
__asm__("SteamAPI_ISteamUGC_DownloadItem: jmp *_fwd_SteamAPI_ISteamUGC_DownloadItem(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_DownloadItem = NULL;

/* SteamAPI_ISteamUGC_GetAppDependencies */
__asm__(".globl SteamAPI_ISteamUGC_GetAppDependencies");
__asm__("SteamAPI_ISteamUGC_GetAppDependencies: jmp *_fwd_SteamAPI_ISteamUGC_GetAppDependencies(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetAppDependencies = NULL;

/* SteamAPI_ISteamUGC_GetItemDownloadInfo */
__asm__(".globl SteamAPI_ISteamUGC_GetItemDownloadInfo");
__asm__("SteamAPI_ISteamUGC_GetItemDownloadInfo: jmp *_fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo = NULL;

/* SteamAPI_ISteamUGC_GetItemInstallInfo */
__asm__(".globl SteamAPI_ISteamUGC_GetItemInstallInfo");
__asm__("SteamAPI_ISteamUGC_GetItemInstallInfo: jmp *_fwd_SteamAPI_ISteamUGC_GetItemInstallInfo(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetItemInstallInfo = NULL;

/* SteamAPI_ISteamUGC_GetItemState */
__asm__(".globl SteamAPI_ISteamUGC_GetItemState");
__asm__("SteamAPI_ISteamUGC_GetItemState: jmp *_fwd_SteamAPI_ISteamUGC_GetItemState(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetItemState = NULL;

/* SteamAPI_ISteamUGC_GetItemUpdateProgress */
__asm__(".globl SteamAPI_ISteamUGC_GetItemUpdateProgress");
__asm__("SteamAPI_ISteamUGC_GetItemUpdateProgress: jmp *_fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress = NULL;

/* SteamAPI_ISteamUGC_GetNumSubscribedItems */
__asm__(".globl SteamAPI_ISteamUGC_GetNumSubscribedItems");
__asm__("SteamAPI_ISteamUGC_GetNumSubscribedItems: jmp *_fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems = NULL;

/* SteamAPI_ISteamUGC_GetNumSupportedGameVersions */
__asm__(".globl SteamAPI_ISteamUGC_GetNumSupportedGameVersions");
__asm__("SteamAPI_ISteamUGC_GetNumSupportedGameVersions: jmp *_fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions = NULL;

/* SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag");
__asm__("SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCChildren */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCChildren");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCChildren: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCMetadata */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCMetadata");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCMetadata: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCNumTags */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCNumTags");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCNumTags: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCPreviewURL */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCPreviewURL");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCPreviewURL: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCResult */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCResult");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCResult: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCResult(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCResult = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCStatistic */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCStatistic");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCStatistic: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCTag */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCTag");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCTag: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTag = NULL;

/* SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName */
__asm__(".globl SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName");
__asm__("SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName: jmp *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName = NULL;

/* SteamAPI_ISteamUGC_GetSubscribedItems */
__asm__(".globl SteamAPI_ISteamUGC_GetSubscribedItems");
__asm__("SteamAPI_ISteamUGC_GetSubscribedItems: jmp *_fwd_SteamAPI_ISteamUGC_GetSubscribedItems(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetSubscribedItems = NULL;

/* SteamAPI_ISteamUGC_GetSupportedGameVersionData */
__asm__(".globl SteamAPI_ISteamUGC_GetSupportedGameVersionData");
__asm__("SteamAPI_ISteamUGC_GetSupportedGameVersionData: jmp *_fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData = NULL;

/* SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences */
__asm__(".globl SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences");
__asm__("SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences: jmp *_fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences = NULL;

/* SteamAPI_ISteamUGC_GetUserItemVote */
__asm__(".globl SteamAPI_ISteamUGC_GetUserItemVote");
__asm__("SteamAPI_ISteamUGC_GetUserItemVote: jmp *_fwd_SteamAPI_ISteamUGC_GetUserItemVote(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetUserItemVote = NULL;

/* SteamAPI_ISteamUGC_GetWorkshopEULAStatus */
__asm__(".globl SteamAPI_ISteamUGC_GetWorkshopEULAStatus");
__asm__("SteamAPI_ISteamUGC_GetWorkshopEULAStatus: jmp *_fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus = NULL;

/* SteamAPI_ISteamUGC_ReleaseQueryUGCRequest */
__asm__(".globl SteamAPI_ISteamUGC_ReleaseQueryUGCRequest");
__asm__("SteamAPI_ISteamUGC_ReleaseQueryUGCRequest: jmp *_fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest = NULL;

/* SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags */
__asm__(".globl SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags");
__asm__("SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags: jmp *_fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags = NULL;

/* SteamAPI_ISteamUGC_RemoveAppDependency */
__asm__(".globl SteamAPI_ISteamUGC_RemoveAppDependency");
__asm__("SteamAPI_ISteamUGC_RemoveAppDependency: jmp *_fwd_SteamAPI_ISteamUGC_RemoveAppDependency(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveAppDependency = NULL;

/* SteamAPI_ISteamUGC_RemoveContentDescriptor */
__asm__(".globl SteamAPI_ISteamUGC_RemoveContentDescriptor");
__asm__("SteamAPI_ISteamUGC_RemoveContentDescriptor: jmp *_fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor = NULL;

/* SteamAPI_ISteamUGC_RemoveDependency */
__asm__(".globl SteamAPI_ISteamUGC_RemoveDependency");
__asm__("SteamAPI_ISteamUGC_RemoveDependency: jmp *_fwd_SteamAPI_ISteamUGC_RemoveDependency(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveDependency = NULL;

/* SteamAPI_ISteamUGC_RemoveItemFromFavorites */
__asm__(".globl SteamAPI_ISteamUGC_RemoveItemFromFavorites");
__asm__("SteamAPI_ISteamUGC_RemoveItemFromFavorites: jmp *_fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites = NULL;

/* SteamAPI_ISteamUGC_RemoveItemKeyValueTags */
__asm__(".globl SteamAPI_ISteamUGC_RemoveItemKeyValueTags");
__asm__("SteamAPI_ISteamUGC_RemoveItemKeyValueTags: jmp *_fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags = NULL;

/* SteamAPI_ISteamUGC_RemoveItemPreview */
__asm__(".globl SteamAPI_ISteamUGC_RemoveItemPreview");
__asm__("SteamAPI_ISteamUGC_RemoveItemPreview: jmp *_fwd_SteamAPI_ISteamUGC_RemoveItemPreview(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemPreview = NULL;

/* SteamAPI_ISteamUGC_RequestUGCDetails */
__asm__(".globl SteamAPI_ISteamUGC_RequestUGCDetails");
__asm__("SteamAPI_ISteamUGC_RequestUGCDetails: jmp *_fwd_SteamAPI_ISteamUGC_RequestUGCDetails(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_RequestUGCDetails = NULL;

/* SteamAPI_ISteamUGC_SendQueryUGCRequest */
__asm__(".globl SteamAPI_ISteamUGC_SendQueryUGCRequest");
__asm__("SteamAPI_ISteamUGC_SendQueryUGCRequest: jmp *_fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest = NULL;

/* SteamAPI_ISteamUGC_SetAdminQuery */
__asm__(".globl SteamAPI_ISteamUGC_SetAdminQuery");
__asm__("SteamAPI_ISteamUGC_SetAdminQuery: jmp *_fwd_SteamAPI_ISteamUGC_SetAdminQuery(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetAdminQuery = NULL;

/* SteamAPI_ISteamUGC_SetAllowCachedResponse */
__asm__(".globl SteamAPI_ISteamUGC_SetAllowCachedResponse");
__asm__("SteamAPI_ISteamUGC_SetAllowCachedResponse: jmp *_fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse = NULL;

/* SteamAPI_ISteamUGC_SetAllowLegacyUpload */
__asm__(".globl SteamAPI_ISteamUGC_SetAllowLegacyUpload");
__asm__("SteamAPI_ISteamUGC_SetAllowLegacyUpload: jmp *_fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload = NULL;

/* SteamAPI_ISteamUGC_SetCloudFileNameFilter */
__asm__(".globl SteamAPI_ISteamUGC_SetCloudFileNameFilter");
__asm__("SteamAPI_ISteamUGC_SetCloudFileNameFilter: jmp *_fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter = NULL;

/* SteamAPI_ISteamUGC_SetItemContent */
__asm__(".globl SteamAPI_ISteamUGC_SetItemContent");
__asm__("SteamAPI_ISteamUGC_SetItemContent: jmp *_fwd_SteamAPI_ISteamUGC_SetItemContent(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemContent = NULL;

/* SteamAPI_ISteamUGC_SetItemDescription */
__asm__(".globl SteamAPI_ISteamUGC_SetItemDescription");
__asm__("SteamAPI_ISteamUGC_SetItemDescription: jmp *_fwd_SteamAPI_ISteamUGC_SetItemDescription(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemDescription = NULL;

/* SteamAPI_ISteamUGC_SetItemMetadata */
__asm__(".globl SteamAPI_ISteamUGC_SetItemMetadata");
__asm__("SteamAPI_ISteamUGC_SetItemMetadata: jmp *_fwd_SteamAPI_ISteamUGC_SetItemMetadata(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemMetadata = NULL;

/* SteamAPI_ISteamUGC_SetItemPreview */
__asm__(".globl SteamAPI_ISteamUGC_SetItemPreview");
__asm__("SteamAPI_ISteamUGC_SetItemPreview: jmp *_fwd_SteamAPI_ISteamUGC_SetItemPreview(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemPreview = NULL;

/* SteamAPI_ISteamUGC_SetItemTags */
__asm__(".globl SteamAPI_ISteamUGC_SetItemTags");
__asm__("SteamAPI_ISteamUGC_SetItemTags: jmp *_fwd_SteamAPI_ISteamUGC_SetItemTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemTags = NULL;

/* SteamAPI_ISteamUGC_SetItemTitle */
__asm__(".globl SteamAPI_ISteamUGC_SetItemTitle");
__asm__("SteamAPI_ISteamUGC_SetItemTitle: jmp *_fwd_SteamAPI_ISteamUGC_SetItemTitle(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemTitle = NULL;

/* SteamAPI_ISteamUGC_SetItemUpdateLanguage */
__asm__(".globl SteamAPI_ISteamUGC_SetItemUpdateLanguage");
__asm__("SteamAPI_ISteamUGC_SetItemUpdateLanguage: jmp *_fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage = NULL;

/* SteamAPI_ISteamUGC_SetItemVisibility */
__asm__(".globl SteamAPI_ISteamUGC_SetItemVisibility");
__asm__("SteamAPI_ISteamUGC_SetItemVisibility: jmp *_fwd_SteamAPI_ISteamUGC_SetItemVisibility(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetItemVisibility = NULL;

/* SteamAPI_ISteamUGC_SetLanguage */
__asm__(".globl SteamAPI_ISteamUGC_SetLanguage");
__asm__("SteamAPI_ISteamUGC_SetLanguage: jmp *_fwd_SteamAPI_ISteamUGC_SetLanguage(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetLanguage = NULL;

/* SteamAPI_ISteamUGC_SetMatchAnyTag */
__asm__(".globl SteamAPI_ISteamUGC_SetMatchAnyTag");
__asm__("SteamAPI_ISteamUGC_SetMatchAnyTag: jmp *_fwd_SteamAPI_ISteamUGC_SetMatchAnyTag(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetMatchAnyTag = NULL;

/* SteamAPI_ISteamUGC_SetRankedByTrendDays */
__asm__(".globl SteamAPI_ISteamUGC_SetRankedByTrendDays");
__asm__("SteamAPI_ISteamUGC_SetRankedByTrendDays: jmp *_fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays = NULL;

/* SteamAPI_ISteamUGC_SetRequiredGameVersions */
__asm__(".globl SteamAPI_ISteamUGC_SetRequiredGameVersions");
__asm__("SteamAPI_ISteamUGC_SetRequiredGameVersions: jmp *_fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions = NULL;

/* SteamAPI_ISteamUGC_SetReturnAdditionalPreviews */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnAdditionalPreviews");
__asm__("SteamAPI_ISteamUGC_SetReturnAdditionalPreviews: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews = NULL;

/* SteamAPI_ISteamUGC_SetReturnChildren */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnChildren");
__asm__("SteamAPI_ISteamUGC_SetReturnChildren: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnChildren(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnChildren = NULL;

/* SteamAPI_ISteamUGC_SetReturnKeyValueTags */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnKeyValueTags");
__asm__("SteamAPI_ISteamUGC_SetReturnKeyValueTags: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags = NULL;

/* SteamAPI_ISteamUGC_SetReturnLongDescription */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnLongDescription");
__asm__("SteamAPI_ISteamUGC_SetReturnLongDescription: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnLongDescription(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnLongDescription = NULL;

/* SteamAPI_ISteamUGC_SetReturnMetadata */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnMetadata");
__asm__("SteamAPI_ISteamUGC_SetReturnMetadata: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnMetadata(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnMetadata = NULL;

/* SteamAPI_ISteamUGC_SetReturnOnlyIDs */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnOnlyIDs");
__asm__("SteamAPI_ISteamUGC_SetReturnOnlyIDs: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs = NULL;

/* SteamAPI_ISteamUGC_SetReturnPlaytimeStats */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnPlaytimeStats");
__asm__("SteamAPI_ISteamUGC_SetReturnPlaytimeStats: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats = NULL;

/* SteamAPI_ISteamUGC_SetReturnTotalOnly */
__asm__(".globl SteamAPI_ISteamUGC_SetReturnTotalOnly");
__asm__("SteamAPI_ISteamUGC_SetReturnTotalOnly: jmp *_fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly = NULL;

/* SteamAPI_ISteamUGC_SetSearchText */
__asm__(".globl SteamAPI_ISteamUGC_SetSearchText");
__asm__("SteamAPI_ISteamUGC_SetSearchText: jmp *_fwd_SteamAPI_ISteamUGC_SetSearchText(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetSearchText = NULL;

/* SteamAPI_ISteamUGC_SetTimeCreatedDateRange */
__asm__(".globl SteamAPI_ISteamUGC_SetTimeCreatedDateRange");
__asm__("SteamAPI_ISteamUGC_SetTimeCreatedDateRange: jmp *_fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange = NULL;

/* SteamAPI_ISteamUGC_SetTimeUpdatedDateRange */
__asm__(".globl SteamAPI_ISteamUGC_SetTimeUpdatedDateRange");
__asm__("SteamAPI_ISteamUGC_SetTimeUpdatedDateRange: jmp *_fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange = NULL;

/* SteamAPI_ISteamUGC_SetUserItemVote */
__asm__(".globl SteamAPI_ISteamUGC_SetUserItemVote");
__asm__("SteamAPI_ISteamUGC_SetUserItemVote: jmp *_fwd_SteamAPI_ISteamUGC_SetUserItemVote(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SetUserItemVote = NULL;

/* SteamAPI_ISteamUGC_ShowWorkshopEULA */
__asm__(".globl SteamAPI_ISteamUGC_ShowWorkshopEULA");
__asm__("SteamAPI_ISteamUGC_ShowWorkshopEULA: jmp *_fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA = NULL;

/* SteamAPI_ISteamUGC_StartItemUpdate */
__asm__(".globl SteamAPI_ISteamUGC_StartItemUpdate");
__asm__("SteamAPI_ISteamUGC_StartItemUpdate: jmp *_fwd_SteamAPI_ISteamUGC_StartItemUpdate(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_StartItemUpdate = NULL;

/* SteamAPI_ISteamUGC_StartPlaytimeTracking */
__asm__(".globl SteamAPI_ISteamUGC_StartPlaytimeTracking");
__asm__("SteamAPI_ISteamUGC_StartPlaytimeTracking: jmp *_fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking = NULL;

/* SteamAPI_ISteamUGC_StopPlaytimeTracking */
__asm__(".globl SteamAPI_ISteamUGC_StopPlaytimeTracking");
__asm__("SteamAPI_ISteamUGC_StopPlaytimeTracking: jmp *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking = NULL;

/* SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems */
__asm__(".globl SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems");
__asm__("SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems: jmp *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems = NULL;

/* SteamAPI_ISteamUGC_SubmitItemUpdate */
__asm__(".globl SteamAPI_ISteamUGC_SubmitItemUpdate");
__asm__("SteamAPI_ISteamUGC_SubmitItemUpdate: jmp *_fwd_SteamAPI_ISteamUGC_SubmitItemUpdate(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SubmitItemUpdate = NULL;

/* SteamAPI_ISteamUGC_SubscribeItem */
__asm__(".globl SteamAPI_ISteamUGC_SubscribeItem");
__asm__("SteamAPI_ISteamUGC_SubscribeItem: jmp *_fwd_SteamAPI_ISteamUGC_SubscribeItem(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SubscribeItem = NULL;

/* SteamAPI_ISteamUGC_SuspendDownloads */
__asm__(".globl SteamAPI_ISteamUGC_SuspendDownloads");
__asm__("SteamAPI_ISteamUGC_SuspendDownloads: jmp *_fwd_SteamAPI_ISteamUGC_SuspendDownloads(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_SuspendDownloads = NULL;

/* SteamAPI_ISteamUGC_UnsubscribeItem */
__asm__(".globl SteamAPI_ISteamUGC_UnsubscribeItem");
__asm__("SteamAPI_ISteamUGC_UnsubscribeItem: jmp *_fwd_SteamAPI_ISteamUGC_UnsubscribeItem(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_UnsubscribeItem = NULL;

/* SteamAPI_ISteamUGC_UpdateItemPreviewFile */
__asm__(".globl SteamAPI_ISteamUGC_UpdateItemPreviewFile");
__asm__("SteamAPI_ISteamUGC_UpdateItemPreviewFile: jmp *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile = NULL;

/* SteamAPI_ISteamUGC_UpdateItemPreviewVideo */
__asm__(".globl SteamAPI_ISteamUGC_UpdateItemPreviewVideo");
__asm__("SteamAPI_ISteamUGC_UpdateItemPreviewVideo: jmp *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo(%rip)");
static void *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo = NULL;

/* SteamAPI_ISteamUser_AdvertiseGame */
__asm__(".globl SteamAPI_ISteamUser_AdvertiseGame");
__asm__("SteamAPI_ISteamUser_AdvertiseGame: jmp *_fwd_SteamAPI_ISteamUser_AdvertiseGame(%rip)");
static void *_fwd_SteamAPI_ISteamUser_AdvertiseGame = NULL;

/* SteamAPI_ISteamUser_BeginAuthSession */
__asm__(".globl SteamAPI_ISteamUser_BeginAuthSession");
__asm__("SteamAPI_ISteamUser_BeginAuthSession: jmp *_fwd_SteamAPI_ISteamUser_BeginAuthSession(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BeginAuthSession = NULL;

/* SteamAPI_ISteamUser_BIsBehindNAT */
__asm__(".globl SteamAPI_ISteamUser_BIsBehindNAT");
__asm__("SteamAPI_ISteamUser_BIsBehindNAT: jmp *_fwd_SteamAPI_ISteamUser_BIsBehindNAT(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BIsBehindNAT = NULL;

/* SteamAPI_ISteamUser_BIsPhoneIdentifying */
__asm__(".globl SteamAPI_ISteamUser_BIsPhoneIdentifying");
__asm__("SteamAPI_ISteamUser_BIsPhoneIdentifying: jmp *_fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying = NULL;

/* SteamAPI_ISteamUser_BIsPhoneRequiringVerification */
__asm__(".globl SteamAPI_ISteamUser_BIsPhoneRequiringVerification");
__asm__("SteamAPI_ISteamUser_BIsPhoneRequiringVerification: jmp *_fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification = NULL;

/* SteamAPI_ISteamUser_BIsPhoneVerified */
__asm__(".globl SteamAPI_ISteamUser_BIsPhoneVerified");
__asm__("SteamAPI_ISteamUser_BIsPhoneVerified: jmp *_fwd_SteamAPI_ISteamUser_BIsPhoneVerified(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneVerified = NULL;

/* SteamAPI_ISteamUser_BIsTwoFactorEnabled */
__asm__(".globl SteamAPI_ISteamUser_BIsTwoFactorEnabled");
__asm__("SteamAPI_ISteamUser_BIsTwoFactorEnabled: jmp *_fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled = NULL;

/* SteamAPI_ISteamUser_BLoggedOn */
__asm__(".globl SteamAPI_ISteamUser_BLoggedOn");
__asm__("SteamAPI_ISteamUser_BLoggedOn: jmp *_fwd_SteamAPI_ISteamUser_BLoggedOn(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BLoggedOn = NULL;

/* SteamAPI_ISteamUser_BSetDurationControlOnlineState */
__asm__(".globl SteamAPI_ISteamUser_BSetDurationControlOnlineState");
__asm__("SteamAPI_ISteamUser_BSetDurationControlOnlineState: jmp *_fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState(%rip)");
static void *_fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState = NULL;

/* SteamAPI_ISteamUser_CancelAuthTicket */
__asm__(".globl SteamAPI_ISteamUser_CancelAuthTicket");
__asm__("SteamAPI_ISteamUser_CancelAuthTicket: jmp *_fwd_SteamAPI_ISteamUser_CancelAuthTicket(%rip)");
static void *_fwd_SteamAPI_ISteamUser_CancelAuthTicket = NULL;

/* SteamAPI_ISteamUser_DecompressVoice */
__asm__(".globl SteamAPI_ISteamUser_DecompressVoice");
__asm__("SteamAPI_ISteamUser_DecompressVoice: jmp *_fwd_SteamAPI_ISteamUser_DecompressVoice(%rip)");
static void *_fwd_SteamAPI_ISteamUser_DecompressVoice = NULL;

/* SteamAPI_ISteamUser_EndAuthSession */
__asm__(".globl SteamAPI_ISteamUser_EndAuthSession");
__asm__("SteamAPI_ISteamUser_EndAuthSession: jmp *_fwd_SteamAPI_ISteamUser_EndAuthSession(%rip)");
static void *_fwd_SteamAPI_ISteamUser_EndAuthSession = NULL;

/* SteamAPI_ISteamUser_GetAuthSessionTicket */
__asm__(".globl SteamAPI_ISteamUser_GetAuthSessionTicket");
__asm__("SteamAPI_ISteamUser_GetAuthSessionTicket: jmp *_fwd_SteamAPI_ISteamUser_GetAuthSessionTicket(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetAuthSessionTicket = NULL;

/* SteamAPI_ISteamUser_GetAuthTicketForWebApi */
__asm__(".globl SteamAPI_ISteamUser_GetAuthTicketForWebApi");
__asm__("SteamAPI_ISteamUser_GetAuthTicketForWebApi: jmp *_fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi = NULL;

/* SteamAPI_ISteamUser_GetAvailableVoice */
__asm__(".globl SteamAPI_ISteamUser_GetAvailableVoice");
__asm__("SteamAPI_ISteamUser_GetAvailableVoice: jmp *_fwd_SteamAPI_ISteamUser_GetAvailableVoice(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetAvailableVoice = NULL;

/* SteamAPI_ISteamUser_GetDurationControl */
__asm__(".globl SteamAPI_ISteamUser_GetDurationControl");
__asm__("SteamAPI_ISteamUser_GetDurationControl: jmp *_fwd_SteamAPI_ISteamUser_GetDurationControl(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetDurationControl = NULL;

/* SteamAPI_ISteamUser_GetEncryptedAppTicket */
__asm__(".globl SteamAPI_ISteamUser_GetEncryptedAppTicket");
__asm__("SteamAPI_ISteamUser_GetEncryptedAppTicket: jmp *_fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket = NULL;

/* SteamAPI_ISteamUser_GetGameBadgeLevel */
__asm__(".globl SteamAPI_ISteamUser_GetGameBadgeLevel");
__asm__("SteamAPI_ISteamUser_GetGameBadgeLevel: jmp *_fwd_SteamAPI_ISteamUser_GetGameBadgeLevel(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetGameBadgeLevel = NULL;

/* SteamAPI_ISteamUser_GetHSteamUser */
__asm__(".globl SteamAPI_ISteamUser_GetHSteamUser");
__asm__("SteamAPI_ISteamUser_GetHSteamUser: jmp *_fwd_SteamAPI_ISteamUser_GetHSteamUser(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetHSteamUser = NULL;

/* SteamAPI_ISteamUser_GetMarketEligibility */
__asm__(".globl SteamAPI_ISteamUser_GetMarketEligibility");
__asm__("SteamAPI_ISteamUser_GetMarketEligibility: jmp *_fwd_SteamAPI_ISteamUser_GetMarketEligibility(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetMarketEligibility = NULL;

/* SteamAPI_ISteamUser_GetPlayerSteamLevel */
__asm__(".globl SteamAPI_ISteamUser_GetPlayerSteamLevel");
__asm__("SteamAPI_ISteamUser_GetPlayerSteamLevel: jmp *_fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel = NULL;

/* SteamAPI_ISteamUser_GetSteamID */
__asm__(".globl SteamAPI_ISteamUser_GetSteamID");
__asm__("SteamAPI_ISteamUser_GetSteamID: jmp *_fwd_SteamAPI_ISteamUser_GetSteamID(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetSteamID = NULL;

/* SteamAPI_ISteamUser_GetUserDataFolder */
__asm__(".globl SteamAPI_ISteamUser_GetUserDataFolder");
__asm__("SteamAPI_ISteamUser_GetUserDataFolder: jmp *_fwd_SteamAPI_ISteamUser_GetUserDataFolder(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetUserDataFolder = NULL;

/* SteamAPI_ISteamUser_GetVoice */
__asm__(".globl SteamAPI_ISteamUser_GetVoice");
__asm__("SteamAPI_ISteamUser_GetVoice: jmp *_fwd_SteamAPI_ISteamUser_GetVoice(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetVoice = NULL;

/* SteamAPI_ISteamUser_GetVoiceOptimalSampleRate */
__asm__(".globl SteamAPI_ISteamUser_GetVoiceOptimalSampleRate");
__asm__("SteamAPI_ISteamUser_GetVoiceOptimalSampleRate: jmp *_fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate(%rip)");
static void *_fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate = NULL;

/* SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED */
__asm__(".globl SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED");
__asm__("SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED: jmp *_fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED(%rip)");
static void *_fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED = NULL;

/* SteamAPI_ISteamUser_RequestEncryptedAppTicket */
__asm__(".globl SteamAPI_ISteamUser_RequestEncryptedAppTicket");
__asm__("SteamAPI_ISteamUser_RequestEncryptedAppTicket: jmp *_fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket(%rip)");
static void *_fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket = NULL;

/* SteamAPI_ISteamUser_RequestStoreAuthURL */
__asm__(".globl SteamAPI_ISteamUser_RequestStoreAuthURL");
__asm__("SteamAPI_ISteamUser_RequestStoreAuthURL: jmp *_fwd_SteamAPI_ISteamUser_RequestStoreAuthURL(%rip)");
static void *_fwd_SteamAPI_ISteamUser_RequestStoreAuthURL = NULL;

/* SteamAPI_ISteamUser_StartVoiceRecording */
__asm__(".globl SteamAPI_ISteamUser_StartVoiceRecording");
__asm__("SteamAPI_ISteamUser_StartVoiceRecording: jmp *_fwd_SteamAPI_ISteamUser_StartVoiceRecording(%rip)");
static void *_fwd_SteamAPI_ISteamUser_StartVoiceRecording = NULL;

/* SteamAPI_ISteamUserStats_AttachLeaderboardUGC */
__asm__(".globl SteamAPI_ISteamUserStats_AttachLeaderboardUGC");
__asm__("SteamAPI_ISteamUserStats_AttachLeaderboardUGC: jmp *_fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC = NULL;

/* SteamAPI_ISteamUserStats_ClearAchievement */
__asm__(".globl SteamAPI_ISteamUserStats_ClearAchievement");
__asm__("SteamAPI_ISteamUserStats_ClearAchievement: jmp *_fwd_SteamAPI_ISteamUserStats_ClearAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_ClearAchievement = NULL;

/* SteamAPI_ISteamUserStats_DownloadLeaderboardEntries */
__asm__(".globl SteamAPI_ISteamUserStats_DownloadLeaderboardEntries");
__asm__("SteamAPI_ISteamUserStats_DownloadLeaderboardEntries: jmp *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries = NULL;

/* SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers */
__asm__(".globl SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers");
__asm__("SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers: jmp *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers = NULL;

/* SteamAPI_ISteamUserStats_FindLeaderboard */
__asm__(".globl SteamAPI_ISteamUserStats_FindLeaderboard");
__asm__("SteamAPI_ISteamUserStats_FindLeaderboard: jmp *_fwd_SteamAPI_ISteamUserStats_FindLeaderboard(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_FindLeaderboard = NULL;

/* SteamAPI_ISteamUserStats_FindOrCreateLeaderboard */
__asm__(".globl SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
__asm__("SteamAPI_ISteamUserStats_FindOrCreateLeaderboard: jmp *_fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard = NULL;

/* SteamAPI_ISteamUserStats_GetAchievement */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievement");
__asm__("SteamAPI_ISteamUserStats_GetAchievement: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievement = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementAchievedPercent */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementAchievedPercent");
__asm__("SteamAPI_ISteamUserStats_GetAchievementAchievedPercent: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime");
__asm__("SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute");
__asm__("SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementIcon */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementIcon");
__asm__("SteamAPI_ISteamUserStats_GetAchievementIcon: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementIcon(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementIcon = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementName */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementName");
__asm__("SteamAPI_ISteamUserStats_GetAchievementName: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementName(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementName = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat");
__asm__("SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat = NULL;

/* SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 */
__asm__(".globl SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32");
__asm__("SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32: jmp *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 = NULL;

/* SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry */
__asm__(".globl SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry");
__asm__("SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry: jmp *_fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry = NULL;

/* SteamAPI_ISteamUserStats_GetGlobalStatDouble */
__asm__(".globl SteamAPI_ISteamUserStats_GetGlobalStatDouble");
__asm__("SteamAPI_ISteamUserStats_GetGlobalStatDouble: jmp *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble = NULL;

/* SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble */
__asm__(".globl SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble");
__asm__("SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble: jmp *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble = NULL;

/* SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 */
__asm__(".globl SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64");
__asm__("SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64: jmp *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 = NULL;

/* SteamAPI_ISteamUserStats_GetGlobalStatInt64 */
__asm__(".globl SteamAPI_ISteamUserStats_GetGlobalStatInt64");
__asm__("SteamAPI_ISteamUserStats_GetGlobalStatInt64: jmp *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64 = NULL;

/* SteamAPI_ISteamUserStats_GetLeaderboardDisplayType */
__asm__(".globl SteamAPI_ISteamUserStats_GetLeaderboardDisplayType");
__asm__("SteamAPI_ISteamUserStats_GetLeaderboardDisplayType: jmp *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType = NULL;

/* SteamAPI_ISteamUserStats_GetLeaderboardEntryCount */
__asm__(".globl SteamAPI_ISteamUserStats_GetLeaderboardEntryCount");
__asm__("SteamAPI_ISteamUserStats_GetLeaderboardEntryCount: jmp *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount = NULL;

/* SteamAPI_ISteamUserStats_GetLeaderboardName */
__asm__(".globl SteamAPI_ISteamUserStats_GetLeaderboardName");
__asm__("SteamAPI_ISteamUserStats_GetLeaderboardName: jmp *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardName(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardName = NULL;

/* SteamAPI_ISteamUserStats_GetLeaderboardSortMethod */
__asm__(".globl SteamAPI_ISteamUserStats_GetLeaderboardSortMethod");
__asm__("SteamAPI_ISteamUserStats_GetLeaderboardSortMethod: jmp *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod = NULL;

/* SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo */
__asm__(".globl SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo");
__asm__("SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo: jmp *_fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo = NULL;

/* SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo */
__asm__(".globl SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo");
__asm__("SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo: jmp *_fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo = NULL;

/* SteamAPI_ISteamUserStats_GetNumAchievements */
__asm__(".globl SteamAPI_ISteamUserStats_GetNumAchievements");
__asm__("SteamAPI_ISteamUserStats_GetNumAchievements: jmp *_fwd_SteamAPI_ISteamUserStats_GetNumAchievements(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetNumAchievements = NULL;

/* SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers */
__asm__(".globl SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers");
__asm__("SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers: jmp *_fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers = NULL;

/* SteamAPI_ISteamUserStats_GetStatFloat */
__asm__(".globl SteamAPI_ISteamUserStats_GetStatFloat");
__asm__("SteamAPI_ISteamUserStats_GetStatFloat: jmp *_fwd_SteamAPI_ISteamUserStats_GetStatFloat(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetStatFloat = NULL;

/* SteamAPI_ISteamUserStats_GetStatInt32 */
__asm__(".globl SteamAPI_ISteamUserStats_GetStatInt32");
__asm__("SteamAPI_ISteamUserStats_GetStatInt32: jmp *_fwd_SteamAPI_ISteamUserStats_GetStatInt32(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetStatInt32 = NULL;

/* SteamAPI_ISteamUserStats_GetUserAchievement */
__asm__(".globl SteamAPI_ISteamUserStats_GetUserAchievement");
__asm__("SteamAPI_ISteamUserStats_GetUserAchievement: jmp *_fwd_SteamAPI_ISteamUserStats_GetUserAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetUserAchievement = NULL;

/* SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime */
__asm__(".globl SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime");
__asm__("SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime: jmp *_fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime = NULL;

/* SteamAPI_ISteamUserStats_GetUserStatFloat */
__asm__(".globl SteamAPI_ISteamUserStats_GetUserStatFloat");
__asm__("SteamAPI_ISteamUserStats_GetUserStatFloat: jmp *_fwd_SteamAPI_ISteamUserStats_GetUserStatFloat(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetUserStatFloat = NULL;

/* SteamAPI_ISteamUserStats_GetUserStatInt32 */
__asm__(".globl SteamAPI_ISteamUserStats_GetUserStatInt32");
__asm__("SteamAPI_ISteamUserStats_GetUserStatInt32: jmp *_fwd_SteamAPI_ISteamUserStats_GetUserStatInt32(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_GetUserStatInt32 = NULL;

/* SteamAPI_ISteamUserStats_IndicateAchievementProgress */
__asm__(".globl SteamAPI_ISteamUserStats_IndicateAchievementProgress");
__asm__("SteamAPI_ISteamUserStats_IndicateAchievementProgress: jmp *_fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress = NULL;

/* SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages */
__asm__(".globl SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages");
__asm__("SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages: jmp *_fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages = NULL;

/* SteamAPI_ISteamUserStats_RequestGlobalStats */
__asm__(".globl SteamAPI_ISteamUserStats_RequestGlobalStats");
__asm__("SteamAPI_ISteamUserStats_RequestGlobalStats: jmp *_fwd_SteamAPI_ISteamUserStats_RequestGlobalStats(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_RequestGlobalStats = NULL;

/* SteamAPI_ISteamUserStats_RequestUserStats */
__asm__(".globl SteamAPI_ISteamUserStats_RequestUserStats");
__asm__("SteamAPI_ISteamUserStats_RequestUserStats: jmp *_fwd_SteamAPI_ISteamUserStats_RequestUserStats(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_RequestUserStats = NULL;

/* SteamAPI_ISteamUserStats_ResetAllStats */
__asm__(".globl SteamAPI_ISteamUserStats_ResetAllStats");
__asm__("SteamAPI_ISteamUserStats_ResetAllStats: jmp *_fwd_SteamAPI_ISteamUserStats_ResetAllStats(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_ResetAllStats = NULL;

/* SteamAPI_ISteamUserStats_SetAchievement */
__asm__(".globl SteamAPI_ISteamUserStats_SetAchievement");
__asm__("SteamAPI_ISteamUserStats_SetAchievement: jmp *_fwd_SteamAPI_ISteamUserStats_SetAchievement(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_SetAchievement = NULL;

/* SteamAPI_ISteamUserStats_SetStatFloat */
__asm__(".globl SteamAPI_ISteamUserStats_SetStatFloat");
__asm__("SteamAPI_ISteamUserStats_SetStatFloat: jmp *_fwd_SteamAPI_ISteamUserStats_SetStatFloat(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_SetStatFloat = NULL;

/* SteamAPI_ISteamUserStats_SetStatInt32 */
__asm__(".globl SteamAPI_ISteamUserStats_SetStatInt32");
__asm__("SteamAPI_ISteamUserStats_SetStatInt32: jmp *_fwd_SteamAPI_ISteamUserStats_SetStatInt32(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_SetStatInt32 = NULL;

/* SteamAPI_ISteamUserStats_StoreStats */
__asm__(".globl SteamAPI_ISteamUserStats_StoreStats");
__asm__("SteamAPI_ISteamUserStats_StoreStats: jmp *_fwd_SteamAPI_ISteamUserStats_StoreStats(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_StoreStats = NULL;

/* SteamAPI_ISteamUserStats_UpdateAvgRateStat */
__asm__(".globl SteamAPI_ISteamUserStats_UpdateAvgRateStat");
__asm__("SteamAPI_ISteamUserStats_UpdateAvgRateStat: jmp *_fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat = NULL;

/* SteamAPI_ISteamUserStats_UploadLeaderboardScore */
__asm__(".globl SteamAPI_ISteamUserStats_UploadLeaderboardScore");
__asm__("SteamAPI_ISteamUserStats_UploadLeaderboardScore: jmp *_fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore(%rip)");
static void *_fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore = NULL;

/* SteamAPI_ISteamUser_StopVoiceRecording */
__asm__(".globl SteamAPI_ISteamUser_StopVoiceRecording");
__asm__("SteamAPI_ISteamUser_StopVoiceRecording: jmp *_fwd_SteamAPI_ISteamUser_StopVoiceRecording(%rip)");
static void *_fwd_SteamAPI_ISteamUser_StopVoiceRecording = NULL;

/* SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED */
__asm__(".globl SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED");
__asm__("SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED: jmp *_fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED(%rip)");
static void *_fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED = NULL;

/* SteamAPI_ISteamUser_TrackAppUsageEvent */
__asm__(".globl SteamAPI_ISteamUser_TrackAppUsageEvent");
__asm__("SteamAPI_ISteamUser_TrackAppUsageEvent: jmp *_fwd_SteamAPI_ISteamUser_TrackAppUsageEvent(%rip)");
static void *_fwd_SteamAPI_ISteamUser_TrackAppUsageEvent = NULL;

/* SteamAPI_ISteamUtils_BOverlayNeedsPresent */
__asm__(".globl SteamAPI_ISteamUtils_BOverlayNeedsPresent");
__asm__("SteamAPI_ISteamUtils_BOverlayNeedsPresent: jmp *_fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent = NULL;

/* SteamAPI_ISteamUtils_CheckFileSignature */
__asm__(".globl SteamAPI_ISteamUtils_CheckFileSignature");
__asm__("SteamAPI_ISteamUtils_CheckFileSignature: jmp *_fwd_SteamAPI_ISteamUtils_CheckFileSignature(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_CheckFileSignature = NULL;

/* SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput */
__asm__(".globl SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput");
__asm__("SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput: jmp *_fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = NULL;

/* SteamAPI_ISteamUtils_DismissGamepadTextInput */
__asm__(".globl SteamAPI_ISteamUtils_DismissGamepadTextInput");
__asm__("SteamAPI_ISteamUtils_DismissGamepadTextInput: jmp *_fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput = NULL;

/* SteamAPI_ISteamUtils_FilterText */
__asm__(".globl SteamAPI_ISteamUtils_FilterText");
__asm__("SteamAPI_ISteamUtils_FilterText: jmp *_fwd_SteamAPI_ISteamUtils_FilterText(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_FilterText = NULL;

/* SteamAPI_ISteamUtils_GetAPICallFailureReason */
__asm__(".globl SteamAPI_ISteamUtils_GetAPICallFailureReason");
__asm__("SteamAPI_ISteamUtils_GetAPICallFailureReason: jmp *_fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason = NULL;

/* SteamAPI_ISteamUtils_GetAPICallResult */
__asm__(".globl SteamAPI_ISteamUtils_GetAPICallResult");
__asm__("SteamAPI_ISteamUtils_GetAPICallResult: jmp *_fwd_SteamAPI_ISteamUtils_GetAPICallResult(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetAPICallResult = NULL;

/* SteamAPI_ISteamUtils_GetAppID */
__asm__(".globl SteamAPI_ISteamUtils_GetAppID");
__asm__("SteamAPI_ISteamUtils_GetAppID: jmp *_fwd_SteamAPI_ISteamUtils_GetAppID(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetAppID = NULL;

/* SteamAPI_ISteamUtils_GetConnectedUniverse */
__asm__(".globl SteamAPI_ISteamUtils_GetConnectedUniverse");
__asm__("SteamAPI_ISteamUtils_GetConnectedUniverse: jmp *_fwd_SteamAPI_ISteamUtils_GetConnectedUniverse(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetConnectedUniverse = NULL;

/* SteamAPI_ISteamUtils_GetCurrentBatteryPower */
__asm__(".globl SteamAPI_ISteamUtils_GetCurrentBatteryPower");
__asm__("SteamAPI_ISteamUtils_GetCurrentBatteryPower: jmp *_fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower = NULL;

/* SteamAPI_ISteamUtils_GetEnteredGamepadTextInput */
__asm__(".globl SteamAPI_ISteamUtils_GetEnteredGamepadTextInput");
__asm__("SteamAPI_ISteamUtils_GetEnteredGamepadTextInput: jmp *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput = NULL;

/* SteamAPI_ISteamUtils_GetEnteredGamepadTextLength */
__asm__(".globl SteamAPI_ISteamUtils_GetEnteredGamepadTextLength");
__asm__("SteamAPI_ISteamUtils_GetEnteredGamepadTextLength: jmp *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength = NULL;

/* SteamAPI_ISteamUtils_GetImageRGBA */
__asm__(".globl SteamAPI_ISteamUtils_GetImageRGBA");
__asm__("SteamAPI_ISteamUtils_GetImageRGBA: jmp *_fwd_SteamAPI_ISteamUtils_GetImageRGBA(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetImageRGBA = NULL;

/* SteamAPI_ISteamUtils_GetImageSize */
__asm__(".globl SteamAPI_ISteamUtils_GetImageSize");
__asm__("SteamAPI_ISteamUtils_GetImageSize: jmp *_fwd_SteamAPI_ISteamUtils_GetImageSize(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetImageSize = NULL;

/* SteamAPI_ISteamUtils_GetIPCCallCount */
__asm__(".globl SteamAPI_ISteamUtils_GetIPCCallCount");
__asm__("SteamAPI_ISteamUtils_GetIPCCallCount: jmp *_fwd_SteamAPI_ISteamUtils_GetIPCCallCount(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetIPCCallCount = NULL;

/* SteamAPI_ISteamUtils_GetIPCountry */
__asm__(".globl SteamAPI_ISteamUtils_GetIPCountry");
__asm__("SteamAPI_ISteamUtils_GetIPCountry: jmp *_fwd_SteamAPI_ISteamUtils_GetIPCountry(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetIPCountry = NULL;

/* SteamAPI_ISteamUtils_GetIPv6ConnectivityState */
__asm__(".globl SteamAPI_ISteamUtils_GetIPv6ConnectivityState");
__asm__("SteamAPI_ISteamUtils_GetIPv6ConnectivityState: jmp *_fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState = NULL;

/* SteamAPI_ISteamUtils_GetSecondsSinceAppActive */
__asm__(".globl SteamAPI_ISteamUtils_GetSecondsSinceAppActive");
__asm__("SteamAPI_ISteamUtils_GetSecondsSinceAppActive: jmp *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive = NULL;

/* SteamAPI_ISteamUtils_GetSecondsSinceComputerActive */
__asm__(".globl SteamAPI_ISteamUtils_GetSecondsSinceComputerActive");
__asm__("SteamAPI_ISteamUtils_GetSecondsSinceComputerActive: jmp *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive = NULL;

/* SteamAPI_ISteamUtils_GetServerRealTime */
__asm__(".globl SteamAPI_ISteamUtils_GetServerRealTime");
__asm__("SteamAPI_ISteamUtils_GetServerRealTime: jmp *_fwd_SteamAPI_ISteamUtils_GetServerRealTime(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetServerRealTime = NULL;

/* SteamAPI_ISteamUtils_GetSteamUILanguage */
__asm__(".globl SteamAPI_ISteamUtils_GetSteamUILanguage");
__asm__("SteamAPI_ISteamUtils_GetSteamUILanguage: jmp *_fwd_SteamAPI_ISteamUtils_GetSteamUILanguage(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_GetSteamUILanguage = NULL;

/* SteamAPI_ISteamUtils_InitFilterText */
__asm__(".globl SteamAPI_ISteamUtils_InitFilterText");
__asm__("SteamAPI_ISteamUtils_InitFilterText: jmp *_fwd_SteamAPI_ISteamUtils_InitFilterText(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_InitFilterText = NULL;

/* SteamAPI_ISteamUtils_IsAPICallCompleted */
__asm__(".globl SteamAPI_ISteamUtils_IsAPICallCompleted");
__asm__("SteamAPI_ISteamUtils_IsAPICallCompleted: jmp *_fwd_SteamAPI_ISteamUtils_IsAPICallCompleted(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsAPICallCompleted = NULL;

/* SteamAPI_ISteamUtils_IsOverlayEnabled */
__asm__(".globl SteamAPI_ISteamUtils_IsOverlayEnabled");
__asm__("SteamAPI_ISteamUtils_IsOverlayEnabled: jmp *_fwd_SteamAPI_ISteamUtils_IsOverlayEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsOverlayEnabled = NULL;

/* SteamAPI_ISteamUtils_IsSteamChinaLauncher */
__asm__(".globl SteamAPI_ISteamUtils_IsSteamChinaLauncher");
__asm__("SteamAPI_ISteamUtils_IsSteamChinaLauncher: jmp *_fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher = NULL;

/* SteamAPI_ISteamUtils_IsSteamInBigPictureMode */
__asm__(".globl SteamAPI_ISteamUtils_IsSteamInBigPictureMode");
__asm__("SteamAPI_ISteamUtils_IsSteamInBigPictureMode: jmp *_fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode = NULL;

/* SteamAPI_ISteamUtils_IsSteamRunningInVR */
__asm__(".globl SteamAPI_ISteamUtils_IsSteamRunningInVR");
__asm__("SteamAPI_ISteamUtils_IsSteamRunningInVR: jmp *_fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR = NULL;

/* SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck */
__asm__(".globl SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");
__asm__("SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck: jmp *_fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = NULL;

/* SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled */
__asm__(".globl SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled");
__asm__("SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled: jmp *_fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled = NULL;

/* SteamAPI_ISteamUtils_SetGameLauncherMode */
__asm__(".globl SteamAPI_ISteamUtils_SetGameLauncherMode");
__asm__("SteamAPI_ISteamUtils_SetGameLauncherMode: jmp *_fwd_SteamAPI_ISteamUtils_SetGameLauncherMode(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_SetGameLauncherMode = NULL;

/* SteamAPI_ISteamUtils_SetOverlayNotificationInset */
__asm__(".globl SteamAPI_ISteamUtils_SetOverlayNotificationInset");
__asm__("SteamAPI_ISteamUtils_SetOverlayNotificationInset: jmp *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset = NULL;

/* SteamAPI_ISteamUtils_SetOverlayNotificationPosition */
__asm__(".globl SteamAPI_ISteamUtils_SetOverlayNotificationPosition");
__asm__("SteamAPI_ISteamUtils_SetOverlayNotificationPosition: jmp *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition = NULL;

/* SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled */
__asm__(".globl SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled");
__asm__("SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled: jmp *_fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled = NULL;

/* SteamAPI_ISteamUtils_SetWarningMessageHook */
__asm__(".globl SteamAPI_ISteamUtils_SetWarningMessageHook");
__asm__("SteamAPI_ISteamUtils_SetWarningMessageHook: jmp *_fwd_SteamAPI_ISteamUtils_SetWarningMessageHook(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_SetWarningMessageHook = NULL;

/* SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput */
__asm__(".globl SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput");
__asm__("SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput: jmp *_fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = NULL;

/* SteamAPI_ISteamUtils_ShowGamepadTextInput */
__asm__(".globl SteamAPI_ISteamUtils_ShowGamepadTextInput");
__asm__("SteamAPI_ISteamUtils_ShowGamepadTextInput: jmp *_fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput = NULL;

/* SteamAPI_ISteamUtils_StartVRDashboard */
__asm__(".globl SteamAPI_ISteamUtils_StartVRDashboard");
__asm__("SteamAPI_ISteamUtils_StartVRDashboard: jmp *_fwd_SteamAPI_ISteamUtils_StartVRDashboard(%rip)");
static void *_fwd_SteamAPI_ISteamUtils_StartVRDashboard = NULL;

/* SteamAPI_ISteamVideo_GetOPFSettings */
__asm__(".globl SteamAPI_ISteamVideo_GetOPFSettings");
__asm__("SteamAPI_ISteamVideo_GetOPFSettings: jmp *_fwd_SteamAPI_ISteamVideo_GetOPFSettings(%rip)");
static void *_fwd_SteamAPI_ISteamVideo_GetOPFSettings = NULL;

/* SteamAPI_ISteamVideo_GetOPFStringForApp */
__asm__(".globl SteamAPI_ISteamVideo_GetOPFStringForApp");
__asm__("SteamAPI_ISteamVideo_GetOPFStringForApp: jmp *_fwd_SteamAPI_ISteamVideo_GetOPFStringForApp(%rip)");
static void *_fwd_SteamAPI_ISteamVideo_GetOPFStringForApp = NULL;

/* SteamAPI_ISteamVideo_GetVideoURL */
__asm__(".globl SteamAPI_ISteamVideo_GetVideoURL");
__asm__("SteamAPI_ISteamVideo_GetVideoURL: jmp *_fwd_SteamAPI_ISteamVideo_GetVideoURL(%rip)");
static void *_fwd_SteamAPI_ISteamVideo_GetVideoURL = NULL;

/* SteamAPI_ISteamVideo_IsBroadcasting */
__asm__(".globl SteamAPI_ISteamVideo_IsBroadcasting");
__asm__("SteamAPI_ISteamVideo_IsBroadcasting: jmp *_fwd_SteamAPI_ISteamVideo_IsBroadcasting(%rip)");
static void *_fwd_SteamAPI_ISteamVideo_IsBroadcasting = NULL;

/* SteamAPI_ManualDispatch_FreeLastCallback */
__asm__(".globl SteamAPI_ManualDispatch_FreeLastCallback");
__asm__("SteamAPI_ManualDispatch_FreeLastCallback: jmp *_fwd_SteamAPI_ManualDispatch_FreeLastCallback(%rip)");
static void *_fwd_SteamAPI_ManualDispatch_FreeLastCallback = NULL;

/* SteamAPI_ManualDispatch_GetAPICallResult */
__asm__(".globl SteamAPI_ManualDispatch_GetAPICallResult");
__asm__("SteamAPI_ManualDispatch_GetAPICallResult: jmp *_fwd_SteamAPI_ManualDispatch_GetAPICallResult(%rip)");
static void *_fwd_SteamAPI_ManualDispatch_GetAPICallResult = NULL;

/* SteamAPI_ManualDispatch_GetNextCallback */
__asm__(".globl SteamAPI_ManualDispatch_GetNextCallback");
__asm__("SteamAPI_ManualDispatch_GetNextCallback: jmp *_fwd_SteamAPI_ManualDispatch_GetNextCallback(%rip)");
static void *_fwd_SteamAPI_ManualDispatch_GetNextCallback = NULL;

/* SteamAPI_ManualDispatch_Init */
__asm__(".globl SteamAPI_ManualDispatch_Init");
__asm__("SteamAPI_ManualDispatch_Init: jmp *_fwd_SteamAPI_ManualDispatch_Init(%rip)");
static void *_fwd_SteamAPI_ManualDispatch_Init = NULL;

/* SteamAPI_ManualDispatch_RunFrame */
__asm__(".globl SteamAPI_ManualDispatch_RunFrame");
__asm__("SteamAPI_ManualDispatch_RunFrame: jmp *_fwd_SteamAPI_ManualDispatch_RunFrame(%rip)");
static void *_fwd_SteamAPI_ManualDispatch_RunFrame = NULL;

/* SteamAPI_MatchMakingKeyValuePair_t_Construct */
__asm__(".globl SteamAPI_MatchMakingKeyValuePair_t_Construct");
__asm__("SteamAPI_MatchMakingKeyValuePair_t_Construct: jmp *_fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct(%rip)");
static void *_fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct = NULL;

/* SteamAPI_RegisterCallback */
__asm__(".globl SteamAPI_RegisterCallback");
__asm__("SteamAPI_RegisterCallback: jmp *_fwd_SteamAPI_RegisterCallback(%rip)");
static void *_fwd_SteamAPI_RegisterCallback = NULL;

/* SteamAPI_RegisterCallResult */
__asm__(".globl SteamAPI_RegisterCallResult");
__asm__("SteamAPI_RegisterCallResult: jmp *_fwd_SteamAPI_RegisterCallResult(%rip)");
static void *_fwd_SteamAPI_RegisterCallResult = NULL;

/* SteamAPI_ReleaseCurrentThreadMemory */
__asm__(".globl SteamAPI_ReleaseCurrentThreadMemory");
__asm__("SteamAPI_ReleaseCurrentThreadMemory: jmp *_fwd_SteamAPI_ReleaseCurrentThreadMemory(%rip)");
static void *_fwd_SteamAPI_ReleaseCurrentThreadMemory = NULL;

/* SteamAPI_RestartAppIfNecessary */
__asm__(".globl SteamAPI_RestartAppIfNecessary");
__asm__("SteamAPI_RestartAppIfNecessary: jmp *_fwd_SteamAPI_RestartAppIfNecessary(%rip)");
static void *_fwd_SteamAPI_RestartAppIfNecessary = NULL;

/* SteamAPI_RunCallbacks */
__asm__(".globl SteamAPI_RunCallbacks");
__asm__("SteamAPI_RunCallbacks: jmp *_fwd_SteamAPI_RunCallbacks(%rip)");
static void *_fwd_SteamAPI_RunCallbacks = NULL;

/* SteamAPI_servernetadr_t_Assign */
__asm__(".globl SteamAPI_servernetadr_t_Assign");
__asm__("SteamAPI_servernetadr_t_Assign: jmp *_fwd_SteamAPI_servernetadr_t_Assign(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_Assign = NULL;

/* SteamAPI_servernetadr_t_Construct */
__asm__(".globl SteamAPI_servernetadr_t_Construct");
__asm__("SteamAPI_servernetadr_t_Construct: jmp *_fwd_SteamAPI_servernetadr_t_Construct(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_Construct = NULL;

/* SteamAPI_servernetadr_t_GetConnectionAddressString */
__asm__(".globl SteamAPI_servernetadr_t_GetConnectionAddressString");
__asm__("SteamAPI_servernetadr_t_GetConnectionAddressString: jmp *_fwd_SteamAPI_servernetadr_t_GetConnectionAddressString(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_GetConnectionAddressString = NULL;

/* SteamAPI_servernetadr_t_GetConnectionPort */
__asm__(".globl SteamAPI_servernetadr_t_GetConnectionPort");
__asm__("SteamAPI_servernetadr_t_GetConnectionPort: jmp *_fwd_SteamAPI_servernetadr_t_GetConnectionPort(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_GetConnectionPort = NULL;

/* SteamAPI_servernetadr_t_GetIP */
__asm__(".globl SteamAPI_servernetadr_t_GetIP");
__asm__("SteamAPI_servernetadr_t_GetIP: jmp *_fwd_SteamAPI_servernetadr_t_GetIP(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_GetIP = NULL;

/* SteamAPI_servernetadr_t_GetQueryAddressString */
__asm__(".globl SteamAPI_servernetadr_t_GetQueryAddressString");
__asm__("SteamAPI_servernetadr_t_GetQueryAddressString: jmp *_fwd_SteamAPI_servernetadr_t_GetQueryAddressString(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_GetQueryAddressString = NULL;

/* SteamAPI_servernetadr_t_GetQueryPort */
__asm__(".globl SteamAPI_servernetadr_t_GetQueryPort");
__asm__("SteamAPI_servernetadr_t_GetQueryPort: jmp *_fwd_SteamAPI_servernetadr_t_GetQueryPort(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_GetQueryPort = NULL;

/* SteamAPI_servernetadr_t_Init */
__asm__(".globl SteamAPI_servernetadr_t_Init");
__asm__("SteamAPI_servernetadr_t_Init: jmp *_fwd_SteamAPI_servernetadr_t_Init(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_Init = NULL;

/* SteamAPI_servernetadr_t_IsLessThan */
__asm__(".globl SteamAPI_servernetadr_t_IsLessThan");
__asm__("SteamAPI_servernetadr_t_IsLessThan: jmp *_fwd_SteamAPI_servernetadr_t_IsLessThan(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_IsLessThan = NULL;

/* SteamAPI_servernetadr_t_SetConnectionPort */
__asm__(".globl SteamAPI_servernetadr_t_SetConnectionPort");
__asm__("SteamAPI_servernetadr_t_SetConnectionPort: jmp *_fwd_SteamAPI_servernetadr_t_SetConnectionPort(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_SetConnectionPort = NULL;

/* SteamAPI_servernetadr_t_SetIP */
__asm__(".globl SteamAPI_servernetadr_t_SetIP");
__asm__("SteamAPI_servernetadr_t_SetIP: jmp *_fwd_SteamAPI_servernetadr_t_SetIP(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_SetIP = NULL;

/* SteamAPI_servernetadr_t_SetQueryPort */
__asm__(".globl SteamAPI_servernetadr_t_SetQueryPort");
__asm__("SteamAPI_servernetadr_t_SetQueryPort: jmp *_fwd_SteamAPI_servernetadr_t_SetQueryPort(%rip)");
static void *_fwd_SteamAPI_servernetadr_t_SetQueryPort = NULL;

/* SteamAPI_SetBreakpadAppID */
__asm__(".globl SteamAPI_SetBreakpadAppID");
__asm__("SteamAPI_SetBreakpadAppID: jmp *_fwd_SteamAPI_SetBreakpadAppID(%rip)");
static void *_fwd_SteamAPI_SetBreakpadAppID = NULL;

/* SteamAPI_SetMiniDumpComment */
__asm__(".globl SteamAPI_SetMiniDumpComment");
__asm__("SteamAPI_SetMiniDumpComment: jmp *_fwd_SteamAPI_SetMiniDumpComment(%rip)");
static void *_fwd_SteamAPI_SetMiniDumpComment = NULL;

/* SteamAPI_SetTryCatchCallbacks */
__asm__(".globl SteamAPI_SetTryCatchCallbacks");
__asm__("SteamAPI_SetTryCatchCallbacks: jmp *_fwd_SteamAPI_SetTryCatchCallbacks(%rip)");
static void *_fwd_SteamAPI_SetTryCatchCallbacks = NULL;

/* SteamAPI_Shutdown */
__asm__(".globl SteamAPI_Shutdown");
__asm__("SteamAPI_Shutdown: jmp *_fwd_SteamAPI_Shutdown(%rip)");
static void *_fwd_SteamAPI_Shutdown = NULL;

/* SteamAPI_SteamApps_v008 */
__asm__(".globl SteamAPI_SteamApps_v008");
__asm__("SteamAPI_SteamApps_v008: jmp *_fwd_SteamAPI_SteamApps_v008(%rip)");
static void *_fwd_SteamAPI_SteamApps_v008 = NULL;

/* SteamAPI_SteamController_v008 */
__asm__(".globl SteamAPI_SteamController_v008");
__asm__("SteamAPI_SteamController_v008: jmp *_fwd_SteamAPI_SteamController_v008(%rip)");
static void *_fwd_SteamAPI_SteamController_v008 = NULL;

/* SteamAPI_SteamDatagramHostedAddress_Clear */
__asm__(".globl SteamAPI_SteamDatagramHostedAddress_Clear");
__asm__("SteamAPI_SteamDatagramHostedAddress_Clear: jmp *_fwd_SteamAPI_SteamDatagramHostedAddress_Clear(%rip)");
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_Clear = NULL;

/* SteamAPI_SteamDatagramHostedAddress_GetPopID */
__asm__(".globl SteamAPI_SteamDatagramHostedAddress_GetPopID");
__asm__("SteamAPI_SteamDatagramHostedAddress_GetPopID: jmp *_fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID(%rip)");
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID = NULL;

/* SteamAPI_SteamDatagramHostedAddress_SetDevAddress */
__asm__(".globl SteamAPI_SteamDatagramHostedAddress_SetDevAddress");
__asm__("SteamAPI_SteamDatagramHostedAddress_SetDevAddress: jmp *_fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress(%rip)");
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress = NULL;

/* SteamAPI_SteamFriends_v017 */
__asm__(".globl SteamAPI_SteamFriends_v017");
__asm__("SteamAPI_SteamFriends_v017: jmp *_fwd_SteamAPI_SteamFriends_v017(%rip)");
static void *_fwd_SteamAPI_SteamFriends_v017 = NULL;

/* SteamAPI_SteamGameSearch_v001 */
__asm__(".globl SteamAPI_SteamGameSearch_v001");
__asm__("SteamAPI_SteamGameSearch_v001: jmp *_fwd_SteamAPI_SteamGameSearch_v001(%rip)");
static void *_fwd_SteamAPI_SteamGameSearch_v001 = NULL;

/* SteamAPI_SteamGameServerHTTP_v003 */
__asm__(".globl SteamAPI_SteamGameServerHTTP_v003");
__asm__("SteamAPI_SteamGameServerHTTP_v003: jmp *_fwd_SteamAPI_SteamGameServerHTTP_v003(%rip)");
static void *_fwd_SteamAPI_SteamGameServerHTTP_v003 = NULL;

/* SteamAPI_SteamGameServerInventory_v003 */
__asm__(".globl SteamAPI_SteamGameServerInventory_v003");
__asm__("SteamAPI_SteamGameServerInventory_v003: jmp *_fwd_SteamAPI_SteamGameServerInventory_v003(%rip)");
static void *_fwd_SteamAPI_SteamGameServerInventory_v003 = NULL;

/* SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 */
__asm__(".globl SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002");
__asm__("SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002: jmp *_fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002(%rip)");
static void *_fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 = NULL;

/* SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 */
__asm__(".globl SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012");
__asm__("SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012: jmp *_fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012(%rip)");
static void *_fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 = NULL;

/* SteamAPI_SteamGameServerNetworking_v006 */
__asm__(".globl SteamAPI_SteamGameServerNetworking_v006");
__asm__("SteamAPI_SteamGameServerNetworking_v006: jmp *_fwd_SteamAPI_SteamGameServerNetworking_v006(%rip)");
static void *_fwd_SteamAPI_SteamGameServerNetworking_v006 = NULL;

/* SteamAPI_SteamGameServerStats_v001 */
__asm__(".globl SteamAPI_SteamGameServerStats_v001");
__asm__("SteamAPI_SteamGameServerStats_v001: jmp *_fwd_SteamAPI_SteamGameServerStats_v001(%rip)");
static void *_fwd_SteamAPI_SteamGameServerStats_v001 = NULL;

/* SteamAPI_SteamGameServerUGC_v020 */
__asm__(".globl SteamAPI_SteamGameServerUGC_v020");
__asm__("SteamAPI_SteamGameServerUGC_v020: jmp *_fwd_SteamAPI_SteamGameServerUGC_v020(%rip)");
static void *_fwd_SteamAPI_SteamGameServerUGC_v020 = NULL;

/* SteamAPI_SteamGameServerUtils_v010 */
__asm__(".globl SteamAPI_SteamGameServerUtils_v010");
__asm__("SteamAPI_SteamGameServerUtils_v010: jmp *_fwd_SteamAPI_SteamGameServerUtils_v010(%rip)");
static void *_fwd_SteamAPI_SteamGameServerUtils_v010 = NULL;

/* SteamAPI_SteamGameServer_v015 */
__asm__(".globl SteamAPI_SteamGameServer_v015");
__asm__("SteamAPI_SteamGameServer_v015: jmp *_fwd_SteamAPI_SteamGameServer_v015(%rip)");
static void *_fwd_SteamAPI_SteamGameServer_v015 = NULL;

/* SteamAPI_SteamHTMLSurface_v005 */
__asm__(".globl SteamAPI_SteamHTMLSurface_v005");
__asm__("SteamAPI_SteamHTMLSurface_v005: jmp *_fwd_SteamAPI_SteamHTMLSurface_v005(%rip)");
static void *_fwd_SteamAPI_SteamHTMLSurface_v005 = NULL;

/* SteamAPI_SteamHTTP_v003 */
__asm__(".globl SteamAPI_SteamHTTP_v003");
__asm__("SteamAPI_SteamHTTP_v003: jmp *_fwd_SteamAPI_SteamHTTP_v003(%rip)");
static void *_fwd_SteamAPI_SteamHTTP_v003 = NULL;

/* SteamAPI_SteamInput_v006 */
__asm__(".globl SteamAPI_SteamInput_v006");
__asm__("SteamAPI_SteamInput_v006: jmp *_fwd_SteamAPI_SteamInput_v006(%rip)");
static void *_fwd_SteamAPI_SteamInput_v006 = NULL;

/* SteamAPI_SteamInventory_v003 */
__asm__(".globl SteamAPI_SteamInventory_v003");
__asm__("SteamAPI_SteamInventory_v003: jmp *_fwd_SteamAPI_SteamInventory_v003(%rip)");
static void *_fwd_SteamAPI_SteamInventory_v003 = NULL;

/* SteamAPI_SteamIPAddress_t_IsSet */
__asm__(".globl SteamAPI_SteamIPAddress_t_IsSet");
__asm__("SteamAPI_SteamIPAddress_t_IsSet: jmp *_fwd_SteamAPI_SteamIPAddress_t_IsSet(%rip)");
static void *_fwd_SteamAPI_SteamIPAddress_t_IsSet = NULL;

/* SteamAPI_SteamMatchmakingServers_v002 */
__asm__(".globl SteamAPI_SteamMatchmakingServers_v002");
__asm__("SteamAPI_SteamMatchmakingServers_v002: jmp *_fwd_SteamAPI_SteamMatchmakingServers_v002(%rip)");
static void *_fwd_SteamAPI_SteamMatchmakingServers_v002 = NULL;

/* SteamAPI_SteamMatchmaking_v009 */
__asm__(".globl SteamAPI_SteamMatchmaking_v009");
__asm__("SteamAPI_SteamMatchmaking_v009: jmp *_fwd_SteamAPI_SteamMatchmaking_v009(%rip)");
static void *_fwd_SteamAPI_SteamMatchmaking_v009 = NULL;

/* SteamAPI_SteamMusicRemote_v001 */
__asm__(".globl SteamAPI_SteamMusicRemote_v001");
__asm__("SteamAPI_SteamMusicRemote_v001: jmp *_fwd_SteamAPI_SteamMusicRemote_v001(%rip)");
static void *_fwd_SteamAPI_SteamMusicRemote_v001 = NULL;

/* SteamAPI_SteamMusic_v001 */
__asm__(".globl SteamAPI_SteamMusic_v001");
__asm__("SteamAPI_SteamMusic_v001: jmp *_fwd_SteamAPI_SteamMusic_v001(%rip)");
static void *_fwd_SteamAPI_SteamMusic_v001 = NULL;

/* SteamAPI_SteamNetworkingConfigValue_t_SetFloat */
__asm__(".globl SteamAPI_SteamNetworkingConfigValue_t_SetFloat");
__asm__("SteamAPI_SteamNetworkingConfigValue_t_SetFloat: jmp *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat = NULL;

/* SteamAPI_SteamNetworkingConfigValue_t_SetInt32 */
__asm__(".globl SteamAPI_SteamNetworkingConfigValue_t_SetInt32");
__asm__("SteamAPI_SteamNetworkingConfigValue_t_SetInt32: jmp *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32 = NULL;

/* SteamAPI_SteamNetworkingConfigValue_t_SetInt64 */
__asm__(".globl SteamAPI_SteamNetworkingConfigValue_t_SetInt64");
__asm__("SteamAPI_SteamNetworkingConfigValue_t_SetInt64: jmp *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64 = NULL;

/* SteamAPI_SteamNetworkingConfigValue_t_SetPtr */
__asm__(".globl SteamAPI_SteamNetworkingConfigValue_t_SetPtr");
__asm__("SteamAPI_SteamNetworkingConfigValue_t_SetPtr: jmp *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr = NULL;

/* SteamAPI_SteamNetworkingConfigValue_t_SetString */
__asm__(".globl SteamAPI_SteamNetworkingConfigValue_t_SetString");
__asm__("SteamAPI_SteamNetworkingConfigValue_t_SetString: jmp *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString = NULL;

/* SteamAPI_SteamNetworkingIdentity_Clear */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_Clear");
__asm__("SteamAPI_SteamNetworkingIdentity_Clear: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_Clear(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_Clear = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetFakeIPType */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetFakeIPType");
__asm__("SteamAPI_SteamNetworkingIdentity_GetFakeIPType: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetGenericBytes */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetGenericBytes");
__asm__("SteamAPI_SteamNetworkingIdentity_GetGenericBytes: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetGenericString */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetGenericString");
__asm__("SteamAPI_SteamNetworkingIdentity_GetGenericString: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetIPAddr */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetIPAddr");
__asm__("SteamAPI_SteamNetworkingIdentity_GetIPAddr: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetIPv4 */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetIPv4");
__asm__("SteamAPI_SteamNetworkingIdentity_GetIPv4: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4 = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetPSNID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetPSNID");
__asm__("SteamAPI_SteamNetworkingIdentity_GetPSNID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetSteamID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetSteamID");
__asm__("SteamAPI_SteamNetworkingIdentity_GetSteamID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetSteamID64 */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetSteamID64");
__asm__("SteamAPI_SteamNetworkingIdentity_GetSteamID64: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64 = NULL;

/* SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID");
__asm__("SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID = NULL;

/* SteamAPI_SteamNetworkingIdentity_IsEqualTo */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_IsEqualTo");
__asm__("SteamAPI_SteamNetworkingIdentity_IsEqualTo: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo = NULL;

/* SteamAPI_SteamNetworkingIdentity_IsFakeIP */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_IsFakeIP");
__asm__("SteamAPI_SteamNetworkingIdentity_IsFakeIP: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP = NULL;

/* SteamAPI_SteamNetworkingIdentity_IsInvalid */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_IsInvalid");
__asm__("SteamAPI_SteamNetworkingIdentity_IsInvalid: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid = NULL;

/* SteamAPI_SteamNetworkingIdentity_IsLocalHost */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_IsLocalHost");
__asm__("SteamAPI_SteamNetworkingIdentity_IsLocalHost: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost = NULL;

/* SteamAPI_SteamNetworkingIdentity_ParseString */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_ParseString");
__asm__("SteamAPI_SteamNetworkingIdentity_ParseString: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_ParseString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_ParseString = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetGenericBytes */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetGenericBytes");
__asm__("SteamAPI_SteamNetworkingIdentity_SetGenericBytes: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetGenericString */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetGenericString");
__asm__("SteamAPI_SteamNetworkingIdentity_SetGenericString: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetIPAddr */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetIPAddr");
__asm__("SteamAPI_SteamNetworkingIdentity_SetIPAddr: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetIPv4Addr */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetIPv4Addr");
__asm__("SteamAPI_SteamNetworkingIdentity_SetIPv4Addr: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetLocalHost */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetLocalHost");
__asm__("SteamAPI_SteamNetworkingIdentity_SetLocalHost: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetPSNID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetPSNID");
__asm__("SteamAPI_SteamNetworkingIdentity_SetPSNID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetSteamID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetSteamID");
__asm__("SteamAPI_SteamNetworkingIdentity_SetSteamID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetSteamID64 */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetSteamID64");
__asm__("SteamAPI_SteamNetworkingIdentity_SetSteamID64: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64 = NULL;

/* SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID");
__asm__("SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID = NULL;

/* SteamAPI_SteamNetworkingIdentity_ToString */
__asm__(".globl SteamAPI_SteamNetworkingIdentity_ToString");
__asm__("SteamAPI_SteamNetworkingIdentity_ToString: jmp *_fwd_SteamAPI_SteamNetworkingIdentity_ToString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIdentity_ToString = NULL;

/* SteamAPI_SteamNetworkingIPAddr_Clear */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_Clear");
__asm__("SteamAPI_SteamNetworkingIPAddr_Clear: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_Clear(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_Clear = NULL;

/* SteamAPI_SteamNetworkingIPAddr_GetFakeIPType */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_GetFakeIPType");
__asm__("SteamAPI_SteamNetworkingIPAddr_GetFakeIPType: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType = NULL;

/* SteamAPI_SteamNetworkingIPAddr_GetIPv4 */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_GetIPv4");
__asm__("SteamAPI_SteamNetworkingIPAddr_GetIPv4: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4 = NULL;

/* SteamAPI_SteamNetworkingIPAddr_IsEqualTo */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_IsEqualTo");
__asm__("SteamAPI_SteamNetworkingIPAddr_IsEqualTo: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo = NULL;

/* SteamAPI_SteamNetworkingIPAddr_IsFakeIP */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_IsFakeIP");
__asm__("SteamAPI_SteamNetworkingIPAddr_IsFakeIP: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP = NULL;

/* SteamAPI_SteamNetworkingIPAddr_IsIPv4 */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_IsIPv4");
__asm__("SteamAPI_SteamNetworkingIPAddr_IsIPv4: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4 = NULL;

/* SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros");
__asm__("SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros = NULL;

/* SteamAPI_SteamNetworkingIPAddr_IsLocalHost */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_IsLocalHost");
__asm__("SteamAPI_SteamNetworkingIPAddr_IsLocalHost: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost = NULL;

/* SteamAPI_SteamNetworkingIPAddr_ParseString */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_ParseString");
__asm__("SteamAPI_SteamNetworkingIPAddr_ParseString: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_ParseString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_ParseString = NULL;

/* SteamAPI_SteamNetworkingIPAddr_SetIPv4 */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_SetIPv4");
__asm__("SteamAPI_SteamNetworkingIPAddr_SetIPv4: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4 = NULL;

/* SteamAPI_SteamNetworkingIPAddr_SetIPv6 */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_SetIPv6");
__asm__("SteamAPI_SteamNetworkingIPAddr_SetIPv6: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6 = NULL;

/* SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost");
__asm__("SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost = NULL;

/* SteamAPI_SteamNetworkingIPAddr_ToString */
__asm__(".globl SteamAPI_SteamNetworkingIPAddr_ToString");
__asm__("SteamAPI_SteamNetworkingIPAddr_ToString: jmp *_fwd_SteamAPI_SteamNetworkingIPAddr_ToString(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_ToString = NULL;

/* SteamAPI_SteamNetworkingMessages_SteamAPI_v002 */
__asm__(".globl SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
__asm__("SteamAPI_SteamNetworkingMessages_SteamAPI_v002: jmp *_fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002 = NULL;

/* SteamAPI_SteamNetworkingMessage_t_Release */
__asm__(".globl SteamAPI_SteamNetworkingMessage_t_Release");
__asm__("SteamAPI_SteamNetworkingMessage_t_Release: jmp *_fwd_SteamAPI_SteamNetworkingMessage_t_Release(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingMessage_t_Release = NULL;

/* SteamAPI_SteamNetworkingSockets_SteamAPI_v012 */
__asm__(".globl SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
__asm__("SteamAPI_SteamNetworkingSockets_SteamAPI_v012: jmp *_fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012 = NULL;

/* SteamAPI_SteamNetworkingUtils_SteamAPI_v004 */
__asm__(".globl SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
__asm__("SteamAPI_SteamNetworkingUtils_SteamAPI_v004: jmp *_fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004(%rip)");
static void *_fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004 = NULL;

/* SteamAPI_SteamNetworking_v006 */
__asm__(".globl SteamAPI_SteamNetworking_v006");
__asm__("SteamAPI_SteamNetworking_v006: jmp *_fwd_SteamAPI_SteamNetworking_v006(%rip)");
static void *_fwd_SteamAPI_SteamNetworking_v006 = NULL;

/* SteamAPI_SteamParentalSettings_v001 */
__asm__(".globl SteamAPI_SteamParentalSettings_v001");
__asm__("SteamAPI_SteamParentalSettings_v001: jmp *_fwd_SteamAPI_SteamParentalSettings_v001(%rip)");
static void *_fwd_SteamAPI_SteamParentalSettings_v001 = NULL;

/* SteamAPI_SteamParties_v002 */
__asm__(".globl SteamAPI_SteamParties_v002");
__asm__("SteamAPI_SteamParties_v002: jmp *_fwd_SteamAPI_SteamParties_v002(%rip)");
static void *_fwd_SteamAPI_SteamParties_v002 = NULL;

/* SteamAPI_SteamRemotePlay_v002 */
__asm__(".globl SteamAPI_SteamRemotePlay_v002");
__asm__("SteamAPI_SteamRemotePlay_v002: jmp *_fwd_SteamAPI_SteamRemotePlay_v002(%rip)");
static void *_fwd_SteamAPI_SteamRemotePlay_v002 = NULL;

/* SteamAPI_SteamRemoteStorage_v016 */
__asm__(".globl SteamAPI_SteamRemoteStorage_v016");
__asm__("SteamAPI_SteamRemoteStorage_v016: jmp *_fwd_SteamAPI_SteamRemoteStorage_v016(%rip)");
static void *_fwd_SteamAPI_SteamRemoteStorage_v016 = NULL;

/* SteamAPI_SteamScreenshots_v003 */
__asm__(".globl SteamAPI_SteamScreenshots_v003");
__asm__("SteamAPI_SteamScreenshots_v003: jmp *_fwd_SteamAPI_SteamScreenshots_v003(%rip)");
static void *_fwd_SteamAPI_SteamScreenshots_v003 = NULL;

/* SteamAPI_SteamTimeline_v004 */
__asm__(".globl SteamAPI_SteamTimeline_v004");
__asm__("SteamAPI_SteamTimeline_v004: jmp *_fwd_SteamAPI_SteamTimeline_v004(%rip)");
static void *_fwd_SteamAPI_SteamTimeline_v004 = NULL;

/* SteamAPI_SteamUGC_v020 */
__asm__(".globl SteamAPI_SteamUGC_v020");
__asm__("SteamAPI_SteamUGC_v020: jmp *_fwd_SteamAPI_SteamUGC_v020(%rip)");
static void *_fwd_SteamAPI_SteamUGC_v020 = NULL;

/* SteamAPI_SteamUserStats_v013 */
__asm__(".globl SteamAPI_SteamUserStats_v013");
__asm__("SteamAPI_SteamUserStats_v013: jmp *_fwd_SteamAPI_SteamUserStats_v013(%rip)");
static void *_fwd_SteamAPI_SteamUserStats_v013 = NULL;

/* SteamAPI_SteamUser_v023 */
__asm__(".globl SteamAPI_SteamUser_v023");
__asm__("SteamAPI_SteamUser_v023: jmp *_fwd_SteamAPI_SteamUser_v023(%rip)");
static void *_fwd_SteamAPI_SteamUser_v023 = NULL;

/* SteamAPI_SteamUtils_v010 */
__asm__(".globl SteamAPI_SteamUtils_v010");
__asm__("SteamAPI_SteamUtils_v010: jmp *_fwd_SteamAPI_SteamUtils_v010(%rip)");
static void *_fwd_SteamAPI_SteamUtils_v010 = NULL;

/* SteamAPI_SteamVideo_v007 */
__asm__(".globl SteamAPI_SteamVideo_v007");
__asm__("SteamAPI_SteamVideo_v007: jmp *_fwd_SteamAPI_SteamVideo_v007(%rip)");
static void *_fwd_SteamAPI_SteamVideo_v007 = NULL;

/* SteamAPI_UnregisterCallback */
__asm__(".globl SteamAPI_UnregisterCallback");
__asm__("SteamAPI_UnregisterCallback: jmp *_fwd_SteamAPI_UnregisterCallback(%rip)");
static void *_fwd_SteamAPI_UnregisterCallback = NULL;

/* SteamAPI_UnregisterCallResult */
__asm__(".globl SteamAPI_UnregisterCallResult");
__asm__("SteamAPI_UnregisterCallResult: jmp *_fwd_SteamAPI_UnregisterCallResult(%rip)");
static void *_fwd_SteamAPI_UnregisterCallResult = NULL;

/* SteamAPI_UseBreakpadCrashHandler */
__asm__(".globl SteamAPI_UseBreakpadCrashHandler");
__asm__("SteamAPI_UseBreakpadCrashHandler: jmp *_fwd_SteamAPI_UseBreakpadCrashHandler(%rip)");
static void *_fwd_SteamAPI_UseBreakpadCrashHandler = NULL;

/* SteamAPI_WriteMiniDump */
__asm__(".globl SteamAPI_WriteMiniDump");
__asm__("SteamAPI_WriteMiniDump: jmp *_fwd_SteamAPI_WriteMiniDump(%rip)");
static void *_fwd_SteamAPI_WriteMiniDump = NULL;

/* SteamClient */
__asm__(".globl SteamClient");
__asm__("SteamClient: jmp *_fwd_SteamClient(%rip)");
static void *_fwd_SteamClient = NULL;

/* SteamGameServer_BSecure */
__asm__(".globl SteamGameServer_BSecure");
__asm__("SteamGameServer_BSecure: jmp *_fwd_SteamGameServer_BSecure(%rip)");
static void *_fwd_SteamGameServer_BSecure = NULL;

/* SteamGameServer_GetHSteamPipe */
__asm__(".globl SteamGameServer_GetHSteamPipe");
__asm__("SteamGameServer_GetHSteamPipe: jmp *_fwd_SteamGameServer_GetHSteamPipe(%rip)");
static void *_fwd_SteamGameServer_GetHSteamPipe = NULL;

/* SteamGameServer_GetHSteamUser */
__asm__(".globl SteamGameServer_GetHSteamUser");
__asm__("SteamGameServer_GetHSteamUser: jmp *_fwd_SteamGameServer_GetHSteamUser(%rip)");
static void *_fwd_SteamGameServer_GetHSteamUser = NULL;

/* SteamGameServer_GetIPCCallCount */
__asm__(".globl SteamGameServer_GetIPCCallCount");
__asm__("SteamGameServer_GetIPCCallCount: jmp *_fwd_SteamGameServer_GetIPCCallCount(%rip)");
static void *_fwd_SteamGameServer_GetIPCCallCount = NULL;

/* SteamGameServer_GetSteamID */
__asm__(".globl SteamGameServer_GetSteamID");
__asm__("SteamGameServer_GetSteamID: jmp *_fwd_SteamGameServer_GetSteamID(%rip)");
static void *_fwd_SteamGameServer_GetSteamID = NULL;

/* SteamGameServer_InitSafe */
__asm__(".globl SteamGameServer_InitSafe");
__asm__("SteamGameServer_InitSafe: jmp *_fwd_SteamGameServer_InitSafe(%rip)");
static void *_fwd_SteamGameServer_InitSafe = NULL;

/* SteamGameServer_RunCallbacks */
__asm__(".globl SteamGameServer_RunCallbacks");
__asm__("SteamGameServer_RunCallbacks: jmp *_fwd_SteamGameServer_RunCallbacks(%rip)");
static void *_fwd_SteamGameServer_RunCallbacks = NULL;

/* SteamGameServer_Shutdown */
__asm__(".globl SteamGameServer_Shutdown");
__asm__("SteamGameServer_Shutdown: jmp *_fwd_SteamGameServer_Shutdown(%rip)");
static void *_fwd_SteamGameServer_Shutdown = NULL;

/* SteamInternal_ContextInit */
__asm__(".globl SteamInternal_ContextInit");
__asm__("SteamInternal_ContextInit: jmp *_fwd_SteamInternal_ContextInit(%rip)");
static void *_fwd_SteamInternal_ContextInit = NULL;

/* SteamInternal_CreateInterface */
__asm__(".globl SteamInternal_CreateInterface");
__asm__("SteamInternal_CreateInterface: jmp *_fwd_SteamInternal_CreateInterface(%rip)");
static void *_fwd_SteamInternal_CreateInterface = NULL;

/* SteamInternal_FindOrCreateGameServerInterface */
__asm__(".globl SteamInternal_FindOrCreateGameServerInterface");
__asm__("SteamInternal_FindOrCreateGameServerInterface: jmp *_fwd_SteamInternal_FindOrCreateGameServerInterface(%rip)");
static void *_fwd_SteamInternal_FindOrCreateGameServerInterface = NULL;

/* SteamInternal_FindOrCreateUserInterface */
__asm__(".globl SteamInternal_FindOrCreateUserInterface");
__asm__("SteamInternal_FindOrCreateUserInterface: jmp *_fwd_SteamInternal_FindOrCreateUserInterface(%rip)");
static void *_fwd_SteamInternal_FindOrCreateUserInterface = NULL;

/* SteamInternal_GameServer_Init_V2 */
__asm__(".globl SteamInternal_GameServer_Init_V2");
__asm__("SteamInternal_GameServer_Init_V2: jmp *_fwd_SteamInternal_GameServer_Init_V2(%rip)");
static void *_fwd_SteamInternal_GameServer_Init_V2 = NULL;

/* SteamInternal_SteamAPI_Init */
__asm__(".globl SteamInternal_SteamAPI_Init");
__asm__("SteamInternal_SteamAPI_Init: jmp *_fwd_SteamInternal_SteamAPI_Init(%rip)");
static void *_fwd_SteamInternal_SteamAPI_Init = NULL;

/* SteamRealPath */
__asm__(".globl SteamRealPath");
__asm__("SteamRealPath: jmp *_fwd_SteamRealPath(%rip)");
static void *_fwd_SteamRealPath = NULL;

/* __wrap_access */
__asm__(".globl __wrap_access");
__asm__("__wrap_access: jmp *_fwd___wrap_access(%rip)");
static void *_fwd___wrap_access = NULL;

/* __wrap_chdir */
__asm__(".globl __wrap_chdir");
__asm__("__wrap_chdir: jmp *_fwd___wrap_chdir(%rip)");
static void *_fwd___wrap_chdir = NULL;

/* __wrap_chmod */
__asm__(".globl __wrap_chmod");
__asm__("__wrap_chmod: jmp *_fwd___wrap_chmod(%rip)");
static void *_fwd___wrap_chmod = NULL;

/* __wrap_chown */
__asm__(".globl __wrap_chown");
__asm__("__wrap_chown: jmp *_fwd___wrap_chown(%rip)");
static void *_fwd___wrap_chown = NULL;

/* __wrap_dlmopen */
__asm__(".globl __wrap_dlmopen");
__asm__("__wrap_dlmopen: jmp *_fwd___wrap_dlmopen(%rip)");
static void *_fwd___wrap_dlmopen = NULL;

/* __wrap_dlopen */
__asm__(".globl __wrap_dlopen");
__asm__("__wrap_dlopen: jmp *_fwd___wrap_dlopen(%rip)");
static void *_fwd___wrap_dlopen = NULL;

/* __wrap_fopen */
__asm__(".globl __wrap_fopen");
__asm__("__wrap_fopen: jmp *_fwd___wrap_fopen(%rip)");
static void *_fwd___wrap_fopen = NULL;

/* __wrap_fopen64 */
__asm__(".globl __wrap_fopen64");
__asm__("__wrap_fopen64: jmp *_fwd___wrap_fopen64(%rip)");
static void *_fwd___wrap_fopen64 = NULL;

/* __wrap_freopen */
__asm__(".globl __wrap_freopen");
__asm__("__wrap_freopen: jmp *_fwd___wrap_freopen(%rip)");
static void *_fwd___wrap_freopen = NULL;

/* __wrap_lchown */
__asm__(".globl __wrap_lchown");
__asm__("__wrap_lchown: jmp *_fwd___wrap_lchown(%rip)");
static void *_fwd___wrap_lchown = NULL;

/* __wrap_link */
__asm__(".globl __wrap_link");
__asm__("__wrap_link: jmp *_fwd___wrap_link(%rip)");
static void *_fwd___wrap_link = NULL;

/* __wrap_lstat */
__asm__(".globl __wrap_lstat");
__asm__("__wrap_lstat: jmp *_fwd___wrap_lstat(%rip)");
static void *_fwd___wrap_lstat = NULL;

/* __wrap_lstat64 */
__asm__(".globl __wrap_lstat64");
__asm__("__wrap_lstat64: jmp *_fwd___wrap_lstat64(%rip)");
static void *_fwd___wrap_lstat64 = NULL;

/* __wrap___lxstat */
__asm__(".globl __wrap___lxstat");
__asm__("__wrap___lxstat: jmp *_fwd___wrap___lxstat(%rip)");
static void *_fwd___wrap___lxstat = NULL;

/* __wrap___lxstat64 */
__asm__(".globl __wrap___lxstat64");
__asm__("__wrap___lxstat64: jmp *_fwd___wrap___lxstat64(%rip)");
static void *_fwd___wrap___lxstat64 = NULL;

/* __wrap_mkdir */
__asm__(".globl __wrap_mkdir");
__asm__("__wrap_mkdir: jmp *_fwd___wrap_mkdir(%rip)");
static void *_fwd___wrap_mkdir = NULL;

/* __wrap_mkfifo */
__asm__(".globl __wrap_mkfifo");
__asm__("__wrap_mkfifo: jmp *_fwd___wrap_mkfifo(%rip)");
static void *_fwd___wrap_mkfifo = NULL;

/* __wrap_mknod */
__asm__(".globl __wrap_mknod");
__asm__("__wrap_mknod: jmp *_fwd___wrap_mknod(%rip)");
static void *_fwd___wrap_mknod = NULL;

/* __wrap_mount */
__asm__(".globl __wrap_mount");
__asm__("__wrap_mount: jmp *_fwd___wrap_mount(%rip)");
static void *_fwd___wrap_mount = NULL;

/* __wrap_open */
__asm__(".globl __wrap_open");
__asm__("__wrap_open: jmp *_fwd___wrap_open(%rip)");
static void *_fwd___wrap_open = NULL;

/* __wrap_open64 */
__asm__(".globl __wrap_open64");
__asm__("__wrap_open64: jmp *_fwd___wrap_open64(%rip)");
static void *_fwd___wrap_open64 = NULL;

/* __wrap_opendir */
__asm__(".globl __wrap_opendir");
__asm__("__wrap_opendir: jmp *_fwd___wrap_opendir(%rip)");
static void *_fwd___wrap_opendir = NULL;

/* __wrap_rename */
__asm__(".globl __wrap_rename");
__asm__("__wrap_rename: jmp *_fwd___wrap_rename(%rip)");
static void *_fwd___wrap_rename = NULL;

/* __wrap_rmdir */
__asm__(".globl __wrap_rmdir");
__asm__("__wrap_rmdir: jmp *_fwd___wrap_rmdir(%rip)");
static void *_fwd___wrap_rmdir = NULL;

/* __wrap_scandir */
__asm__(".globl __wrap_scandir");
__asm__("__wrap_scandir: jmp *_fwd___wrap_scandir(%rip)");
static void *_fwd___wrap_scandir = NULL;

/* __wrap_scandir64 */
__asm__(".globl __wrap_scandir64");
__asm__("__wrap_scandir64: jmp *_fwd___wrap_scandir64(%rip)");
static void *_fwd___wrap_scandir64 = NULL;

/* __wrap_stat */
__asm__(".globl __wrap_stat");
__asm__("__wrap_stat: jmp *_fwd___wrap_stat(%rip)");
static void *_fwd___wrap_stat = NULL;

/* __wrap_stat64 */
__asm__(".globl __wrap_stat64");
__asm__("__wrap_stat64: jmp *_fwd___wrap_stat64(%rip)");
static void *_fwd___wrap_stat64 = NULL;

/* __wrap_statfs */
__asm__(".globl __wrap_statfs");
__asm__("__wrap_statfs: jmp *_fwd___wrap_statfs(%rip)");
static void *_fwd___wrap_statfs = NULL;

/* __wrap_statfs64 */
__asm__(".globl __wrap_statfs64");
__asm__("__wrap_statfs64: jmp *_fwd___wrap_statfs64(%rip)");
static void *_fwd___wrap_statfs64 = NULL;

/* __wrap_statvfs */
__asm__(".globl __wrap_statvfs");
__asm__("__wrap_statvfs: jmp *_fwd___wrap_statvfs(%rip)");
static void *_fwd___wrap_statvfs = NULL;

/* __wrap_statvfs64 */
__asm__(".globl __wrap_statvfs64");
__asm__("__wrap_statvfs64: jmp *_fwd___wrap_statvfs64(%rip)");
static void *_fwd___wrap_statvfs64 = NULL;

/* __wrap_symlink */
__asm__(".globl __wrap_symlink");
__asm__("__wrap_symlink: jmp *_fwd___wrap_symlink(%rip)");
static void *_fwd___wrap_symlink = NULL;

/* __wrap_unlink */
__asm__(".globl __wrap_unlink");
__asm__("__wrap_unlink: jmp *_fwd___wrap_unlink(%rip)");
static void *_fwd___wrap_unlink = NULL;

/* __wrap_utime */
__asm__(".globl __wrap_utime");
__asm__("__wrap_utime: jmp *_fwd___wrap_utime(%rip)");
static void *_fwd___wrap_utime = NULL;

/* __wrap_utimes */
__asm__(".globl __wrap_utimes");
__asm__("__wrap_utimes: jmp *_fwd___wrap_utimes(%rip)");
static void *_fwd___wrap_utimes = NULL;

/* __wrap___xstat */
__asm__(".globl __wrap___xstat");
__asm__("__wrap___xstat: jmp *_fwd___wrap___xstat(%rip)");
static void *_fwd___wrap___xstat = NULL;

/* __wrap___xstat64 */
__asm__(".globl __wrap___xstat64");
__asm__("__wrap___xstat64: jmp *_fwd___wrap___xstat64(%rip)");
static void *_fwd___wrap___xstat64 = NULL;


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

    _fwd_GetHSteamPipe = dlsym(g_real_lib, "GetHSteamPipe");
    _fwd_GetHSteamUser = dlsym(g_real_lib, "GetHSteamUser");
    _fwd_SteamAPI_gameserveritem_t_Construct = dlsym(g_real_lib, "SteamAPI_gameserveritem_t_Construct");
    _fwd_SteamAPI_gameserveritem_t_GetName = dlsym(g_real_lib, "SteamAPI_gameserveritem_t_GetName");
    _fwd_SteamAPI_gameserveritem_t_SetName = dlsym(g_real_lib, "SteamAPI_gameserveritem_t_SetName");
    _fwd_SteamAPI_GetHSteamPipe = dlsym(g_real_lib, "SteamAPI_GetHSteamPipe");
    _fwd_SteamAPI_GetHSteamUser = dlsym(g_real_lib, "SteamAPI_GetHSteamUser");
    _fwd_SteamAPI_GetSteamInstallPath = dlsym(g_real_lib, "SteamAPI_GetSteamInstallPath");
    _fwd_SteamAPI_InitAnonymousUser = dlsym(g_real_lib, "SteamAPI_InitAnonymousUser");
    _fwd_SteamAPI_InitFlat = dlsym(g_real_lib, "SteamAPI_InitFlat");
    _fwd_SteamAPI_InitSafe = dlsym(g_real_lib, "SteamAPI_InitSafe");
    _fwd_SteamAPI_IsSteamRunning = dlsym(g_real_lib, "SteamAPI_IsSteamRunning");
    _fwd_SteamAPI_ISteamApps_BIsCybercafe = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsCybercafe");
    _fwd_SteamAPI_ISteamApps_BIsLowViolence = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsLowViolence");
    _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing");
    _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend");
    _fwd_SteamAPI_ISteamApps_BIsTimedTrial = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsTimedTrial");
    _fwd_SteamAPI_ISteamApps_BIsVACBanned = dlsym(g_real_lib, "SteamAPI_ISteamApps_BIsVACBanned");
    _fwd_SteamAPI_ISteamApps_GetAppBuildId = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetAppBuildId");
    _fwd_SteamAPI_ISteamApps_GetAppInstallDir = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetAppInstallDir");
    _fwd_SteamAPI_ISteamApps_GetAppOwner = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetAppOwner");
    _fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetAvailableGameLanguages");
    _fwd_SteamAPI_ISteamApps_GetBetaInfo = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetBetaInfo");
    _fwd_SteamAPI_ISteamApps_GetCurrentBetaName = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetCurrentBetaName");
    _fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    _fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetDlcDownloadProgress");
    _fwd_SteamAPI_ISteamApps_GetFileDetails = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetFileDetails");
    _fwd_SteamAPI_ISteamApps_GetInstalledDepots = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetInstalledDepots");
    _fwd_SteamAPI_ISteamApps_GetLaunchCommandLine = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetLaunchCommandLine");
    _fwd_SteamAPI_ISteamApps_GetLaunchQueryParam = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetLaunchQueryParam");
    _fwd_SteamAPI_ISteamApps_GetNumBetas = dlsym(g_real_lib, "SteamAPI_ISteamApps_GetNumBetas");
    _fwd_SteamAPI_ISteamApps_InstallDLC = dlsym(g_real_lib, "SteamAPI_ISteamApps_InstallDLC");
    _fwd_SteamAPI_ISteamApps_MarkContentCorrupt = dlsym(g_real_lib, "SteamAPI_ISteamApps_MarkContentCorrupt");
    _fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys = dlsym(g_real_lib, "SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys");
    _fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey = dlsym(g_real_lib, "SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey");
    _fwd_SteamAPI_ISteamApps_SetActiveBeta = dlsym(g_real_lib, "SteamAPI_ISteamApps_SetActiveBeta");
    _fwd_SteamAPI_ISteamApps_SetDlcContext = dlsym(g_real_lib, "SteamAPI_ISteamApps_SetDlcContext");
    _fwd_SteamAPI_ISteamApps_UninstallDLC = dlsym(g_real_lib, "SteamAPI_ISteamApps_UninstallDLC");
    _fwd_SteamAPI_ISteamClient_BReleaseSteamPipe = dlsym(g_real_lib, "SteamAPI_ISteamClient_BReleaseSteamPipe");
    _fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed = dlsym(g_real_lib, "SteamAPI_ISteamClient_BShutdownIfAllPipesClosed");
    _fwd_SteamAPI_ISteamClient_ConnectToGlobalUser = dlsym(g_real_lib, "SteamAPI_ISteamClient_ConnectToGlobalUser");
    _fwd_SteamAPI_ISteamClient_CreateLocalUser = dlsym(g_real_lib, "SteamAPI_ISteamClient_CreateLocalUser");
    _fwd_SteamAPI_ISteamClient_CreateSteamPipe = dlsym(g_real_lib, "SteamAPI_ISteamClient_CreateSteamPipe");
    _fwd_SteamAPI_ISteamClient_GetIPCCallCount = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetIPCCallCount");
    _fwd_SteamAPI_ISteamClient_GetISteamApps = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamApps");
    _fwd_SteamAPI_ISteamClient_GetISteamController = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamController");
    _fwd_SteamAPI_ISteamClient_GetISteamFriends = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamFriends");
    _fwd_SteamAPI_ISteamClient_GetISteamGameSearch = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamGameSearch");
    _fwd_SteamAPI_ISteamClient_GetISteamGameServer = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamGameServer");
    _fwd_SteamAPI_ISteamClient_GetISteamGameServerStats = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamGameServerStats");
    _fwd_SteamAPI_ISteamClient_GetISteamGenericInterface = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    _fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamHTMLSurface");
    _fwd_SteamAPI_ISteamClient_GetISteamHTTP = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamHTTP");
    _fwd_SteamAPI_ISteamClient_GetISteamInput = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamInput");
    _fwd_SteamAPI_ISteamClient_GetISteamInventory = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamInventory");
    _fwd_SteamAPI_ISteamClient_GetISteamMatchmaking = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    _fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamMatchmakingServers");
    _fwd_SteamAPI_ISteamClient_GetISteamMusic = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamMusic");
    _fwd_SteamAPI_ISteamClient_GetISteamMusicRemote = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamMusicRemote");
    _fwd_SteamAPI_ISteamClient_GetISteamNetworking = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamNetworking");
    _fwd_SteamAPI_ISteamClient_GetISteamParentalSettings = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamParentalSettings");
    _fwd_SteamAPI_ISteamClient_GetISteamParties = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamParties");
    _fwd_SteamAPI_ISteamClient_GetISteamRemotePlay = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamRemotePlay");
    _fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamRemoteStorage");
    _fwd_SteamAPI_ISteamClient_GetISteamScreenshots = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamScreenshots");
    _fwd_SteamAPI_ISteamClient_GetISteamUGC = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamUGC");
    _fwd_SteamAPI_ISteamClient_GetISteamUser = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamUser");
    _fwd_SteamAPI_ISteamClient_GetISteamUserStats = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamUserStats");
    _fwd_SteamAPI_ISteamClient_GetISteamUtils = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamUtils");
    _fwd_SteamAPI_ISteamClient_GetISteamVideo = dlsym(g_real_lib, "SteamAPI_ISteamClient_GetISteamVideo");
    _fwd_SteamAPI_ISteamClient_ReleaseUser = dlsym(g_real_lib, "SteamAPI_ISteamClient_ReleaseUser");
    _fwd_SteamAPI_ISteamClient_SetLocalIPBinding = dlsym(g_real_lib, "SteamAPI_ISteamClient_SetLocalIPBinding");
    _fwd_SteamAPI_ISteamClient_SetWarningMessageHook = dlsym(g_real_lib, "SteamAPI_ISteamClient_SetWarningMessageHook");
    _fwd_SteamAPI_ISteamController_ActivateActionSet = dlsym(g_real_lib, "SteamAPI_ISteamController_ActivateActionSet");
    _fwd_SteamAPI_ISteamController_ActivateActionSetLayer = dlsym(g_real_lib, "SteamAPI_ISteamController_ActivateActionSetLayer");
    _fwd_SteamAPI_ISteamController_DeactivateActionSetLayer = dlsym(g_real_lib, "SteamAPI_ISteamController_DeactivateActionSetLayer");
    _fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers = dlsym(g_real_lib, "SteamAPI_ISteamController_DeactivateAllActionSetLayers");
    _fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_GetActionOriginFromXboxOrigin");
    _fwd_SteamAPI_ISteamController_GetActionSetHandle = dlsym(g_real_lib, "SteamAPI_ISteamController_GetActionSetHandle");
    _fwd_SteamAPI_ISteamController_GetActiveActionSetLayers = dlsym(g_real_lib, "SteamAPI_ISteamController_GetActiveActionSetLayers");
    _fwd_SteamAPI_ISteamController_GetAnalogActionData = dlsym(g_real_lib, "SteamAPI_ISteamController_GetAnalogActionData");
    _fwd_SteamAPI_ISteamController_GetAnalogActionHandle = dlsym(g_real_lib, "SteamAPI_ISteamController_GetAnalogActionHandle");
    _fwd_SteamAPI_ISteamController_GetAnalogActionOrigins = dlsym(g_real_lib, "SteamAPI_ISteamController_GetAnalogActionOrigins");
    _fwd_SteamAPI_ISteamController_GetConnectedControllers = dlsym(g_real_lib, "SteamAPI_ISteamController_GetConnectedControllers");
    _fwd_SteamAPI_ISteamController_GetControllerBindingRevision = dlsym(g_real_lib, "SteamAPI_ISteamController_GetControllerBindingRevision");
    _fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex = dlsym(g_real_lib, "SteamAPI_ISteamController_GetControllerForGamepadIndex");
    _fwd_SteamAPI_ISteamController_GetCurrentActionSet = dlsym(g_real_lib, "SteamAPI_ISteamController_GetCurrentActionSet");
    _fwd_SteamAPI_ISteamController_GetDigitalActionData = dlsym(g_real_lib, "SteamAPI_ISteamController_GetDigitalActionData");
    _fwd_SteamAPI_ISteamController_GetDigitalActionHandle = dlsym(g_real_lib, "SteamAPI_ISteamController_GetDigitalActionHandle");
    _fwd_SteamAPI_ISteamController_GetDigitalActionOrigins = dlsym(g_real_lib, "SteamAPI_ISteamController_GetDigitalActionOrigins");
    _fwd_SteamAPI_ISteamController_GetGamepadIndexForController = dlsym(g_real_lib, "SteamAPI_ISteamController_GetGamepadIndexForController");
    _fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_GetGlyphForActionOrigin");
    _fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_GetGlyphForXboxOrigin");
    _fwd_SteamAPI_ISteamController_GetInputTypeForHandle = dlsym(g_real_lib, "SteamAPI_ISteamController_GetInputTypeForHandle");
    _fwd_SteamAPI_ISteamController_GetMotionData = dlsym(g_real_lib, "SteamAPI_ISteamController_GetMotionData");
    _fwd_SteamAPI_ISteamController_GetStringForActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_GetStringForActionOrigin");
    _fwd_SteamAPI_ISteamController_GetStringForXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_GetStringForXboxOrigin");
    _fwd_SteamAPI_ISteamController_Init = dlsym(g_real_lib, "SteamAPI_ISteamController_Init");
    _fwd_SteamAPI_ISteamController_RunFrame = dlsym(g_real_lib, "SteamAPI_ISteamController_RunFrame");
    _fwd_SteamAPI_ISteamController_SetLEDColor = dlsym(g_real_lib, "SteamAPI_ISteamController_SetLEDColor");
    _fwd_SteamAPI_ISteamController_ShowBindingPanel = dlsym(g_real_lib, "SteamAPI_ISteamController_ShowBindingPanel");
    _fwd_SteamAPI_ISteamController_Shutdown = dlsym(g_real_lib, "SteamAPI_ISteamController_Shutdown");
    _fwd_SteamAPI_ISteamController_StopAnalogActionMomentum = dlsym(g_real_lib, "SteamAPI_ISteamController_StopAnalogActionMomentum");
    _fwd_SteamAPI_ISteamController_TranslateActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamController_TranslateActionOrigin");
    _fwd_SteamAPI_ISteamController_TriggerHapticPulse = dlsym(g_real_lib, "SteamAPI_ISteamController_TriggerHapticPulse");
    _fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse = dlsym(g_real_lib, "SteamAPI_ISteamController_TriggerRepeatedHapticPulse");
    _fwd_SteamAPI_ISteamController_TriggerVibration = dlsym(g_real_lib, "SteamAPI_ISteamController_TriggerVibration");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlay = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlay");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToStore");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage");
    _fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem = dlsym(g_real_lib, "SteamAPI_ISteamFriends_BHasEquippedProfileItem");
    _fwd_SteamAPI_ISteamFriends_ClearRichPresence = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ClearRichPresence");
    _fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam = dlsym(g_real_lib, "SteamAPI_ISteamFriends_CloseClanChatWindowInSteam");
    _fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts = dlsym(g_real_lib, "SteamAPI_ISteamFriends_DownloadClanActivityCounts");
    _fwd_SteamAPI_ISteamFriends_EnumerateFollowingList = dlsym(g_real_lib, "SteamAPI_ISteamFriends_EnumerateFollowingList");
    _fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetChatMemberByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanActivityCounts = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanActivityCounts");
    _fwd_SteamAPI_ISteamFriends_GetClanByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanChatMemberCount");
    _fwd_SteamAPI_ISteamFriends_GetClanChatMessage = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanChatMessage");
    _fwd_SteamAPI_ISteamFriends_GetClanCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanCount");
    _fwd_SteamAPI_ISteamFriends_GetClanName = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanName");
    _fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanOfficerByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanOfficerCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanOfficerCount");
    _fwd_SteamAPI_ISteamFriends_GetClanOwner = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanOwner");
    _fwd_SteamAPI_ISteamFriends_GetClanTag = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetClanTag");
    _fwd_SteamAPI_ISteamFriends_GetCoplayFriend = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetCoplayFriend");
    _fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetCoplayFriendCount");
    _fwd_SteamAPI_ISteamFriends_GetFollowerCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFollowerCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendCoplayGame");
    _fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendCoplayTime");
    _fwd_SteamAPI_ISteamFriends_GetFriendCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendCountFromSource");
    _fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendFromSourceByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    _fwd_SteamAPI_ISteamFriends_GetFriendMessage = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendMessage");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaName = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaNameHistory");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaState = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    _fwd_SteamAPI_ISteamFriends_GetFriendRelationship = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendRelationship");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresence = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresence");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupMembersCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupMembersList");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupName = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupName");
    _fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetFriendSteamLevel");
    _fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetLargeFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages");
    _fwd_SteamAPI_ISteamFriends_GetPersonaName = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetPersonaName");
    _fwd_SteamAPI_ISteamFriends_GetPersonaState = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetPersonaState");
    _fwd_SteamAPI_ISteamFriends_GetPlayerNickname = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetPlayerNickname");
    _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetProfileItemPropertyString");
    _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetProfileItemPropertyUint");
    _fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetSmallFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetUserRestrictions = dlsym(g_real_lib, "SteamAPI_ISteamFriends_GetUserRestrictions");
    _fwd_SteamAPI_ISteamFriends_HasFriend = dlsym(g_real_lib, "SteamAPI_ISteamFriends_HasFriend");
    _fwd_SteamAPI_ISteamFriends_InviteUserToGame = dlsym(g_real_lib, "SteamAPI_ISteamFriends_InviteUserToGame");
    _fwd_SteamAPI_ISteamFriends_IsClanChatAdmin = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsClanChatAdmin");
    _fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam");
    _fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsClanOfficialGameGroup");
    _fwd_SteamAPI_ISteamFriends_IsClanPublic = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsClanPublic");
    _fwd_SteamAPI_ISteamFriends_IsFollowing = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsFollowing");
    _fwd_SteamAPI_ISteamFriends_IsUserInSource = dlsym(g_real_lib, "SteamAPI_ISteamFriends_IsUserInSource");
    _fwd_SteamAPI_ISteamFriends_JoinClanChatRoom = dlsym(g_real_lib, "SteamAPI_ISteamFriends_JoinClanChatRoom");
    _fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom = dlsym(g_real_lib, "SteamAPI_ISteamFriends_LeaveClanChatRoom");
    _fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam = dlsym(g_real_lib, "SteamAPI_ISteamFriends_OpenClanChatWindowInSteam");
    _fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser = dlsym(g_real_lib, "SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser");
    _fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage = dlsym(g_real_lib, "SteamAPI_ISteamFriends_ReplyToFriendMessage");
    _fwd_SteamAPI_ISteamFriends_RequestClanOfficerList = dlsym(g_real_lib, "SteamAPI_ISteamFriends_RequestClanOfficerList");
    _fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems = dlsym(g_real_lib, "SteamAPI_ISteamFriends_RequestEquippedProfileItems");
    _fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence = dlsym(g_real_lib, "SteamAPI_ISteamFriends_RequestFriendRichPresence");
    _fwd_SteamAPI_ISteamFriends_RequestUserInformation = dlsym(g_real_lib, "SteamAPI_ISteamFriends_RequestUserInformation");
    _fwd_SteamAPI_ISteamFriends_SendClanChatMessage = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SendClanChatMessage");
    _fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SetInGameVoiceSpeaking");
    _fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SetListenForFriendsMessages");
    _fwd_SteamAPI_ISteamFriends_SetPersonaName = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SetPersonaName");
    _fwd_SteamAPI_ISteamFriends_SetPlayedWith = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SetPlayedWith");
    _fwd_SteamAPI_ISteamFriends_SetRichPresence = dlsym(g_real_lib, "SteamAPI_ISteamFriends_SetRichPresence");
    _fwd_SteamAPI_ISteamGameSearch_AcceptGame = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_AcceptGame");
    _fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_AddGameSearchParams");
    _fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame");
    _fwd_SteamAPI_ISteamGameSearch_DeclineGame = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_DeclineGame");
    _fwd_SteamAPI_ISteamGameSearch_EndGame = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_EndGame");
    _fwd_SteamAPI_ISteamGameSearch_EndGameSearch = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_EndGameSearch");
    _fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_HostConfirmGameStart");
    _fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_RequestPlayersForGame");
    _fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_RetrieveConnectionDetails");
    _fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_SearchForGameSolo");
    _fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_SearchForGameWithLobby");
    _fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_SetConnectionDetails");
    _fwd_SteamAPI_ISteamGameSearch_SetGameHostParams = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_SetGameHostParams");
    _fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult = dlsym(g_real_lib, "SteamAPI_ISteamGameSearch_SubmitPlayerResult");
    _fwd_SteamAPI_ISteamGameServer_AssociateWithClan = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_AssociateWithClan");
    _fwd_SteamAPI_ISteamGameServer_BeginAuthSession = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_BeginAuthSession");
    _fwd_SteamAPI_ISteamGameServer_BLoggedOn = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_BLoggedOn");
    _fwd_SteamAPI_ISteamGameServer_BSecure = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_BSecure");
    _fwd_SteamAPI_ISteamGameServer_BUpdateUserData = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_BUpdateUserData");
    _fwd_SteamAPI_ISteamGameServer_CancelAuthTicket = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_CancelAuthTicket");
    _fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_ClearAllKeyValues");
    _fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility");
    _fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection");
    _fwd_SteamAPI_ISteamGameServer_EndAuthSession = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_EndAuthSession");
    _fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetAuthSessionTicket");
    _fwd_SteamAPI_ISteamGameServer_GetGameplayStats = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetGameplayStats");
    _fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetNextOutgoingPacket");
    _fwd_SteamAPI_ISteamGameServer_GetPublicIP = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetPublicIP");
    _fwd_SteamAPI_ISteamGameServer_GetServerReputation = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetServerReputation");
    _fwd_SteamAPI_ISteamGameServer_GetSteamID = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_GetSteamID");
    _fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_HandleIncomingPacket");
    _fwd_SteamAPI_ISteamGameServer_LogOff = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_LogOff");
    _fwd_SteamAPI_ISteamGameServer_LogOn = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_LogOn");
    _fwd_SteamAPI_ISteamGameServer_LogOnAnonymous = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_LogOnAnonymous");
    _fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_RequestUserGroupStatus");
    _fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED");
    _fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED");
    _fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetAdvertiseServerActive");
    _fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetBotPlayerCount");
    _fwd_SteamAPI_ISteamGameServer_SetDedicatedServer = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetDedicatedServer");
    _fwd_SteamAPI_ISteamGameServer_SetGameData = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetGameData");
    _fwd_SteamAPI_ISteamGameServer_SetGameDescription = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetGameDescription");
    _fwd_SteamAPI_ISteamGameServer_SetGameTags = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetGameTags");
    _fwd_SteamAPI_ISteamGameServer_SetKeyValue = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetKeyValue");
    _fwd_SteamAPI_ISteamGameServer_SetMapName = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetMapName");
    _fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetMaxPlayerCount");
    _fwd_SteamAPI_ISteamGameServer_SetModDir = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetModDir");
    _fwd_SteamAPI_ISteamGameServer_SetPasswordProtected = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetPasswordProtected");
    _fwd_SteamAPI_ISteamGameServer_SetProduct = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetProduct");
    _fwd_SteamAPI_ISteamGameServer_SetRegion = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetRegion");
    _fwd_SteamAPI_ISteamGameServer_SetServerName = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetServerName");
    _fwd_SteamAPI_ISteamGameServer_SetSpectatorPort = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetSpectatorPort");
    _fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_SetSpectatorServerName");
    _fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_ClearUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_GetUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_GetUserStatFloat");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32 = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_GetUserStatInt32");
    _fwd_SteamAPI_ISteamGameServerStats_RequestUserStats = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_RequestUserStats");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_SetUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_SetUserStatFloat");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32 = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_SetUserStatInt32");
    _fwd_SteamAPI_ISteamGameServerStats_StoreUserStats = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_StoreUserStats");
    _fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat = dlsym(g_real_lib, "SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat");
    _fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_UserHasLicenseForApp");
    _fwd_SteamAPI_ISteamGameServer_WasRestartRequested = dlsym(g_real_lib, "SteamAPI_ISteamGameServer_WasRestartRequested");
    _fwd_SteamAPI_ISteamHTMLSurface_AddHeader = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_AddHeader");
    _fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_AllowStartRequest");
    _fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_CopyToClipboard");
    _fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_CreateBrowser");
    _fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_ExecuteJavascript");
    _fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse");
    _fwd_SteamAPI_ISteamHTMLSurface_Find = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_Find");
    _fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_GetLinkAtPosition");
    _fwd_SteamAPI_ISteamHTMLSurface_GoBack = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_GoBack");
    _fwd_SteamAPI_ISteamHTMLSurface_GoForward = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_GoForward");
    _fwd_SteamAPI_ISteamHTMLSurface_Init = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_Init");
    _fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_JSDialogResponse");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyChar = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_KeyChar");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyDown = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_KeyDown");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyUp = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_KeyUp");
    _fwd_SteamAPI_ISteamHTMLSurface_LoadURL = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_LoadURL");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_MouseDoubleClick");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseDown = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_MouseDown");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseMove = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_MouseMove");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseUp = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_MouseUp");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseWheel = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_MouseWheel");
    _fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_OpenDeveloperTools");
    _fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_PasteFromClipboard");
    _fwd_SteamAPI_ISteamHTMLSurface_Reload = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_Reload");
    _fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_RemoveBrowser");
    _fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetBackgroundMode");
    _fwd_SteamAPI_ISteamHTMLSurface_SetCookie = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetCookie");
    _fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor");
    _fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetHorizontalScroll");
    _fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetKeyFocus");
    _fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetPageScaleFactor");
    _fwd_SteamAPI_ISteamHTMLSurface_SetSize = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetSize");
    _fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_SetVerticalScroll");
    _fwd_SteamAPI_ISteamHTMLSurface_Shutdown = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_Shutdown");
    _fwd_SteamAPI_ISteamHTMLSurface_StopFind = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_StopFind");
    _fwd_SteamAPI_ISteamHTMLSurface_StopLoad = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_StopLoad");
    _fwd_SteamAPI_ISteamHTMLSurface_ViewSource = dlsym(g_real_lib, "SteamAPI_ISteamHTMLSurface_ViewSource");
    _fwd_SteamAPI_ISteamHTTP_CreateCookieContainer = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_CreateCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_DeferHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData");
    _fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_PrioritizeHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_ReleaseCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_SendHTTPRequest = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SendHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse");
    _fwd_SteamAPI_ISteamHTTP_SetCookie = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetCookie");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestContextValue");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo = dlsym(g_real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo");
    _fwd_SteamAPI_ISteamInput_ActivateActionSet = dlsym(g_real_lib, "SteamAPI_ISteamInput_ActivateActionSet");
    _fwd_SteamAPI_ISteamInput_ActivateActionSetLayer = dlsym(g_real_lib, "SteamAPI_ISteamInput_ActivateActionSetLayer");
    _fwd_SteamAPI_ISteamInput_BNewDataAvailable = dlsym(g_real_lib, "SteamAPI_ISteamInput_BNewDataAvailable");
    _fwd_SteamAPI_ISteamInput_BWaitForData = dlsym(g_real_lib, "SteamAPI_ISteamInput_BWaitForData");
    _fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer = dlsym(g_real_lib, "SteamAPI_ISteamInput_DeactivateActionSetLayer");
    _fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers = dlsym(g_real_lib, "SteamAPI_ISteamInput_DeactivateAllActionSetLayers");
    _fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks = dlsym(g_real_lib, "SteamAPI_ISteamInput_EnableActionEventCallbacks");
    _fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks = dlsym(g_real_lib, "SteamAPI_ISteamInput_EnableDeviceCallbacks");
    _fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin");
    _fwd_SteamAPI_ISteamInput_GetActionSetHandle = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetActionSetHandle");
    _fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetActiveActionSetLayers");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionData = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetAnalogActionData");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionHandle = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetAnalogActionHandle");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetAnalogActionOrigins");
    _fwd_SteamAPI_ISteamInput_GetConnectedControllers = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetConnectedControllers");
    _fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetControllerForGamepadIndex");
    _fwd_SteamAPI_ISteamInput_GetCurrentActionSet = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetCurrentActionSet");
    _fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetDeviceBindingRevision");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionData = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetDigitalActionData");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionHandle = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetDigitalActionHandle");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetDigitalActionOrigins");
    _fwd_SteamAPI_ISteamInput_GetGamepadIndexForController = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetGamepadIndexForController");
    _fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy");
    _fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetGlyphForXboxOrigin");
    _fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetInputTypeForHandle = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetInputTypeForHandle");
    _fwd_SteamAPI_ISteamInput_GetMotionData = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetMotionData");
    _fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetRemotePlaySessionID");
    _fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetSessionInputConfigurationSettings");
    _fwd_SteamAPI_ISteamInput_GetStringForActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetStringForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetStringForAnalogActionName");
    _fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetStringForDigitalActionName");
    _fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_GetStringForXboxOrigin");
    _fwd_SteamAPI_ISteamInput_Init = dlsym(g_real_lib, "SteamAPI_ISteamInput_Init");
    _fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse = dlsym(g_real_lib, "SteamAPI_ISteamInput_Legacy_TriggerHapticPulse");
    _fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse = dlsym(g_real_lib, "SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse");
    _fwd_SteamAPI_ISteamInput_RunFrame = dlsym(g_real_lib, "SteamAPI_ISteamInput_RunFrame");
    _fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect = dlsym(g_real_lib, "SteamAPI_ISteamInput_SetDualSenseTriggerEffect");
    _fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath = dlsym(g_real_lib, "SteamAPI_ISteamInput_SetInputActionManifestFilePath");
    _fwd_SteamAPI_ISteamInput_SetLEDColor = dlsym(g_real_lib, "SteamAPI_ISteamInput_SetLEDColor");
    _fwd_SteamAPI_ISteamInput_ShowBindingPanel = dlsym(g_real_lib, "SteamAPI_ISteamInput_ShowBindingPanel");
    _fwd_SteamAPI_ISteamInput_Shutdown = dlsym(g_real_lib, "SteamAPI_ISteamInput_Shutdown");
    _fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum = dlsym(g_real_lib, "SteamAPI_ISteamInput_StopAnalogActionMomentum");
    _fwd_SteamAPI_ISteamInput_TranslateActionOrigin = dlsym(g_real_lib, "SteamAPI_ISteamInput_TranslateActionOrigin");
    _fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent = dlsym(g_real_lib, "SteamAPI_ISteamInput_TriggerSimpleHapticEvent");
    _fwd_SteamAPI_ISteamInput_TriggerVibration = dlsym(g_real_lib, "SteamAPI_ISteamInput_TriggerVibration");
    _fwd_SteamAPI_ISteamInput_TriggerVibrationExtended = dlsym(g_real_lib, "SteamAPI_ISteamInput_TriggerVibrationExtended");
    _fwd_SteamAPI_ISteamInventory_AddPromoItem = dlsym(g_real_lib, "SteamAPI_ISteamInventory_AddPromoItem");
    _fwd_SteamAPI_ISteamInventory_AddPromoItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_AddPromoItems");
    _fwd_SteamAPI_ISteamInventory_CheckResultSteamID = dlsym(g_real_lib, "SteamAPI_ISteamInventory_CheckResultSteamID");
    _fwd_SteamAPI_ISteamInventory_ConsumeItem = dlsym(g_real_lib, "SteamAPI_ISteamInventory_ConsumeItem");
    _fwd_SteamAPI_ISteamInventory_DeserializeResult = dlsym(g_real_lib, "SteamAPI_ISteamInventory_DeserializeResult");
    _fwd_SteamAPI_ISteamInventory_DestroyResult = dlsym(g_real_lib, "SteamAPI_ISteamInventory_DestroyResult");
    _fwd_SteamAPI_ISteamInventory_ExchangeItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_ExchangeItems");
    _fwd_SteamAPI_ISteamInventory_GenerateItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GenerateItems");
    _fwd_SteamAPI_ISteamInventory_GetAllItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetAllItems");
    _fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs");
    _fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetItemDefinitionIDs");
    _fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetItemDefinitionProperty");
    _fwd_SteamAPI_ISteamInventory_GetItemPrice = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetItemPrice");
    _fwd_SteamAPI_ISteamInventory_GetItemsByID = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetItemsByID");
    _fwd_SteamAPI_ISteamInventory_GetItemsWithPrices = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetItemsWithPrices");
    _fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetNumItemsWithPrices");
    _fwd_SteamAPI_ISteamInventory_GetResultItemProperty = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetResultItemProperty");
    _fwd_SteamAPI_ISteamInventory_GetResultItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetResultItems");
    _fwd_SteamAPI_ISteamInventory_GetResultStatus = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetResultStatus");
    _fwd_SteamAPI_ISteamInventory_GetResultTimestamp = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GetResultTimestamp");
    _fwd_SteamAPI_ISteamInventory_GrantPromoItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_GrantPromoItems");
    _fwd_SteamAPI_ISteamInventory_InspectItem = dlsym(g_real_lib, "SteamAPI_ISteamInventory_InspectItem");
    _fwd_SteamAPI_ISteamInventory_LoadItemDefinitions = dlsym(g_real_lib, "SteamAPI_ISteamInventory_LoadItemDefinitions");
    _fwd_SteamAPI_ISteamInventory_RemoveProperty = dlsym(g_real_lib, "SteamAPI_ISteamInventory_RemoveProperty");
    _fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs = dlsym(g_real_lib, "SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs");
    _fwd_SteamAPI_ISteamInventory_RequestPrices = dlsym(g_real_lib, "SteamAPI_ISteamInventory_RequestPrices");
    _fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SendItemDropHeartbeat");
    _fwd_SteamAPI_ISteamInventory_SerializeResult = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SerializeResult");
    _fwd_SteamAPI_ISteamInventory_SetPropertyBool = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SetPropertyBool");
    _fwd_SteamAPI_ISteamInventory_SetPropertyFloat = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SetPropertyFloat");
    _fwd_SteamAPI_ISteamInventory_SetPropertyInt64 = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SetPropertyInt64");
    _fwd_SteamAPI_ISteamInventory_SetPropertyString = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SetPropertyString");
    _fwd_SteamAPI_ISteamInventory_StartPurchase = dlsym(g_real_lib, "SteamAPI_ISteamInventory_StartPurchase");
    _fwd_SteamAPI_ISteamInventory_StartUpdateProperties = dlsym(g_real_lib, "SteamAPI_ISteamInventory_StartUpdateProperties");
    _fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties = dlsym(g_real_lib, "SteamAPI_ISteamInventory_SubmitUpdateProperties");
    _fwd_SteamAPI_ISteamInventory_TradeItems = dlsym(g_real_lib, "SteamAPI_ISteamInventory_TradeItems");
    _fwd_SteamAPI_ISteamInventory_TransferItemQuantity = dlsym(g_real_lib, "SteamAPI_ISteamInventory_TransferItemQuantity");
    _fwd_SteamAPI_ISteamInventory_TriggerItemDrop = dlsym(g_real_lib, "SteamAPI_ISteamInventory_TriggerItemDrop");
    _fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    _fwd_SteamAPI_ISteamMatchmaking_CreateLobby = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_CreateLobby");
    _fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_DeleteLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetFavoriteGameCount");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyDataCount");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyGameServer");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    _fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    _fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    _fwd_SteamAPI_ISteamMatchmaking_JoinLobby = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_JoinLobby");
    _fwd_SteamAPI_ISteamMatchmaking_LeaveLobby = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingPingResponse_ServerResponded");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete");
    _fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_RemoveFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded");
    _fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded");
    _fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_CancelQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_CancelServerQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_GetServerCount");
    _fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_GetServerDetails");
    _fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_IsRefreshing");
    _fwd_SteamAPI_ISteamMatchmakingServers_PingServer = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_PingServer");
    _fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_PlayerDetails");
    _fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RefreshQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RefreshServer");
    _fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_ReleaseRequest");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestInternetServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestLANServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_ServerRules = dlsym(g_real_lib, "SteamAPI_ISteamMatchmakingServers_ServerRules");
    _fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLinkedLobby");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyGameServer");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyOwner");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyType = dlsym(g_real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    _fwd_SteamAPI_ISteamMusic_BIsEnabled = dlsym(g_real_lib, "SteamAPI_ISteamMusic_BIsEnabled");
    _fwd_SteamAPI_ISteamMusic_BIsPlaying = dlsym(g_real_lib, "SteamAPI_ISteamMusic_BIsPlaying");
    _fwd_SteamAPI_ISteamMusic_GetPlaybackStatus = dlsym(g_real_lib, "SteamAPI_ISteamMusic_GetPlaybackStatus");
    _fwd_SteamAPI_ISteamMusic_GetVolume = dlsym(g_real_lib, "SteamAPI_ISteamMusic_GetVolume");
    _fwd_SteamAPI_ISteamMusic_Pause = dlsym(g_real_lib, "SteamAPI_ISteamMusic_Pause");
    _fwd_SteamAPI_ISteamMusic_Play = dlsym(g_real_lib, "SteamAPI_ISteamMusic_Play");
    _fwd_SteamAPI_ISteamMusic_PlayNext = dlsym(g_real_lib, "SteamAPI_ISteamMusic_PlayNext");
    _fwd_SteamAPI_ISteamMusic_PlayPrevious = dlsym(g_real_lib, "SteamAPI_ISteamMusic_PlayPrevious");
    _fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_BActivationSuccess");
    _fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_EnableLooped = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnableLooped");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnablePlaylists");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnablePlayNext");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnablePlayPrevious");
    _fwd_SteamAPI_ISteamMusicRemote_EnableQueue = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnableQueue");
    _fwd_SteamAPI_ISteamMusicRemote_EnableShuffled = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_EnableShuffled");
    _fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_PlaylistDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_PlaylistWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_QueueDidChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_QueueDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_QueueWillChange = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_QueueWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_ResetPlaylistEntries");
    _fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_ResetQueueEntries");
    _fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetDisplayName = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetDisplayName");
    _fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetPlaylistEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64");
    _fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_SetQueueEntry");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateLooped = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateLooped");
    _fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateShuffled");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateVolume = dlsym(g_real_lib, "SteamAPI_ISteamMusicRemote_UpdateVolume");
    _fwd_SteamAPI_ISteamMusic_SetVolume = dlsym(g_real_lib, "SteamAPI_ISteamMusic_SetVolume");
    _fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    _fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    _fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser");
    _fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    _fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_CreateConnectionSocket");
    _fwd_SteamAPI_ISteamNetworking_CreateListenSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_CreateListenSocket");
    _fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_CreateP2PConnectionSocket");
    _fwd_SteamAPI_ISteamNetworking_DestroyListenSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_DestroyListenSocket");
    _fwd_SteamAPI_ISteamNetworking_DestroySocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_DestroySocket");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP");
    _fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_GetListenSocketInfo");
    _fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_GetMaxPacketSize");
    _fwd_SteamAPI_ISteamNetworking_GetP2PSessionState = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_GetP2PSessionState");
    _fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_GetSocketConnectionType");
    _fwd_SteamAPI_ISteamNetworking_GetSocketInfo = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_GetSocketInfo");
    _fwd_SteamAPI_ISteamNetworking_IsDataAvailable = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_IsDataAvailable");
    _fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_IsDataAvailableOnSocket");
    _fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    _fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
    _fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    _fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    _fwd_SteamAPI_ISteamNetworking_ReadP2PPacket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    _fwd_SteamAPI_ISteamNetworking_RetrieveData = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_RetrieveData");
    _fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_RetrieveDataFromSocket");
    _fwd_SteamAPI_ISteamNetworking_SendDataOnSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_SendDataOnSocket");
    _fwd_SteamAPI_ISteamNetworking_SendP2PPacket = dlsym(g_real_lib, "SteamAPI_ISteamNetworking_SendP2PPacket");
    _fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_CreateSocketPair");
    _fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer");
    _fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetCertificateRequest");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionName");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionUserData");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetIdentity");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_InitAuthentication");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_ResetIdentity");
    _fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    _fwd_SteamAPI_ISteamNetworkingSockets_SendMessages = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SendMessages");
    _fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SetCertificate");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionName");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionUserData");
    _fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_AllocateMessage");
    _fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate");
    _fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString");
    _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations");
    _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetConfigValue");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetPOPCount");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetPOPList");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    _fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    _fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_IsFakeIPv4");
    _fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues");
    _fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_ParsePingLocationString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString = dlsym(g_real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString");
    _fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsAppBlocked");
    _fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsAppInBlockList");
    _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsFeatureBlocked");
    _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList");
    _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled");
    _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked = dlsym(g_real_lib, "SteamAPI_ISteamParentalSettings_BIsParentalLockLocked");
    _fwd_SteamAPI_ISteamParties_CancelReservation = dlsym(g_real_lib, "SteamAPI_ISteamParties_CancelReservation");
    _fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots = dlsym(g_real_lib, "SteamAPI_ISteamParties_ChangeNumOpenSlots");
    _fwd_SteamAPI_ISteamParties_CreateBeacon = dlsym(g_real_lib, "SteamAPI_ISteamParties_CreateBeacon");
    _fwd_SteamAPI_ISteamParties_DestroyBeacon = dlsym(g_real_lib, "SteamAPI_ISteamParties_DestroyBeacon");
    _fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetAvailableBeaconLocations");
    _fwd_SteamAPI_ISteamParties_GetBeaconByIndex = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetBeaconByIndex");
    _fwd_SteamAPI_ISteamParties_GetBeaconDetails = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetBeaconDetails");
    _fwd_SteamAPI_ISteamParties_GetBeaconLocationData = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetBeaconLocationData");
    _fwd_SteamAPI_ISteamParties_GetNumActiveBeacons = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetNumActiveBeacons");
    _fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations = dlsym(g_real_lib, "SteamAPI_ISteamParties_GetNumAvailableBeaconLocations");
    _fwd_SteamAPI_ISteamParties_JoinParty = dlsym(g_real_lib, "SteamAPI_ISteamParties_JoinParty");
    _fwd_SteamAPI_ISteamParties_OnReservationCompleted = dlsym(g_real_lib, "SteamAPI_ISteamParties_OnReservationCompleted");
    _fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_BGetSessionClientResolution");
    _fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite");
    _fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_GetSessionClientName");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionCount = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_GetSessionCount");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionID = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_GetSessionID");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID = dlsym(g_real_lib, "SteamAPI_ISteamRemotePlay_GetSessionSteamID");
    _fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch");
    _fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate");
    _fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest");
    _fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_DeletePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EndFileWriteBatch");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_FileDelete = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileDelete");
    _fwd_SteamAPI_ISteamRemoteStorage_FileExists = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileExists");
    _fwd_SteamAPI_ISteamRemoteStorage_FileForget = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileForget");
    _fwd_SteamAPI_ISteamRemoteStorage_FilePersisted = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FilePersisted");
    _fwd_SteamAPI_ISteamRemoteStorage_FileRead = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileRead");
    _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileReadAsync");
    _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete");
    _fwd_SteamAPI_ISteamRemoteStorage_FileShare = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileShare");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWrite = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWrite");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteAsync");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamClose");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk");
    _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetCachedUGCCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileCount = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetFileCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileSize = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetFileSize");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetFileTimestamp");
    _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetLocalFileChange");
    _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetQuota = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetQuota");
    _fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetSyncPlatforms");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetUGCDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
    _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
    _fwd_SteamAPI_ISteamRemoteStorage_PublishVideo = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_PublishVideo");
    _fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_PublishWorkshopFile");
    _fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp");
    _fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_SetSyncPlatforms");
    _fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction");
    _fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_SubscribePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCDownload = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UGCDownload");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCRead = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UGCRead");
    _fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote = dlsym(g_real_lib, "SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote");
    _fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_AddScreenshotToLibrary");
    _fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary");
    _fwd_SteamAPI_ISteamScreenshots_HookScreenshots = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_HookScreenshots");
    _fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_IsScreenshotsHooked");
    _fwd_SteamAPI_ISteamScreenshots_SetLocation = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_SetLocation");
    _fwd_SteamAPI_ISteamScreenshots_TagPublishedFile = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_TagPublishedFile");
    _fwd_SteamAPI_ISteamScreenshots_TagUser = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_TagUser");
    _fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_TriggerScreenshot");
    _fwd_SteamAPI_ISteamScreenshots_WriteScreenshot = dlsym(g_real_lib, "SteamAPI_ISteamScreenshots_WriteScreenshot");
    _fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_AddGamePhaseTag");
    _fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_AddRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_ClearTimelineTooltip");
    _fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_DoesEventRecordingExist");
    _fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist");
    _fwd_SteamAPI_ISteamTimeline_EndGamePhase = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_EndGamePhase");
    _fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_EndRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_OpenOverlayToGamePhase");
    _fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_RemoveTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_SetGamePhaseAttribute");
    _fwd_SteamAPI_ISteamTimeline_SetGamePhaseID = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_SetGamePhaseID");
    _fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_SetTimelineGameMode");
    _fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_SetTimelineTooltip");
    _fwd_SteamAPI_ISteamTimeline_StartGamePhase = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_StartGamePhase");
    _fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_StartRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent = dlsym(g_real_lib, "SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent");
    _fwd_SteamAPI_ISteamUGC_AddAppDependency = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddAppDependency");
    _fwd_SteamAPI_ISteamUGC_AddContentDescriptor = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddContentDescriptor");
    _fwd_SteamAPI_ISteamUGC_AddDependency = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddDependency");
    _fwd_SteamAPI_ISteamUGC_AddExcludedTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddExcludedTag");
    _fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddItemKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_AddItemPreviewFile = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddItemPreviewFile");
    _fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddItemPreviewVideo");
    _fwd_SteamAPI_ISteamUGC_AddItemToFavorites = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddItemToFavorites");
    _fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddRequiredKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_AddRequiredTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddRequiredTag");
    _fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup = dlsym(g_real_lib, "SteamAPI_ISteamUGC_AddRequiredTagGroup");
    _fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer = dlsym(g_real_lib, "SteamAPI_ISteamUGC_BInitWorkshopForGameServer");
    _fwd_SteamAPI_ISteamUGC_CreateItem = dlsym(g_real_lib, "SteamAPI_ISteamUGC_CreateItem");
    _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor = dlsym(g_real_lib, "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor");
    _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage = dlsym(g_real_lib, "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage");
    _fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest = dlsym(g_real_lib, "SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest");
    _fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest = dlsym(g_real_lib, "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest");
    _fwd_SteamAPI_ISteamUGC_DeleteItem = dlsym(g_real_lib, "SteamAPI_ISteamUGC_DeleteItem");
    _fwd_SteamAPI_ISteamUGC_DownloadItem = dlsym(g_real_lib, "SteamAPI_ISteamUGC_DownloadItem");
    _fwd_SteamAPI_ISteamUGC_GetAppDependencies = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetAppDependencies");
    _fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetItemDownloadInfo");
    _fwd_SteamAPI_ISteamUGC_GetItemInstallInfo = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetItemInstallInfo");
    _fwd_SteamAPI_ISteamUGC_GetItemState = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetItemState");
    _fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetItemUpdateProgress");
    _fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetNumSubscribedItems");
    _fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetNumSupportedGameVersions");
    _fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCChildren");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCMetadata");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumTags");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCPreviewURL");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCResult = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCResult");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCStatistic");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName");
    _fwd_SteamAPI_ISteamUGC_GetSubscribedItems = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetSubscribedItems");
    _fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetSupportedGameVersionData");
    _fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences");
    _fwd_SteamAPI_ISteamUGC_GetUserItemVote = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetUserItemVote");
    _fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus = dlsym(g_real_lib, "SteamAPI_ISteamUGC_GetWorkshopEULAStatus");
    _fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest = dlsym(g_real_lib, "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest");
    _fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_RemoveAppDependency = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveAppDependency");
    _fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveContentDescriptor");
    _fwd_SteamAPI_ISteamUGC_RemoveDependency = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveDependency");
    _fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveItemFromFavorites");
    _fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveItemKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_RemoveItemPreview = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RemoveItemPreview");
    _fwd_SteamAPI_ISteamUGC_RequestUGCDetails = dlsym(g_real_lib, "SteamAPI_ISteamUGC_RequestUGCDetails");
    _fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SendQueryUGCRequest");
    _fwd_SteamAPI_ISteamUGC_SetAdminQuery = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetAdminQuery");
    _fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetAllowCachedResponse");
    _fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetAllowLegacyUpload");
    _fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetCloudFileNameFilter");
    _fwd_SteamAPI_ISteamUGC_SetItemContent = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemContent");
    _fwd_SteamAPI_ISteamUGC_SetItemDescription = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemDescription");
    _fwd_SteamAPI_ISteamUGC_SetItemMetadata = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemMetadata");
    _fwd_SteamAPI_ISteamUGC_SetItemPreview = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemPreview");
    _fwd_SteamAPI_ISteamUGC_SetItemTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemTags");
    _fwd_SteamAPI_ISteamUGC_SetItemTitle = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemTitle");
    _fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemUpdateLanguage");
    _fwd_SteamAPI_ISteamUGC_SetItemVisibility = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetItemVisibility");
    _fwd_SteamAPI_ISteamUGC_SetLanguage = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetLanguage");
    _fwd_SteamAPI_ISteamUGC_SetMatchAnyTag = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetMatchAnyTag");
    _fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetRankedByTrendDays");
    _fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetRequiredGameVersions");
    _fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnAdditionalPreviews");
    _fwd_SteamAPI_ISteamUGC_SetReturnChildren = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnChildren");
    _fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_SetReturnLongDescription = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnLongDescription");
    _fwd_SteamAPI_ISteamUGC_SetReturnMetadata = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnMetadata");
    _fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnOnlyIDs");
    _fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnPlaytimeStats");
    _fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetReturnTotalOnly");
    _fwd_SteamAPI_ISteamUGC_SetSearchText = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetSearchText");
    _fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetTimeCreatedDateRange");
    _fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetTimeUpdatedDateRange");
    _fwd_SteamAPI_ISteamUGC_SetUserItemVote = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SetUserItemVote");
    _fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA = dlsym(g_real_lib, "SteamAPI_ISteamUGC_ShowWorkshopEULA");
    _fwd_SteamAPI_ISteamUGC_StartItemUpdate = dlsym(g_real_lib, "SteamAPI_ISteamUGC_StartItemUpdate");
    _fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking = dlsym(g_real_lib, "SteamAPI_ISteamUGC_StartPlaytimeTracking");
    _fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking = dlsym(g_real_lib, "SteamAPI_ISteamUGC_StopPlaytimeTracking");
    _fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems = dlsym(g_real_lib, "SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems");
    _fwd_SteamAPI_ISteamUGC_SubmitItemUpdate = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SubmitItemUpdate");
    _fwd_SteamAPI_ISteamUGC_SubscribeItem = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SubscribeItem");
    _fwd_SteamAPI_ISteamUGC_SuspendDownloads = dlsym(g_real_lib, "SteamAPI_ISteamUGC_SuspendDownloads");
    _fwd_SteamAPI_ISteamUGC_UnsubscribeItem = dlsym(g_real_lib, "SteamAPI_ISteamUGC_UnsubscribeItem");
    _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile = dlsym(g_real_lib, "SteamAPI_ISteamUGC_UpdateItemPreviewFile");
    _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo = dlsym(g_real_lib, "SteamAPI_ISteamUGC_UpdateItemPreviewVideo");
    _fwd_SteamAPI_ISteamUser_AdvertiseGame = dlsym(g_real_lib, "SteamAPI_ISteamUser_AdvertiseGame");
    _fwd_SteamAPI_ISteamUser_BeginAuthSession = dlsym(g_real_lib, "SteamAPI_ISteamUser_BeginAuthSession");
    _fwd_SteamAPI_ISteamUser_BIsBehindNAT = dlsym(g_real_lib, "SteamAPI_ISteamUser_BIsBehindNAT");
    _fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying = dlsym(g_real_lib, "SteamAPI_ISteamUser_BIsPhoneIdentifying");
    _fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification = dlsym(g_real_lib, "SteamAPI_ISteamUser_BIsPhoneRequiringVerification");
    _fwd_SteamAPI_ISteamUser_BIsPhoneVerified = dlsym(g_real_lib, "SteamAPI_ISteamUser_BIsPhoneVerified");
    _fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled = dlsym(g_real_lib, "SteamAPI_ISteamUser_BIsTwoFactorEnabled");
    _fwd_SteamAPI_ISteamUser_BLoggedOn = dlsym(g_real_lib, "SteamAPI_ISteamUser_BLoggedOn");
    _fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState = dlsym(g_real_lib, "SteamAPI_ISteamUser_BSetDurationControlOnlineState");
    _fwd_SteamAPI_ISteamUser_CancelAuthTicket = dlsym(g_real_lib, "SteamAPI_ISteamUser_CancelAuthTicket");
    _fwd_SteamAPI_ISteamUser_DecompressVoice = dlsym(g_real_lib, "SteamAPI_ISteamUser_DecompressVoice");
    _fwd_SteamAPI_ISteamUser_EndAuthSession = dlsym(g_real_lib, "SteamAPI_ISteamUser_EndAuthSession");
    _fwd_SteamAPI_ISteamUser_GetAuthSessionTicket = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetAuthSessionTicket");
    _fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetAuthTicketForWebApi");
    _fwd_SteamAPI_ISteamUser_GetAvailableVoice = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetAvailableVoice");
    _fwd_SteamAPI_ISteamUser_GetDurationControl = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetDurationControl");
    _fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetEncryptedAppTicket");
    _fwd_SteamAPI_ISteamUser_GetGameBadgeLevel = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetGameBadgeLevel");
    _fwd_SteamAPI_ISteamUser_GetHSteamUser = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetHSteamUser");
    _fwd_SteamAPI_ISteamUser_GetMarketEligibility = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetMarketEligibility");
    _fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetPlayerSteamLevel");
    _fwd_SteamAPI_ISteamUser_GetSteamID = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetSteamID");
    _fwd_SteamAPI_ISteamUser_GetUserDataFolder = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetUserDataFolder");
    _fwd_SteamAPI_ISteamUser_GetVoice = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetVoice");
    _fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate = dlsym(g_real_lib, "SteamAPI_ISteamUser_GetVoiceOptimalSampleRate");
    _fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED = dlsym(g_real_lib, "SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED");
    _fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket = dlsym(g_real_lib, "SteamAPI_ISteamUser_RequestEncryptedAppTicket");
    _fwd_SteamAPI_ISteamUser_RequestStoreAuthURL = dlsym(g_real_lib, "SteamAPI_ISteamUser_RequestStoreAuthURL");
    _fwd_SteamAPI_ISteamUser_StartVoiceRecording = dlsym(g_real_lib, "SteamAPI_ISteamUser_StartVoiceRecording");
    _fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_AttachLeaderboardUGC");
    _fwd_SteamAPI_ISteamUserStats_ClearAchievement = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_ClearAchievement");
    _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries");
    _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers");
    _fwd_SteamAPI_ISteamUserStats_FindLeaderboard = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_FindLeaderboard");
    _fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
    _fwd_SteamAPI_ISteamUserStats_GetAchievement = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievement");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementAchievedPercent");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementIcon = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementIcon");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementName = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementName");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32");
    _fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatDouble");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatInt64");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardDisplayType");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardEntryCount");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardName = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardName");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardSortMethod");
    _fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo");
    _fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo");
    _fwd_SteamAPI_ISteamUserStats_GetNumAchievements = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetNumAchievements");
    _fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers");
    _fwd_SteamAPI_ISteamUserStats_GetStatFloat = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetStatFloat");
    _fwd_SteamAPI_ISteamUserStats_GetStatInt32 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetStatInt32");
    _fwd_SteamAPI_ISteamUserStats_GetUserAchievement = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetUserAchievement");
    _fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime");
    _fwd_SteamAPI_ISteamUserStats_GetUserStatFloat = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetUserStatFloat");
    _fwd_SteamAPI_ISteamUserStats_GetUserStatInt32 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_GetUserStatInt32");
    _fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_IndicateAchievementProgress");
    _fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages");
    _fwd_SteamAPI_ISteamUserStats_RequestGlobalStats = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_RequestGlobalStats");
    _fwd_SteamAPI_ISteamUserStats_RequestUserStats = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_RequestUserStats");
    _fwd_SteamAPI_ISteamUserStats_ResetAllStats = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_ResetAllStats");
    _fwd_SteamAPI_ISteamUserStats_SetAchievement = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_SetAchievement");
    _fwd_SteamAPI_ISteamUserStats_SetStatFloat = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_SetStatFloat");
    _fwd_SteamAPI_ISteamUserStats_SetStatInt32 = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_SetStatInt32");
    _fwd_SteamAPI_ISteamUserStats_StoreStats = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_StoreStats");
    _fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_UpdateAvgRateStat");
    _fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore = dlsym(g_real_lib, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    _fwd_SteamAPI_ISteamUser_StopVoiceRecording = dlsym(g_real_lib, "SteamAPI_ISteamUser_StopVoiceRecording");
    _fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED = dlsym(g_real_lib, "SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED");
    _fwd_SteamAPI_ISteamUser_TrackAppUsageEvent = dlsym(g_real_lib, "SteamAPI_ISteamUser_TrackAppUsageEvent");
    _fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent = dlsym(g_real_lib, "SteamAPI_ISteamUtils_BOverlayNeedsPresent");
    _fwd_SteamAPI_ISteamUtils_CheckFileSignature = dlsym(g_real_lib, "SteamAPI_ISteamUtils_CheckFileSignature");
    _fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = dlsym(g_real_lib, "SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput = dlsym(g_real_lib, "SteamAPI_ISteamUtils_DismissGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_FilterText = dlsym(g_real_lib, "SteamAPI_ISteamUtils_FilterText");
    _fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetAPICallFailureReason");
    _fwd_SteamAPI_ISteamUtils_GetAPICallResult = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetAPICallResult");
    _fwd_SteamAPI_ISteamUtils_GetAppID = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetAppID");
    _fwd_SteamAPI_ISteamUtils_GetConnectedUniverse = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetConnectedUniverse");
    _fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetCurrentBatteryPower");
    _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength");
    _fwd_SteamAPI_ISteamUtils_GetImageRGBA = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetImageRGBA");
    _fwd_SteamAPI_ISteamUtils_GetImageSize = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetImageSize");
    _fwd_SteamAPI_ISteamUtils_GetIPCCallCount = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetIPCCallCount");
    _fwd_SteamAPI_ISteamUtils_GetIPCountry = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetIPCountry");
    _fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetIPv6ConnectivityState");
    _fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetSecondsSinceAppActive");
    _fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetSecondsSinceComputerActive");
    _fwd_SteamAPI_ISteamUtils_GetServerRealTime = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetServerRealTime");
    _fwd_SteamAPI_ISteamUtils_GetSteamUILanguage = dlsym(g_real_lib, "SteamAPI_ISteamUtils_GetSteamUILanguage");
    _fwd_SteamAPI_ISteamUtils_InitFilterText = dlsym(g_real_lib, "SteamAPI_ISteamUtils_InitFilterText");
    _fwd_SteamAPI_ISteamUtils_IsAPICallCompleted = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    _fwd_SteamAPI_ISteamUtils_IsOverlayEnabled = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsOverlayEnabled");
    _fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsSteamChinaLauncher");
    _fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsSteamInBigPictureMode");
    _fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsSteamRunningInVR");
    _fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");
    _fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled = dlsym(g_real_lib, "SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled");
    _fwd_SteamAPI_ISteamUtils_SetGameLauncherMode = dlsym(g_real_lib, "SteamAPI_ISteamUtils_SetGameLauncherMode");
    _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset = dlsym(g_real_lib, "SteamAPI_ISteamUtils_SetOverlayNotificationInset");
    _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition = dlsym(g_real_lib, "SteamAPI_ISteamUtils_SetOverlayNotificationPosition");
    _fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled = dlsym(g_real_lib, "SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled");
    _fwd_SteamAPI_ISteamUtils_SetWarningMessageHook = dlsym(g_real_lib, "SteamAPI_ISteamUtils_SetWarningMessageHook");
    _fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = dlsym(g_real_lib, "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput = dlsym(g_real_lib, "SteamAPI_ISteamUtils_ShowGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_StartVRDashboard = dlsym(g_real_lib, "SteamAPI_ISteamUtils_StartVRDashboard");
    _fwd_SteamAPI_ISteamVideo_GetOPFSettings = dlsym(g_real_lib, "SteamAPI_ISteamVideo_GetOPFSettings");
    _fwd_SteamAPI_ISteamVideo_GetOPFStringForApp = dlsym(g_real_lib, "SteamAPI_ISteamVideo_GetOPFStringForApp");
    _fwd_SteamAPI_ISteamVideo_GetVideoURL = dlsym(g_real_lib, "SteamAPI_ISteamVideo_GetVideoURL");
    _fwd_SteamAPI_ISteamVideo_IsBroadcasting = dlsym(g_real_lib, "SteamAPI_ISteamVideo_IsBroadcasting");
    _fwd_SteamAPI_ManualDispatch_FreeLastCallback = dlsym(g_real_lib, "SteamAPI_ManualDispatch_FreeLastCallback");
    _fwd_SteamAPI_ManualDispatch_GetAPICallResult = dlsym(g_real_lib, "SteamAPI_ManualDispatch_GetAPICallResult");
    _fwd_SteamAPI_ManualDispatch_GetNextCallback = dlsym(g_real_lib, "SteamAPI_ManualDispatch_GetNextCallback");
    _fwd_SteamAPI_ManualDispatch_Init = dlsym(g_real_lib, "SteamAPI_ManualDispatch_Init");
    _fwd_SteamAPI_ManualDispatch_RunFrame = dlsym(g_real_lib, "SteamAPI_ManualDispatch_RunFrame");
    _fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct = dlsym(g_real_lib, "SteamAPI_MatchMakingKeyValuePair_t_Construct");
    _fwd_SteamAPI_RegisterCallback = dlsym(g_real_lib, "SteamAPI_RegisterCallback");
    _fwd_SteamAPI_RegisterCallResult = dlsym(g_real_lib, "SteamAPI_RegisterCallResult");
    _fwd_SteamAPI_ReleaseCurrentThreadMemory = dlsym(g_real_lib, "SteamAPI_ReleaseCurrentThreadMemory");
    _fwd_SteamAPI_RestartAppIfNecessary = dlsym(g_real_lib, "SteamAPI_RestartAppIfNecessary");
    _fwd_SteamAPI_RunCallbacks = dlsym(g_real_lib, "SteamAPI_RunCallbacks");
    _fwd_SteamAPI_servernetadr_t_Assign = dlsym(g_real_lib, "SteamAPI_servernetadr_t_Assign");
    _fwd_SteamAPI_servernetadr_t_Construct = dlsym(g_real_lib, "SteamAPI_servernetadr_t_Construct");
    _fwd_SteamAPI_servernetadr_t_GetConnectionAddressString = dlsym(g_real_lib, "SteamAPI_servernetadr_t_GetConnectionAddressString");
    _fwd_SteamAPI_servernetadr_t_GetConnectionPort = dlsym(g_real_lib, "SteamAPI_servernetadr_t_GetConnectionPort");
    _fwd_SteamAPI_servernetadr_t_GetIP = dlsym(g_real_lib, "SteamAPI_servernetadr_t_GetIP");
    _fwd_SteamAPI_servernetadr_t_GetQueryAddressString = dlsym(g_real_lib, "SteamAPI_servernetadr_t_GetQueryAddressString");
    _fwd_SteamAPI_servernetadr_t_GetQueryPort = dlsym(g_real_lib, "SteamAPI_servernetadr_t_GetQueryPort");
    _fwd_SteamAPI_servernetadr_t_Init = dlsym(g_real_lib, "SteamAPI_servernetadr_t_Init");
    _fwd_SteamAPI_servernetadr_t_IsLessThan = dlsym(g_real_lib, "SteamAPI_servernetadr_t_IsLessThan");
    _fwd_SteamAPI_servernetadr_t_SetConnectionPort = dlsym(g_real_lib, "SteamAPI_servernetadr_t_SetConnectionPort");
    _fwd_SteamAPI_servernetadr_t_SetIP = dlsym(g_real_lib, "SteamAPI_servernetadr_t_SetIP");
    _fwd_SteamAPI_servernetadr_t_SetQueryPort = dlsym(g_real_lib, "SteamAPI_servernetadr_t_SetQueryPort");
    _fwd_SteamAPI_SetBreakpadAppID = dlsym(g_real_lib, "SteamAPI_SetBreakpadAppID");
    _fwd_SteamAPI_SetMiniDumpComment = dlsym(g_real_lib, "SteamAPI_SetMiniDumpComment");
    _fwd_SteamAPI_SetTryCatchCallbacks = dlsym(g_real_lib, "SteamAPI_SetTryCatchCallbacks");
    _fwd_SteamAPI_Shutdown = dlsym(g_real_lib, "SteamAPI_Shutdown");
    _fwd_SteamAPI_SteamApps_v008 = dlsym(g_real_lib, "SteamAPI_SteamApps_v008");
    _fwd_SteamAPI_SteamController_v008 = dlsym(g_real_lib, "SteamAPI_SteamController_v008");
    _fwd_SteamAPI_SteamDatagramHostedAddress_Clear = dlsym(g_real_lib, "SteamAPI_SteamDatagramHostedAddress_Clear");
    _fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID = dlsym(g_real_lib, "SteamAPI_SteamDatagramHostedAddress_GetPopID");
    _fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress = dlsym(g_real_lib, "SteamAPI_SteamDatagramHostedAddress_SetDevAddress");
    _fwd_SteamAPI_SteamFriends_v017 = dlsym(g_real_lib, "SteamAPI_SteamFriends_v017");
    _fwd_SteamAPI_SteamGameSearch_v001 = dlsym(g_real_lib, "SteamAPI_SteamGameSearch_v001");
    _fwd_SteamAPI_SteamGameServerHTTP_v003 = dlsym(g_real_lib, "SteamAPI_SteamGameServerHTTP_v003");
    _fwd_SteamAPI_SteamGameServerInventory_v003 = dlsym(g_real_lib, "SteamAPI_SteamGameServerInventory_v003");
    _fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 = dlsym(g_real_lib, "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002");
    _fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 = dlsym(g_real_lib, "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012");
    _fwd_SteamAPI_SteamGameServerNetworking_v006 = dlsym(g_real_lib, "SteamAPI_SteamGameServerNetworking_v006");
    _fwd_SteamAPI_SteamGameServerStats_v001 = dlsym(g_real_lib, "SteamAPI_SteamGameServerStats_v001");
    _fwd_SteamAPI_SteamGameServerUGC_v020 = dlsym(g_real_lib, "SteamAPI_SteamGameServerUGC_v020");
    _fwd_SteamAPI_SteamGameServerUtils_v010 = dlsym(g_real_lib, "SteamAPI_SteamGameServerUtils_v010");
    _fwd_SteamAPI_SteamGameServer_v015 = dlsym(g_real_lib, "SteamAPI_SteamGameServer_v015");
    _fwd_SteamAPI_SteamHTMLSurface_v005 = dlsym(g_real_lib, "SteamAPI_SteamHTMLSurface_v005");
    _fwd_SteamAPI_SteamHTTP_v003 = dlsym(g_real_lib, "SteamAPI_SteamHTTP_v003");
    _fwd_SteamAPI_SteamInput_v006 = dlsym(g_real_lib, "SteamAPI_SteamInput_v006");
    _fwd_SteamAPI_SteamInventory_v003 = dlsym(g_real_lib, "SteamAPI_SteamInventory_v003");
    _fwd_SteamAPI_SteamIPAddress_t_IsSet = dlsym(g_real_lib, "SteamAPI_SteamIPAddress_t_IsSet");
    _fwd_SteamAPI_SteamMatchmakingServers_v002 = dlsym(g_real_lib, "SteamAPI_SteamMatchmakingServers_v002");
    _fwd_SteamAPI_SteamMatchmaking_v009 = dlsym(g_real_lib, "SteamAPI_SteamMatchmaking_v009");
    _fwd_SteamAPI_SteamMusicRemote_v001 = dlsym(g_real_lib, "SteamAPI_SteamMusicRemote_v001");
    _fwd_SteamAPI_SteamMusic_v001 = dlsym(g_real_lib, "SteamAPI_SteamMusic_v001");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat = dlsym(g_real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetFloat");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetInt32");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetInt64");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr = dlsym(g_real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetPtr");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetString");
    _fwd_SteamAPI_SteamNetworkingIdentity_Clear = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_Clear");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetFakeIPType");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetGenericBytes");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetGenericString");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetIPAddr");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetIPv4");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetPSNID");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetSteamID");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_IsEqualTo");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_IsFakeIP");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_IsInvalid");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_IsLocalHost");
    _fwd_SteamAPI_SteamNetworkingIdentity_ParseString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_ParseString");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetGenericBytes");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetGenericString");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetIPAddr");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetIPv4Addr");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetLocalHost");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetPSNID");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetSteamID");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID");
    _fwd_SteamAPI_SteamNetworkingIdentity_ToString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIdentity_ToString");
    _fwd_SteamAPI_SteamNetworkingIPAddr_Clear = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_Clear");
    _fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_GetFakeIPType");
    _fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_GetIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_IsEqualTo");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_IsFakeIP");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_IsIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_IsLocalHost");
    _fwd_SteamAPI_SteamNetworkingIPAddr_ParseString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_ParseString");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv6");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost");
    _fwd_SteamAPI_SteamNetworkingIPAddr_ToString = dlsym(g_real_lib, "SteamAPI_SteamNetworkingIPAddr_ToString");
    _fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    _fwd_SteamAPI_SteamNetworkingMessage_t_Release = dlsym(g_real_lib, "SteamAPI_SteamNetworkingMessage_t_Release");
    _fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    _fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004 = dlsym(g_real_lib, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    _fwd_SteamAPI_SteamNetworking_v006 = dlsym(g_real_lib, "SteamAPI_SteamNetworking_v006");
    _fwd_SteamAPI_SteamParentalSettings_v001 = dlsym(g_real_lib, "SteamAPI_SteamParentalSettings_v001");
    _fwd_SteamAPI_SteamParties_v002 = dlsym(g_real_lib, "SteamAPI_SteamParties_v002");
    _fwd_SteamAPI_SteamRemotePlay_v002 = dlsym(g_real_lib, "SteamAPI_SteamRemotePlay_v002");
    _fwd_SteamAPI_SteamRemoteStorage_v016 = dlsym(g_real_lib, "SteamAPI_SteamRemoteStorage_v016");
    _fwd_SteamAPI_SteamScreenshots_v003 = dlsym(g_real_lib, "SteamAPI_SteamScreenshots_v003");
    _fwd_SteamAPI_SteamTimeline_v004 = dlsym(g_real_lib, "SteamAPI_SteamTimeline_v004");
    _fwd_SteamAPI_SteamUGC_v020 = dlsym(g_real_lib, "SteamAPI_SteamUGC_v020");
    _fwd_SteamAPI_SteamUserStats_v013 = dlsym(g_real_lib, "SteamAPI_SteamUserStats_v013");
    _fwd_SteamAPI_SteamUser_v023 = dlsym(g_real_lib, "SteamAPI_SteamUser_v023");
    _fwd_SteamAPI_SteamUtils_v010 = dlsym(g_real_lib, "SteamAPI_SteamUtils_v010");
    _fwd_SteamAPI_SteamVideo_v007 = dlsym(g_real_lib, "SteamAPI_SteamVideo_v007");
    _fwd_SteamAPI_UnregisterCallback = dlsym(g_real_lib, "SteamAPI_UnregisterCallback");
    _fwd_SteamAPI_UnregisterCallResult = dlsym(g_real_lib, "SteamAPI_UnregisterCallResult");
    _fwd_SteamAPI_UseBreakpadCrashHandler = dlsym(g_real_lib, "SteamAPI_UseBreakpadCrashHandler");
    _fwd_SteamAPI_WriteMiniDump = dlsym(g_real_lib, "SteamAPI_WriteMiniDump");
    _fwd_SteamClient = dlsym(g_real_lib, "SteamClient");
    _fwd_SteamGameServer_BSecure = dlsym(g_real_lib, "SteamGameServer_BSecure");
    _fwd_SteamGameServer_GetHSteamPipe = dlsym(g_real_lib, "SteamGameServer_GetHSteamPipe");
    _fwd_SteamGameServer_GetHSteamUser = dlsym(g_real_lib, "SteamGameServer_GetHSteamUser");
    _fwd_SteamGameServer_GetIPCCallCount = dlsym(g_real_lib, "SteamGameServer_GetIPCCallCount");
    _fwd_SteamGameServer_GetSteamID = dlsym(g_real_lib, "SteamGameServer_GetSteamID");
    _fwd_SteamGameServer_InitSafe = dlsym(g_real_lib, "SteamGameServer_InitSafe");
    _fwd_SteamGameServer_RunCallbacks = dlsym(g_real_lib, "SteamGameServer_RunCallbacks");
    _fwd_SteamGameServer_Shutdown = dlsym(g_real_lib, "SteamGameServer_Shutdown");
    _fwd_SteamInternal_ContextInit = dlsym(g_real_lib, "SteamInternal_ContextInit");
    _fwd_SteamInternal_CreateInterface = dlsym(g_real_lib, "SteamInternal_CreateInterface");
    _fwd_SteamInternal_FindOrCreateGameServerInterface = dlsym(g_real_lib, "SteamInternal_FindOrCreateGameServerInterface");
    _fwd_SteamInternal_FindOrCreateUserInterface = dlsym(g_real_lib, "SteamInternal_FindOrCreateUserInterface");
    _fwd_SteamInternal_GameServer_Init_V2 = dlsym(g_real_lib, "SteamInternal_GameServer_Init_V2");
    _fwd_SteamInternal_SteamAPI_Init = dlsym(g_real_lib, "SteamInternal_SteamAPI_Init");
    _fwd_SteamRealPath = dlsym(g_real_lib, "SteamRealPath");
    _fwd___wrap_access = dlsym(g_real_lib, "__wrap_access");
    _fwd___wrap_chdir = dlsym(g_real_lib, "__wrap_chdir");
    _fwd___wrap_chmod = dlsym(g_real_lib, "__wrap_chmod");
    _fwd___wrap_chown = dlsym(g_real_lib, "__wrap_chown");
    _fwd___wrap_dlmopen = dlsym(g_real_lib, "__wrap_dlmopen");
    _fwd___wrap_dlopen = dlsym(g_real_lib, "__wrap_dlopen");
    _fwd___wrap_fopen = dlsym(g_real_lib, "__wrap_fopen");
    _fwd___wrap_fopen64 = dlsym(g_real_lib, "__wrap_fopen64");
    _fwd___wrap_freopen = dlsym(g_real_lib, "__wrap_freopen");
    _fwd___wrap_lchown = dlsym(g_real_lib, "__wrap_lchown");
    _fwd___wrap_link = dlsym(g_real_lib, "__wrap_link");
    _fwd___wrap_lstat = dlsym(g_real_lib, "__wrap_lstat");
    _fwd___wrap_lstat64 = dlsym(g_real_lib, "__wrap_lstat64");
    _fwd___wrap___lxstat = dlsym(g_real_lib, "__wrap___lxstat");
    _fwd___wrap___lxstat64 = dlsym(g_real_lib, "__wrap___lxstat64");
    _fwd___wrap_mkdir = dlsym(g_real_lib, "__wrap_mkdir");
    _fwd___wrap_mkfifo = dlsym(g_real_lib, "__wrap_mkfifo");
    _fwd___wrap_mknod = dlsym(g_real_lib, "__wrap_mknod");
    _fwd___wrap_mount = dlsym(g_real_lib, "__wrap_mount");
    _fwd___wrap_open = dlsym(g_real_lib, "__wrap_open");
    _fwd___wrap_open64 = dlsym(g_real_lib, "__wrap_open64");
    _fwd___wrap_opendir = dlsym(g_real_lib, "__wrap_opendir");
    _fwd___wrap_rename = dlsym(g_real_lib, "__wrap_rename");
    _fwd___wrap_rmdir = dlsym(g_real_lib, "__wrap_rmdir");
    _fwd___wrap_scandir = dlsym(g_real_lib, "__wrap_scandir");
    _fwd___wrap_scandir64 = dlsym(g_real_lib, "__wrap_scandir64");
    _fwd___wrap_stat = dlsym(g_real_lib, "__wrap_stat");
    _fwd___wrap_stat64 = dlsym(g_real_lib, "__wrap_stat64");
    _fwd___wrap_statfs = dlsym(g_real_lib, "__wrap_statfs");
    _fwd___wrap_statfs64 = dlsym(g_real_lib, "__wrap_statfs64");
    _fwd___wrap_statvfs = dlsym(g_real_lib, "__wrap_statvfs");
    _fwd___wrap_statvfs64 = dlsym(g_real_lib, "__wrap_statvfs64");
    _fwd___wrap_symlink = dlsym(g_real_lib, "__wrap_symlink");
    _fwd___wrap_unlink = dlsym(g_real_lib, "__wrap_unlink");
    _fwd___wrap_utime = dlsym(g_real_lib, "__wrap_utime");
    _fwd___wrap_utimes = dlsym(g_real_lib, "__wrap_utimes");
    _fwd___wrap___xstat = dlsym(g_real_lib, "__wrap___xstat");
    _fwd___wrap___xstat64 = dlsym(g_real_lib, "__wrap___xstat64");

    LOG("All symbols resolved. CreamySteamy proxy active with %d DLCs.", g_dlc_count);
}

