/*
 * CreamySteamyLinux - Steam API Proxy for DLC Unlocking
 * 
 * This replaces libsteam_api.so. It loads the real library (steam_api_o.so)
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
    
    /* Find the real library - renamed to steam_api_o.so (no 'lib' prefix to prevent Unity auto-preload) */
    Dl_info info;
    char real_path[4096] = {0};
    
    if (dladdr((void*)ensure_real_lib, &info) && info.dli_fname) {
        strncpy(real_path, info.dli_fname, sizeof(real_path)-1);
        char *slash = strrchr(real_path, '/');
        if (slash) {
            strcpy(slash+1, "steam_api_o.so");
        }
    }
    
    if (real_path[0]) {
        g_real_lib = dlopen(real_path, RTLD_NOW | RTLD_LOCAL);
    }
    
    if (!g_real_lib) {
        /* Fallback: try relative */
        g_real_lib = dlopen("./steam_api_o.so", RTLD_NOW | RTLD_LOCAL);
    }
    
    if (!g_real_lib) {
        LOG("FATAL: Cannot load real steam_api_o.so: %s", dlerror());
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


/* ========================================================================= */
/* Auto-generated forwarding stubs (PIC-safe)                                */
/* ========================================================================= */

/* Forward pointers */
static void *_fwd_GetHSteamPipe = NULL;
static void *_fwd_GetHSteamUser = NULL;
static void *_fwd_SteamAPI_gameserveritem_t_Construct = NULL;
static void *_fwd_SteamAPI_gameserveritem_t_GetName = NULL;
static void *_fwd_SteamAPI_gameserveritem_t_SetName = NULL;
static void *_fwd_SteamAPI_GetHSteamPipe = NULL;
static void *_fwd_SteamAPI_GetHSteamUser = NULL;
static void *_fwd_SteamAPI_GetSteamInstallPath = NULL;
static void *_fwd_SteamAPI_InitAnonymousUser = NULL;
static void *_fwd_SteamAPI_InitFlat = NULL;
static void *_fwd_SteamAPI_InitSafe = NULL;
static void *_fwd_SteamAPI_IsSteamRunning = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsCybercafe = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsLowViolence = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsTimedTrial = NULL;
static void *_fwd_SteamAPI_ISteamApps_BIsVACBanned = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetAppBuildId = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetAppInstallDir = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetAppOwner = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetBetaInfo = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetCurrentBetaName = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetFileDetails = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetInstalledDepots = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetLaunchCommandLine = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetLaunchQueryParam = NULL;
static void *_fwd_SteamAPI_ISteamApps_GetNumBetas = NULL;
static void *_fwd_SteamAPI_ISteamApps_InstallDLC = NULL;
static void *_fwd_SteamAPI_ISteamApps_MarkContentCorrupt = NULL;
static void *_fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys = NULL;
static void *_fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey = NULL;
static void *_fwd_SteamAPI_ISteamApps_SetActiveBeta = NULL;
static void *_fwd_SteamAPI_ISteamApps_SetDlcContext = NULL;
static void *_fwd_SteamAPI_ISteamApps_UninstallDLC = NULL;
static void *_fwd_SteamAPI_ISteamClient_BReleaseSteamPipe = NULL;
static void *_fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed = NULL;
static void *_fwd_SteamAPI_ISteamClient_ConnectToGlobalUser = NULL;
static void *_fwd_SteamAPI_ISteamClient_CreateLocalUser = NULL;
static void *_fwd_SteamAPI_ISteamClient_CreateSteamPipe = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetIPCCallCount = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamApps = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamController = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamFriends = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameSearch = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameServer = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamGameServerStats = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamGenericInterface = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamHTTP = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamInput = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamInventory = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamMatchmaking = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamMusic = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamMusicRemote = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamNetworking = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamParentalSettings = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamParties = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamRemotePlay = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamScreenshots = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamUGC = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamUser = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamUserStats = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamUtils = NULL;
static void *_fwd_SteamAPI_ISteamClient_GetISteamVideo = NULL;
static void *_fwd_SteamAPI_ISteamClient_ReleaseUser = NULL;
static void *_fwd_SteamAPI_ISteamClient_SetLocalIPBinding = NULL;
static void *_fwd_SteamAPI_ISteamClient_SetWarningMessageHook = NULL;
static void *_fwd_SteamAPI_ISteamController_ActivateActionSet = NULL;
static void *_fwd_SteamAPI_ISteamController_ActivateActionSetLayer = NULL;
static void *_fwd_SteamAPI_ISteamController_DeactivateActionSetLayer = NULL;
static void *_fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers = NULL;
static void *_fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_GetActionSetHandle = NULL;
static void *_fwd_SteamAPI_ISteamController_GetActiveActionSetLayers = NULL;
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionData = NULL;
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionHandle = NULL;
static void *_fwd_SteamAPI_ISteamController_GetAnalogActionOrigins = NULL;
static void *_fwd_SteamAPI_ISteamController_GetConnectedControllers = NULL;
static void *_fwd_SteamAPI_ISteamController_GetControllerBindingRevision = NULL;
static void *_fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex = NULL;
static void *_fwd_SteamAPI_ISteamController_GetCurrentActionSet = NULL;
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionData = NULL;
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionHandle = NULL;
static void *_fwd_SteamAPI_ISteamController_GetDigitalActionOrigins = NULL;
static void *_fwd_SteamAPI_ISteamController_GetGamepadIndexForController = NULL;
static void *_fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_GetInputTypeForHandle = NULL;
static void *_fwd_SteamAPI_ISteamController_GetMotionData = NULL;
static void *_fwd_SteamAPI_ISteamController_GetStringForActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_GetStringForXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_Init = NULL;
static void *_fwd_SteamAPI_ISteamController_RunFrame = NULL;
static void *_fwd_SteamAPI_ISteamController_SetLEDColor = NULL;
static void *_fwd_SteamAPI_ISteamController_ShowBindingPanel = NULL;
static void *_fwd_SteamAPI_ISteamController_Shutdown = NULL;
static void *_fwd_SteamAPI_ISteamController_StopAnalogActionMomentum = NULL;
static void *_fwd_SteamAPI_ISteamController_TranslateActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamController_TriggerHapticPulse = NULL;
static void *_fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse = NULL;
static void *_fwd_SteamAPI_ISteamController_TriggerVibration = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlay = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage = NULL;
static void *_fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ClearRichPresence = NULL;
static void *_fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam = NULL;
static void *_fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts = NULL;
static void *_fwd_SteamAPI_ISteamFriends_EnumerateFollowingList = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanActivityCounts = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanChatMessage = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanName = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanOfficerCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanOwner = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetClanTag = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetCoplayFriend = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFollowerCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendMessage = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaName = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendPersonaState = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRelationship = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresence = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendsGroupName = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetPersonaName = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetPersonaState = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetPlayerNickname = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar = NULL;
static void *_fwd_SteamAPI_ISteamFriends_GetUserRestrictions = NULL;
static void *_fwd_SteamAPI_ISteamFriends_HasFriend = NULL;
static void *_fwd_SteamAPI_ISteamFriends_InviteUserToGame = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsClanChatAdmin = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsClanPublic = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsFollowing = NULL;
static void *_fwd_SteamAPI_ISteamFriends_IsUserInSource = NULL;
static void *_fwd_SteamAPI_ISteamFriends_JoinClanChatRoom = NULL;
static void *_fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom = NULL;
static void *_fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam = NULL;
static void *_fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser = NULL;
static void *_fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage = NULL;
static void *_fwd_SteamAPI_ISteamFriends_RequestClanOfficerList = NULL;
static void *_fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems = NULL;
static void *_fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence = NULL;
static void *_fwd_SteamAPI_ISteamFriends_RequestUserInformation = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SendClanChatMessage = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SetPersonaName = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SetPlayedWith = NULL;
static void *_fwd_SteamAPI_ISteamFriends_SetRichPresence = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_AcceptGame = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_DeclineGame = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_EndGame = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_EndGameSearch = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_SetGameHostParams = NULL;
static void *_fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_AssociateWithClan = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_BeginAuthSession = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_BLoggedOn = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_BSecure = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_BUpdateUserData = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_CancelAuthTicket = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_EndAuthSession = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetGameplayStats = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetPublicIP = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetServerReputation = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_GetSteamID = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_LogOff = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_LogOn = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_LogOnAnonymous = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetDedicatedServer = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetGameData = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetGameDescription = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetGameTags = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetKeyValue = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetMapName = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetModDir = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetPasswordProtected = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetProduct = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetRegion = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetServerName = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetSpectatorPort = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32 = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_RequestUserStats = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32 = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_StoreUserStats = NULL;
static void *_fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp = NULL;
static void *_fwd_SteamAPI_ISteamGameServer_WasRestartRequested = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_AddHeader = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_Find = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_GoBack = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_GoForward = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_Init = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyChar = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyDown = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_KeyUp = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_LoadURL = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseDown = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseMove = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseUp = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_MouseWheel = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_Reload = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetCookie = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetSize = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_Shutdown = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_StopFind = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_StopLoad = NULL;
static void *_fwd_SteamAPI_ISteamHTMLSurface_ViewSource = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_CreateCookieContainer = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequest = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetCookie = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate = NULL;
static void *_fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo = NULL;
static void *_fwd_SteamAPI_ISteamInput_ActivateActionSet = NULL;
static void *_fwd_SteamAPI_ISteamInput_ActivateActionSetLayer = NULL;
static void *_fwd_SteamAPI_ISteamInput_BNewDataAvailable = NULL;
static void *_fwd_SteamAPI_ISteamInput_BWaitForData = NULL;
static void *_fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer = NULL;
static void *_fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers = NULL;
static void *_fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks = NULL;
static void *_fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetActionSetHandle = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionData = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionHandle = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetConnectedControllers = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetCurrentActionSet = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionData = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionHandle = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetGamepadIndexForController = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetInputTypeForHandle = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetMotionData = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetStringForActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName = NULL;
static void *_fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_Init = NULL;
static void *_fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse = NULL;
static void *_fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse = NULL;
static void *_fwd_SteamAPI_ISteamInput_RunFrame = NULL;
static void *_fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect = NULL;
static void *_fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath = NULL;
static void *_fwd_SteamAPI_ISteamInput_SetLEDColor = NULL;
static void *_fwd_SteamAPI_ISteamInput_ShowBindingPanel = NULL;
static void *_fwd_SteamAPI_ISteamInput_Shutdown = NULL;
static void *_fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum = NULL;
static void *_fwd_SteamAPI_ISteamInput_TranslateActionOrigin = NULL;
static void *_fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent = NULL;
static void *_fwd_SteamAPI_ISteamInput_TriggerVibration = NULL;
static void *_fwd_SteamAPI_ISteamInput_TriggerVibrationExtended = NULL;
static void *_fwd_SteamAPI_ISteamInventory_AddPromoItem = NULL;
static void *_fwd_SteamAPI_ISteamInventory_AddPromoItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_CheckResultSteamID = NULL;
static void *_fwd_SteamAPI_ISteamInventory_ConsumeItem = NULL;
static void *_fwd_SteamAPI_ISteamInventory_DeserializeResult = NULL;
static void *_fwd_SteamAPI_ISteamInventory_DestroyResult = NULL;
static void *_fwd_SteamAPI_ISteamInventory_ExchangeItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GenerateItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetAllItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetItemPrice = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetItemsByID = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetItemsWithPrices = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetResultItemProperty = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetResultItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetResultStatus = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GetResultTimestamp = NULL;
static void *_fwd_SteamAPI_ISteamInventory_GrantPromoItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_InspectItem = NULL;
static void *_fwd_SteamAPI_ISteamInventory_LoadItemDefinitions = NULL;
static void *_fwd_SteamAPI_ISteamInventory_RemoveProperty = NULL;
static void *_fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs = NULL;
static void *_fwd_SteamAPI_ISteamInventory_RequestPrices = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SerializeResult = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyBool = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyFloat = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyInt64 = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SetPropertyString = NULL;
static void *_fwd_SteamAPI_ISteamInventory_StartPurchase = NULL;
static void *_fwd_SteamAPI_ISteamInventory_StartUpdateProperties = NULL;
static void *_fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties = NULL;
static void *_fwd_SteamAPI_ISteamInventory_TradeItems = NULL;
static void *_fwd_SteamAPI_ISteamInventory_TransferItemQuantity = NULL;
static void *_fwd_SteamAPI_ISteamInventory_TriggerItemDrop = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_CreateLobby = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_JoinLobby = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_LeaveLobby = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_PingServer = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList = NULL;
static void *_fwd_SteamAPI_ISteamMatchmakingServers_ServerRules = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner = NULL;
static void *_fwd_SteamAPI_ISteamMatchmaking_SetLobbyType = NULL;
static void *_fwd_SteamAPI_ISteamMusic_BIsEnabled = NULL;
static void *_fwd_SteamAPI_ISteamMusic_BIsPlaying = NULL;
static void *_fwd_SteamAPI_ISteamMusic_GetPlaybackStatus = NULL;
static void *_fwd_SteamAPI_ISteamMusic_GetVolume = NULL;
static void *_fwd_SteamAPI_ISteamMusic_Pause = NULL;
static void *_fwd_SteamAPI_ISteamMusic_Play = NULL;
static void *_fwd_SteamAPI_ISteamMusic_PlayNext = NULL;
static void *_fwd_SteamAPI_ISteamMusic_PlayPrevious = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableLooped = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableQueue = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_EnableShuffled = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_QueueDidChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_QueueWillChange = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetDisplayName = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateLooped = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled = NULL;
static void *_fwd_SteamAPI_ISteamMusicRemote_UpdateVolume = NULL;
static void *_fwd_SteamAPI_ISteamMusic_SetVolume = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_CreateListenSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_DestroyListenSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_DestroySocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_GetP2PSessionState = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_GetSocketInfo = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_IsDataAvailable = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_ReadP2PPacket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_RetrieveData = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_SendDataOnSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworking_SendP2PPacket = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessages = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString = NULL;
static void *_fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled = NULL;
static void *_fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked = NULL;
static void *_fwd_SteamAPI_ISteamParties_CancelReservation = NULL;
static void *_fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots = NULL;
static void *_fwd_SteamAPI_ISteamParties_CreateBeacon = NULL;
static void *_fwd_SteamAPI_ISteamParties_DestroyBeacon = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetBeaconByIndex = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetBeaconDetails = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetBeaconLocationData = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetNumActiveBeacons = NULL;
static void *_fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations = NULL;
static void *_fwd_SteamAPI_ISteamParties_JoinParty = NULL;
static void *_fwd_SteamAPI_ISteamParties_OnReservationCompleted = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionCount = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionID = NULL;
static void *_fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileDelete = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileExists = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileForget = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FilePersisted = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileRead = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileShare = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWrite = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileCount = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileSize = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetQuota = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_PublishVideo = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownload = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UGCRead = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility = NULL;
static void *_fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_HookScreenshots = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_SetLocation = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_TagPublishedFile = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_TagUser = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot = NULL;
static void *_fwd_SteamAPI_ISteamScreenshots_WriteScreenshot = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_EndGamePhase = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_SetGamePhaseID = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_StartGamePhase = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddAppDependency = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddContentDescriptor = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddDependency = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddExcludedTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddItemPreviewFile = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddItemToFavorites = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup = NULL;
static void *_fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer = NULL;
static void *_fwd_SteamAPI_ISteamUGC_CreateItem = NULL;
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor = NULL;
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage = NULL;
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest = NULL;
static void *_fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest = NULL;
static void *_fwd_SteamAPI_ISteamUGC_DeleteItem = NULL;
static void *_fwd_SteamAPI_ISteamUGC_DownloadItem = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetAppDependencies = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetItemInstallInfo = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetItemState = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCResult = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetSubscribedItems = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetUserItemVote = NULL;
static void *_fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus = NULL;
static void *_fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveAppDependency = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveDependency = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RemoveItemPreview = NULL;
static void *_fwd_SteamAPI_ISteamUGC_RequestUGCDetails = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetAdminQuery = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemContent = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemDescription = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemMetadata = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemPreview = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemTitle = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetItemVisibility = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetLanguage = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetMatchAnyTag = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnChildren = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnLongDescription = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnMetadata = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetSearchText = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SetUserItemVote = NULL;
static void *_fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA = NULL;
static void *_fwd_SteamAPI_ISteamUGC_StartItemUpdate = NULL;
static void *_fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking = NULL;
static void *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking = NULL;
static void *_fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SubmitItemUpdate = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SubscribeItem = NULL;
static void *_fwd_SteamAPI_ISteamUGC_SuspendDownloads = NULL;
static void *_fwd_SteamAPI_ISteamUGC_UnsubscribeItem = NULL;
static void *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile = NULL;
static void *_fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo = NULL;
static void *_fwd_SteamAPI_ISteamUser_AdvertiseGame = NULL;
static void *_fwd_SteamAPI_ISteamUser_BeginAuthSession = NULL;
static void *_fwd_SteamAPI_ISteamUser_BIsBehindNAT = NULL;
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying = NULL;
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification = NULL;
static void *_fwd_SteamAPI_ISteamUser_BIsPhoneVerified = NULL;
static void *_fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled = NULL;
static void *_fwd_SteamAPI_ISteamUser_BLoggedOn = NULL;
static void *_fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState = NULL;
static void *_fwd_SteamAPI_ISteamUser_CancelAuthTicket = NULL;
static void *_fwd_SteamAPI_ISteamUser_DecompressVoice = NULL;
static void *_fwd_SteamAPI_ISteamUser_EndAuthSession = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetAuthSessionTicket = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetAvailableVoice = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetDurationControl = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetGameBadgeLevel = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetHSteamUser = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetMarketEligibility = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetSteamID = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetUserDataFolder = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetVoice = NULL;
static void *_fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate = NULL;
static void *_fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED = NULL;
static void *_fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket = NULL;
static void *_fwd_SteamAPI_ISteamUser_RequestStoreAuthURL = NULL;
static void *_fwd_SteamAPI_ISteamUser_StartVoiceRecording = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_ClearAchievement = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_FindLeaderboard = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievement = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementIcon = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementName = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardName = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetNumAchievements = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetStatFloat = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetStatInt32 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetUserAchievement = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetUserStatFloat = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_GetUserStatInt32 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_RequestGlobalStats = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_RequestUserStats = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_ResetAllStats = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_SetAchievement = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_SetStatFloat = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_SetStatInt32 = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_StoreStats = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat = NULL;
static void *_fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore = NULL;
static void *_fwd_SteamAPI_ISteamUser_StopVoiceRecording = NULL;
static void *_fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED = NULL;
static void *_fwd_SteamAPI_ISteamUser_TrackAppUsageEvent = NULL;
static void *_fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent = NULL;
static void *_fwd_SteamAPI_ISteamUtils_CheckFileSignature = NULL;
static void *_fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = NULL;
static void *_fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput = NULL;
static void *_fwd_SteamAPI_ISteamUtils_FilterText = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetAPICallResult = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetAppID = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetConnectedUniverse = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetImageRGBA = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetImageSize = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetIPCCallCount = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetIPCountry = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetServerRealTime = NULL;
static void *_fwd_SteamAPI_ISteamUtils_GetSteamUILanguage = NULL;
static void *_fwd_SteamAPI_ISteamUtils_InitFilterText = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsAPICallCompleted = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsOverlayEnabled = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = NULL;
static void *_fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled = NULL;
static void *_fwd_SteamAPI_ISteamUtils_SetGameLauncherMode = NULL;
static void *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset = NULL;
static void *_fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition = NULL;
static void *_fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled = NULL;
static void *_fwd_SteamAPI_ISteamUtils_SetWarningMessageHook = NULL;
static void *_fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = NULL;
static void *_fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput = NULL;
static void *_fwd_SteamAPI_ISteamUtils_StartVRDashboard = NULL;
static void *_fwd_SteamAPI_ISteamVideo_GetOPFSettings = NULL;
static void *_fwd_SteamAPI_ISteamVideo_GetOPFStringForApp = NULL;
static void *_fwd_SteamAPI_ISteamVideo_GetVideoURL = NULL;
static void *_fwd_SteamAPI_ISteamVideo_IsBroadcasting = NULL;
static void *_fwd_SteamAPI_ManualDispatch_FreeLastCallback = NULL;
static void *_fwd_SteamAPI_ManualDispatch_GetAPICallResult = NULL;
static void *_fwd_SteamAPI_ManualDispatch_GetNextCallback = NULL;
static void *_fwd_SteamAPI_ManualDispatch_Init = NULL;
static void *_fwd_SteamAPI_ManualDispatch_RunFrame = NULL;
static void *_fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct = NULL;
static void *_fwd_SteamAPI_RegisterCallback = NULL;
static void *_fwd_SteamAPI_RegisterCallResult = NULL;
static void *_fwd_SteamAPI_ReleaseCurrentThreadMemory = NULL;
static void *_fwd_SteamAPI_RestartAppIfNecessary = NULL;
static void *_fwd_SteamAPI_RunCallbacks = NULL;
static void *_fwd_SteamAPI_servernetadr_t_Assign = NULL;
static void *_fwd_SteamAPI_servernetadr_t_Construct = NULL;
static void *_fwd_SteamAPI_servernetadr_t_GetConnectionAddressString = NULL;
static void *_fwd_SteamAPI_servernetadr_t_GetConnectionPort = NULL;
static void *_fwd_SteamAPI_servernetadr_t_GetIP = NULL;
static void *_fwd_SteamAPI_servernetadr_t_GetQueryAddressString = NULL;
static void *_fwd_SteamAPI_servernetadr_t_GetQueryPort = NULL;
static void *_fwd_SteamAPI_servernetadr_t_Init = NULL;
static void *_fwd_SteamAPI_servernetadr_t_IsLessThan = NULL;
static void *_fwd_SteamAPI_servernetadr_t_SetConnectionPort = NULL;
static void *_fwd_SteamAPI_servernetadr_t_SetIP = NULL;
static void *_fwd_SteamAPI_servernetadr_t_SetQueryPort = NULL;
static void *_fwd_SteamAPI_SetBreakpadAppID = NULL;
static void *_fwd_SteamAPI_SetMiniDumpComment = NULL;
static void *_fwd_SteamAPI_SetTryCatchCallbacks = NULL;
static void *_fwd_SteamAPI_Shutdown = NULL;
static void *_fwd_SteamAPI_SteamApps_v008 = NULL;
static void *_fwd_SteamAPI_SteamController_v008 = NULL;
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_Clear = NULL;
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID = NULL;
static void *_fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress = NULL;
static void *_fwd_SteamAPI_SteamFriends_v017 = NULL;
static void *_fwd_SteamAPI_SteamGameSearch_v001 = NULL;
static void *_fwd_SteamAPI_SteamGameServerHTTP_v003 = NULL;
static void *_fwd_SteamAPI_SteamGameServerInventory_v003 = NULL;
static void *_fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 = NULL;
static void *_fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 = NULL;
static void *_fwd_SteamAPI_SteamGameServerNetworking_v006 = NULL;
static void *_fwd_SteamAPI_SteamGameServerStats_v001 = NULL;
static void *_fwd_SteamAPI_SteamGameServerUGC_v020 = NULL;
static void *_fwd_SteamAPI_SteamGameServerUtils_v010 = NULL;
static void *_fwd_SteamAPI_SteamGameServer_v015 = NULL;
static void *_fwd_SteamAPI_SteamHTMLSurface_v005 = NULL;
static void *_fwd_SteamAPI_SteamHTTP_v003 = NULL;
static void *_fwd_SteamAPI_SteamInput_v006 = NULL;
static void *_fwd_SteamAPI_SteamInventory_v003 = NULL;
static void *_fwd_SteamAPI_SteamIPAddress_t_IsSet = NULL;
static void *_fwd_SteamAPI_SteamMatchmakingServers_v002 = NULL;
static void *_fwd_SteamAPI_SteamMatchmaking_v009 = NULL;
static void *_fwd_SteamAPI_SteamMusicRemote_v001 = NULL;
static void *_fwd_SteamAPI_SteamMusic_v001 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat = NULL;
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr = NULL;
static void *_fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_Clear = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_ParseString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIdentity_ToString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_Clear = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_ParseString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost = NULL;
static void *_fwd_SteamAPI_SteamNetworkingIPAddr_ToString = NULL;
static void *_fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingMessage_t_Release = NULL;
static void *_fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012 = NULL;
static void *_fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004 = NULL;
static void *_fwd_SteamAPI_SteamNetworking_v006 = NULL;
static void *_fwd_SteamAPI_SteamParentalSettings_v001 = NULL;
static void *_fwd_SteamAPI_SteamParties_v002 = NULL;
static void *_fwd_SteamAPI_SteamRemotePlay_v002 = NULL;
static void *_fwd_SteamAPI_SteamRemoteStorage_v016 = NULL;
static void *_fwd_SteamAPI_SteamScreenshots_v003 = NULL;
static void *_fwd_SteamAPI_SteamTimeline_v004 = NULL;
static void *_fwd_SteamAPI_SteamUGC_v020 = NULL;
static void *_fwd_SteamAPI_SteamUserStats_v013 = NULL;
static void *_fwd_SteamAPI_SteamUser_v023 = NULL;
static void *_fwd_SteamAPI_SteamUtils_v010 = NULL;
static void *_fwd_SteamAPI_SteamVideo_v007 = NULL;
static void *_fwd_SteamAPI_UnregisterCallback = NULL;
static void *_fwd_SteamAPI_UnregisterCallResult = NULL;
static void *_fwd_SteamAPI_UseBreakpadCrashHandler = NULL;
static void *_fwd_SteamAPI_WriteMiniDump = NULL;
static void *_fwd_SteamClient = NULL;
static void *_fwd_SteamGameServer_BSecure = NULL;
static void *_fwd_SteamGameServer_GetHSteamPipe = NULL;
static void *_fwd_SteamGameServer_GetHSteamUser = NULL;
static void *_fwd_SteamGameServer_GetIPCCallCount = NULL;
static void *_fwd_SteamGameServer_GetSteamID = NULL;
static void *_fwd_SteamGameServer_InitSafe = NULL;
static void *_fwd_SteamGameServer_RunCallbacks = NULL;
static void *_fwd_SteamGameServer_Shutdown = NULL;
static void *_fwd_SteamInternal_ContextInit = NULL;
static void *_fwd_SteamInternal_CreateInterface = NULL;
static void *_fwd_SteamInternal_FindOrCreateGameServerInterface = NULL;
static void *_fwd_SteamInternal_FindOrCreateUserInterface = NULL;
static void *_fwd_SteamInternal_GameServer_Init_V2 = NULL;
static void *_fwd_SteamInternal_SteamAPI_Init = NULL;
static void *_fwd_SteamRealPath = NULL;
static void *_fwd___wrap_access = NULL;
static void *_fwd___wrap_chdir = NULL;
static void *_fwd___wrap_chmod = NULL;
static void *_fwd___wrap_chown = NULL;
static void *_fwd___wrap_dlmopen = NULL;
static void *_fwd___wrap_dlopen = NULL;
static void *_fwd___wrap_fopen = NULL;
static void *_fwd___wrap_fopen64 = NULL;
static void *_fwd___wrap_freopen = NULL;
static void *_fwd___wrap_lchown = NULL;
static void *_fwd___wrap_link = NULL;
static void *_fwd___wrap_lstat = NULL;
static void *_fwd___wrap_lstat64 = NULL;
static void *_fwd___wrap___lxstat = NULL;
static void *_fwd___wrap___lxstat64 = NULL;
static void *_fwd___wrap_mkdir = NULL;
static void *_fwd___wrap_mkfifo = NULL;
static void *_fwd___wrap_mknod = NULL;
static void *_fwd___wrap_mount = NULL;
static void *_fwd___wrap_open = NULL;
static void *_fwd___wrap_open64 = NULL;
static void *_fwd___wrap_opendir = NULL;
static void *_fwd___wrap_rename = NULL;
static void *_fwd___wrap_rmdir = NULL;
static void *_fwd___wrap_scandir = NULL;
static void *_fwd___wrap_scandir64 = NULL;
static void *_fwd___wrap_stat = NULL;
static void *_fwd___wrap_stat64 = NULL;
static void *_fwd___wrap_statfs = NULL;
static void *_fwd___wrap_statfs64 = NULL;
static void *_fwd___wrap_statvfs = NULL;
static void *_fwd___wrap_statvfs64 = NULL;
static void *_fwd___wrap_symlink = NULL;
static void *_fwd___wrap_unlink = NULL;
static void *_fwd___wrap_utime = NULL;
static void *_fwd___wrap_utimes = NULL;
static void *_fwd___wrap___xstat = NULL;
static void *_fwd___wrap___xstat64 = NULL;

/* GetHSteamPipe */
__attribute__((naked)) void GetHSteamPipe(void) {
    __asm__ volatile (
        "movq _fwd_GetHSteamPipe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* GetHSteamUser */
__attribute__((naked)) void GetHSteamUser(void) {
    __asm__ volatile (
        "movq _fwd_GetHSteamUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_gameserveritem_t_Construct */
__attribute__((naked)) void SteamAPI_gameserveritem_t_Construct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_gameserveritem_t_Construct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_gameserveritem_t_GetName */
__attribute__((naked)) void SteamAPI_gameserveritem_t_GetName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_gameserveritem_t_GetName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_gameserveritem_t_SetName */
__attribute__((naked)) void SteamAPI_gameserveritem_t_SetName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_gameserveritem_t_SetName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_GetHSteamPipe */
__attribute__((naked)) void SteamAPI_GetHSteamPipe(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_GetHSteamPipe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_GetHSteamUser */
__attribute__((naked)) void SteamAPI_GetHSteamUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_GetHSteamUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_GetSteamInstallPath */
__attribute__((naked)) void SteamAPI_GetSteamInstallPath(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_GetSteamInstallPath@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_InitAnonymousUser */
__attribute__((naked)) void SteamAPI_InitAnonymousUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_InitAnonymousUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_InitFlat */
__attribute__((naked)) void SteamAPI_InitFlat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_InitFlat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_InitSafe */
__attribute__((naked)) void SteamAPI_InitSafe(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_InitSafe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_IsSteamRunning */
__attribute__((naked)) void SteamAPI_IsSteamRunning(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_IsSteamRunning@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsCybercafe */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsCybercafe(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsCybercafe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsLowViolence */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsLowViolence(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsLowViolence@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsTimedTrial */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsTimedTrial(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsTimedTrial@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_BIsVACBanned */
__attribute__((naked)) void SteamAPI_ISteamApps_BIsVACBanned(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_BIsVACBanned@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetAppBuildId */
__attribute__((naked)) void SteamAPI_ISteamApps_GetAppBuildId(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetAppBuildId@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetAppInstallDir */
__attribute__((naked)) void SteamAPI_ISteamApps_GetAppInstallDir(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetAppInstallDir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetAppOwner */
__attribute__((naked)) void SteamAPI_ISteamApps_GetAppOwner(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetAppOwner@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetAvailableGameLanguages */
__attribute__((naked)) void SteamAPI_ISteamApps_GetAvailableGameLanguages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetBetaInfo */
__attribute__((naked)) void SteamAPI_ISteamApps_GetBetaInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetBetaInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetCurrentBetaName */
__attribute__((naked)) void SteamAPI_ISteamApps_GetCurrentBetaName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetCurrentBetaName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetCurrentGameLanguage */
__attribute__((naked)) void SteamAPI_ISteamApps_GetCurrentGameLanguage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetDlcDownloadProgress */
__attribute__((naked)) void SteamAPI_ISteamApps_GetDlcDownloadProgress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetFileDetails */
__attribute__((naked)) void SteamAPI_ISteamApps_GetFileDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetFileDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetInstalledDepots */
__attribute__((naked)) void SteamAPI_ISteamApps_GetInstalledDepots(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetInstalledDepots@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetLaunchCommandLine */
__attribute__((naked)) void SteamAPI_ISteamApps_GetLaunchCommandLine(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetLaunchCommandLine@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetLaunchQueryParam */
__attribute__((naked)) void SteamAPI_ISteamApps_GetLaunchQueryParam(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetLaunchQueryParam@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_GetNumBetas */
__attribute__((naked)) void SteamAPI_ISteamApps_GetNumBetas(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_GetNumBetas@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_InstallDLC */
__attribute__((naked)) void SteamAPI_ISteamApps_InstallDLC(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_InstallDLC@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_MarkContentCorrupt */
__attribute__((naked)) void SteamAPI_ISteamApps_MarkContentCorrupt(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_MarkContentCorrupt@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys */
__attribute__((naked)) void SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey */
__attribute__((naked)) void SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_SetActiveBeta */
__attribute__((naked)) void SteamAPI_ISteamApps_SetActiveBeta(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_SetActiveBeta@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_SetDlcContext */
__attribute__((naked)) void SteamAPI_ISteamApps_SetDlcContext(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_SetDlcContext@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamApps_UninstallDLC */
__attribute__((naked)) void SteamAPI_ISteamApps_UninstallDLC(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamApps_UninstallDLC@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_BReleaseSteamPipe */
__attribute__((naked)) void SteamAPI_ISteamClient_BReleaseSteamPipe(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_BReleaseSteamPipe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_BShutdownIfAllPipesClosed */
__attribute__((naked)) void SteamAPI_ISteamClient_BShutdownIfAllPipesClosed(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_ConnectToGlobalUser */
__attribute__((naked)) void SteamAPI_ISteamClient_ConnectToGlobalUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_ConnectToGlobalUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_CreateLocalUser */
__attribute__((naked)) void SteamAPI_ISteamClient_CreateLocalUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_CreateLocalUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_CreateSteamPipe */
__attribute__((naked)) void SteamAPI_ISteamClient_CreateSteamPipe(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_CreateSteamPipe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetIPCCallCount */
__attribute__((naked)) void SteamAPI_ISteamClient_GetIPCCallCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetIPCCallCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamApps */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamApps(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamApps@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamController */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamController(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamController@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamFriends */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamFriends(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamFriends@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamGameSearch */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamGameSearch(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamGameSearch@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamGameServer */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamGameServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamGameServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamGameServerStats */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamGameServerStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamGameServerStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamGenericInterface */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamGenericInterface(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamGenericInterface@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamHTMLSurface */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamHTMLSurface(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamHTTP */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamHTTP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamHTTP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamInput */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamInventory */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamInventory(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamInventory@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamMatchmaking */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamMatchmaking(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamMatchmaking@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamMatchmakingServers */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamMatchmakingServers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamMusic */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamMusic(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamMusic@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamMusicRemote */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamMusicRemote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamMusicRemote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamNetworking */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamNetworking(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamNetworking@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamParentalSettings */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamParentalSettings(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamParentalSettings@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamParties */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamParties(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamParties@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamRemotePlay */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamRemotePlay(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamRemotePlay@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamRemoteStorage */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamRemoteStorage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamScreenshots */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamScreenshots(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamScreenshots@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamUGC */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamUGC(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamUGC@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamUser */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamUserStats */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamUserStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamUserStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamUtils */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamUtils(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamUtils@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_GetISteamVideo */
__attribute__((naked)) void SteamAPI_ISteamClient_GetISteamVideo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_GetISteamVideo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_ReleaseUser */
__attribute__((naked)) void SteamAPI_ISteamClient_ReleaseUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_ReleaseUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_SetLocalIPBinding */
__attribute__((naked)) void SteamAPI_ISteamClient_SetLocalIPBinding(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_SetLocalIPBinding@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamClient_SetWarningMessageHook */
__attribute__((naked)) void SteamAPI_ISteamClient_SetWarningMessageHook(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamClient_SetWarningMessageHook@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_ActivateActionSet */
__attribute__((naked)) void SteamAPI_ISteamController_ActivateActionSet(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_ActivateActionSet@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_ActivateActionSetLayer */
__attribute__((naked)) void SteamAPI_ISteamController_ActivateActionSetLayer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_ActivateActionSetLayer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_DeactivateActionSetLayer */
__attribute__((naked)) void SteamAPI_ISteamController_DeactivateActionSetLayer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_DeactivateActionSetLayer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_DeactivateAllActionSetLayers */
__attribute__((naked)) void SteamAPI_ISteamController_DeactivateAllActionSetLayers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetActionOriginFromXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_GetActionOriginFromXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetActionSetHandle */
__attribute__((naked)) void SteamAPI_ISteamController_GetActionSetHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetActionSetHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetActiveActionSetLayers */
__attribute__((naked)) void SteamAPI_ISteamController_GetActiveActionSetLayers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetActiveActionSetLayers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetAnalogActionData */
__attribute__((naked)) void SteamAPI_ISteamController_GetAnalogActionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetAnalogActionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetAnalogActionHandle */
__attribute__((naked)) void SteamAPI_ISteamController_GetAnalogActionHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetAnalogActionHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetAnalogActionOrigins */
__attribute__((naked)) void SteamAPI_ISteamController_GetAnalogActionOrigins(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetAnalogActionOrigins@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetConnectedControllers */
__attribute__((naked)) void SteamAPI_ISteamController_GetConnectedControllers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetConnectedControllers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetControllerBindingRevision */
__attribute__((naked)) void SteamAPI_ISteamController_GetControllerBindingRevision(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetControllerBindingRevision@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetControllerForGamepadIndex */
__attribute__((naked)) void SteamAPI_ISteamController_GetControllerForGamepadIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetCurrentActionSet */
__attribute__((naked)) void SteamAPI_ISteamController_GetCurrentActionSet(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetCurrentActionSet@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetDigitalActionData */
__attribute__((naked)) void SteamAPI_ISteamController_GetDigitalActionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetDigitalActionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetDigitalActionHandle */
__attribute__((naked)) void SteamAPI_ISteamController_GetDigitalActionHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetDigitalActionHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetDigitalActionOrigins */
__attribute__((naked)) void SteamAPI_ISteamController_GetDigitalActionOrigins(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetDigitalActionOrigins@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetGamepadIndexForController */
__attribute__((naked)) void SteamAPI_ISteamController_GetGamepadIndexForController(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetGamepadIndexForController@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetGlyphForActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_GetGlyphForActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetGlyphForXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_GetGlyphForXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetInputTypeForHandle */
__attribute__((naked)) void SteamAPI_ISteamController_GetInputTypeForHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetInputTypeForHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetMotionData */
__attribute__((naked)) void SteamAPI_ISteamController_GetMotionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetMotionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetStringForActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_GetStringForActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetStringForActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_GetStringForXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_GetStringForXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_GetStringForXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_Init */
__attribute__((naked)) void SteamAPI_ISteamController_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_RunFrame */
__attribute__((naked)) void SteamAPI_ISteamController_RunFrame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_RunFrame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_SetLEDColor */
__attribute__((naked)) void SteamAPI_ISteamController_SetLEDColor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_SetLEDColor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_ShowBindingPanel */
__attribute__((naked)) void SteamAPI_ISteamController_ShowBindingPanel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_ShowBindingPanel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_Shutdown */
__attribute__((naked)) void SteamAPI_ISteamController_Shutdown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_Shutdown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_StopAnalogActionMomentum */
__attribute__((naked)) void SteamAPI_ISteamController_StopAnalogActionMomentum(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_StopAnalogActionMomentum@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_TranslateActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamController_TranslateActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_TranslateActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_TriggerHapticPulse */
__attribute__((naked)) void SteamAPI_ISteamController_TriggerHapticPulse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_TriggerHapticPulse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_TriggerRepeatedHapticPulse */
__attribute__((naked)) void SteamAPI_ISteamController_TriggerRepeatedHapticPulse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamController_TriggerVibration */
__attribute__((naked)) void SteamAPI_ISteamController_TriggerVibration(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamController_TriggerVibration@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlay */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlay(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlay@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayToStore */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayToStore(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayToUser */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayToUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage */
__attribute__((naked)) void SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_BHasEquippedProfileItem */
__attribute__((naked)) void SteamAPI_ISteamFriends_BHasEquippedProfileItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ClearRichPresence */
__attribute__((naked)) void SteamAPI_ISteamFriends_ClearRichPresence(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ClearRichPresence@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_CloseClanChatWindowInSteam */
__attribute__((naked)) void SteamAPI_ISteamFriends_CloseClanChatWindowInSteam(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_DownloadClanActivityCounts */
__attribute__((naked)) void SteamAPI_ISteamFriends_DownloadClanActivityCounts(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_EnumerateFollowingList */
__attribute__((naked)) void SteamAPI_ISteamFriends_EnumerateFollowingList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_EnumerateFollowingList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetChatMemberByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetChatMemberByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanActivityCounts */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanActivityCounts(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanActivityCounts@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanChatMemberCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanChatMemberCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanChatMessage */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanChatMessage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanChatMessage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanName */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanOfficerByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanOfficerByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanOfficerCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanOfficerCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanOfficerCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanOwner */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanOwner(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanOwner@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetClanTag */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetClanTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetClanTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetCoplayFriend */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetCoplayFriend(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetCoplayFriend@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetCoplayFriendCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetCoplayFriendCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFollowerCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFollowerCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFollowerCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendCoplayGame */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendCoplayGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendCoplayTime */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendCoplayTime(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendCountFromSource */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendCountFromSource(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendFromSourceByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendFromSourceByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendGamePlayed */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendGamePlayed(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendMessage */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendMessage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendMessage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendPersonaName */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendPersonaName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendPersonaName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendPersonaNameHistory */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendPersonaNameHistory(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendPersonaState */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendPersonaState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendPersonaState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendRelationship */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendRelationship(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendRelationship@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendRichPresence */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendRichPresence(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendRichPresence@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendsGroupCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendsGroupCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendsGroupMembersCount */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendsGroupMembersCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendsGroupMembersList */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendsGroupMembersList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendsGroupName */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendsGroupName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendsGroupName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetFriendSteamLevel */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetFriendSteamLevel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetLargeFriendAvatar */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetLargeFriendAvatar(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetMediumFriendAvatar */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetMediumFriendAvatar(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetPersonaName */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetPersonaName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetPersonaName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetPersonaState */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetPersonaState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetPersonaState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetPlayerNickname */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetPlayerNickname(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetPlayerNickname@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetProfileItemPropertyString */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetProfileItemPropertyString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetProfileItemPropertyUint */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetProfileItemPropertyUint(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetSmallFriendAvatar */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetSmallFriendAvatar(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_GetUserRestrictions */
__attribute__((naked)) void SteamAPI_ISteamFriends_GetUserRestrictions(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_GetUserRestrictions@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_HasFriend */
__attribute__((naked)) void SteamAPI_ISteamFriends_HasFriend(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_HasFriend@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_InviteUserToGame */
__attribute__((naked)) void SteamAPI_ISteamFriends_InviteUserToGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_InviteUserToGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsClanChatAdmin */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsClanChatAdmin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsClanChatAdmin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsClanOfficialGameGroup */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsClanOfficialGameGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsClanPublic */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsClanPublic(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsClanPublic@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsFollowing */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsFollowing(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsFollowing@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_IsUserInSource */
__attribute__((naked)) void SteamAPI_ISteamFriends_IsUserInSource(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_IsUserInSource@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_JoinClanChatRoom */
__attribute__((naked)) void SteamAPI_ISteamFriends_JoinClanChatRoom(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_JoinClanChatRoom@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_LeaveClanChatRoom */
__attribute__((naked)) void SteamAPI_ISteamFriends_LeaveClanChatRoom(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_OpenClanChatWindowInSteam */
__attribute__((naked)) void SteamAPI_ISteamFriends_OpenClanChatWindowInSteam(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser */
__attribute__((naked)) void SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_ReplyToFriendMessage */
__attribute__((naked)) void SteamAPI_ISteamFriends_ReplyToFriendMessage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_RequestClanOfficerList */
__attribute__((naked)) void SteamAPI_ISteamFriends_RequestClanOfficerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_RequestClanOfficerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_RequestEquippedProfileItems */
__attribute__((naked)) void SteamAPI_ISteamFriends_RequestEquippedProfileItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_RequestFriendRichPresence */
__attribute__((naked)) void SteamAPI_ISteamFriends_RequestFriendRichPresence(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_RequestUserInformation */
__attribute__((naked)) void SteamAPI_ISteamFriends_RequestUserInformation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_RequestUserInformation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SendClanChatMessage */
__attribute__((naked)) void SteamAPI_ISteamFriends_SendClanChatMessage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SendClanChatMessage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SetInGameVoiceSpeaking */
__attribute__((naked)) void SteamAPI_ISteamFriends_SetInGameVoiceSpeaking(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SetListenForFriendsMessages */
__attribute__((naked)) void SteamAPI_ISteamFriends_SetListenForFriendsMessages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SetPersonaName */
__attribute__((naked)) void SteamAPI_ISteamFriends_SetPersonaName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SetPersonaName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SetPlayedWith */
__attribute__((naked)) void SteamAPI_ISteamFriends_SetPlayedWith(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SetPlayedWith@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamFriends_SetRichPresence */
__attribute__((naked)) void SteamAPI_ISteamFriends_SetRichPresence(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamFriends_SetRichPresence@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_AcceptGame */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_AcceptGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_AcceptGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_AddGameSearchParams */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_AddGameSearchParams(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_DeclineGame */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_DeclineGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_DeclineGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_EndGame */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_EndGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_EndGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_EndGameSearch */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_EndGameSearch(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_EndGameSearch@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_HostConfirmGameStart */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_HostConfirmGameStart(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_RequestPlayersForGame */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_RequestPlayersForGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_RetrieveConnectionDetails */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_RetrieveConnectionDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_SearchForGameSolo */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_SearchForGameSolo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_SearchForGameWithLobby */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_SearchForGameWithLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_SetConnectionDetails */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_SetConnectionDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_SetGameHostParams */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_SetGameHostParams(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_SetGameHostParams@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameSearch_SubmitPlayerResult */
__attribute__((naked)) void SteamAPI_ISteamGameSearch_SubmitPlayerResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_AssociateWithClan */
__attribute__((naked)) void SteamAPI_ISteamGameServer_AssociateWithClan(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_AssociateWithClan@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_BeginAuthSession */
__attribute__((naked)) void SteamAPI_ISteamGameServer_BeginAuthSession(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_BeginAuthSession@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_BLoggedOn */
__attribute__((naked)) void SteamAPI_ISteamGameServer_BLoggedOn(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_BLoggedOn@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_BSecure */
__attribute__((naked)) void SteamAPI_ISteamGameServer_BSecure(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_BSecure@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_BUpdateUserData */
__attribute__((naked)) void SteamAPI_ISteamGameServer_BUpdateUserData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_BUpdateUserData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_CancelAuthTicket */
__attribute__((naked)) void SteamAPI_ISteamGameServer_CancelAuthTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_CancelAuthTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_ClearAllKeyValues */
__attribute__((naked)) void SteamAPI_ISteamGameServer_ClearAllKeyValues(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility */
__attribute__((naked)) void SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection */
__attribute__((naked)) void SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_EndAuthSession */
__attribute__((naked)) void SteamAPI_ISteamGameServer_EndAuthSession(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_EndAuthSession@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetAuthSessionTicket */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetAuthSessionTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetGameplayStats */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetGameplayStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetGameplayStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetNextOutgoingPacket */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetNextOutgoingPacket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetPublicIP */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetPublicIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetPublicIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetServerReputation */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetServerReputation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetServerReputation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_GetSteamID */
__attribute__((naked)) void SteamAPI_ISteamGameServer_GetSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_GetSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_HandleIncomingPacket */
__attribute__((naked)) void SteamAPI_ISteamGameServer_HandleIncomingPacket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_LogOff */
__attribute__((naked)) void SteamAPI_ISteamGameServer_LogOff(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_LogOff@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_LogOn */
__attribute__((naked)) void SteamAPI_ISteamGameServer_LogOn(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_LogOn@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_LogOnAnonymous */
__attribute__((naked)) void SteamAPI_ISteamGameServer_LogOnAnonymous(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_LogOnAnonymous@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_RequestUserGroupStatus */
__attribute__((naked)) void SteamAPI_ISteamGameServer_RequestUserGroupStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetAdvertiseServerActive */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetAdvertiseServerActive(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetBotPlayerCount */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetBotPlayerCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetDedicatedServer */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetDedicatedServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetDedicatedServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetGameData */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetGameData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetGameData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetGameDescription */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetGameDescription(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetGameDescription@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetGameTags */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetGameTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetGameTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetKeyValue */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetKeyValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetKeyValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetMapName */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetMapName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetMapName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetMaxPlayerCount */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetMaxPlayerCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetModDir */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetModDir(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetModDir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetPasswordProtected */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetPasswordProtected(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetPasswordProtected@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetProduct */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetProduct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetProduct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetRegion */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetRegion(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetRegion@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetServerName */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetServerName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetServerName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetSpectatorPort */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetSpectatorPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetSpectatorPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_SetSpectatorServerName */
__attribute__((naked)) void SteamAPI_ISteamGameServer_SetSpectatorServerName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_ClearUserAchievement */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_ClearUserAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_GetUserAchievement */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_GetUserAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_GetUserStatFloat */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_GetUserStatFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_GetUserStatInt32 */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_GetUserStatInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_RequestUserStats */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_RequestUserStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_RequestUserStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_SetUserAchievement */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_SetUserAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_SetUserStatFloat */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_SetUserStatFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_SetUserStatInt32 */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_SetUserStatInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_StoreUserStats */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_StoreUserStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_StoreUserStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat */
__attribute__((naked)) void SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_UserHasLicenseForApp */
__attribute__((naked)) void SteamAPI_ISteamGameServer_UserHasLicenseForApp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamGameServer_WasRestartRequested */
__attribute__((naked)) void SteamAPI_ISteamGameServer_WasRestartRequested(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamGameServer_WasRestartRequested@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_AddHeader */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_AddHeader(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_AddHeader@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_AllowStartRequest */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_AllowStartRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_CopyToClipboard */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_CopyToClipboard(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_CreateBrowser */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_CreateBrowser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_ExecuteJavascript */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_ExecuteJavascript(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_Find */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_Find(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_Find@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_GetLinkAtPosition */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_GetLinkAtPosition(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_GoBack */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_GoBack(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_GoBack@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_GoForward */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_GoForward(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_GoForward@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_Init */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_JSDialogResponse */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_JSDialogResponse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_KeyChar */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_KeyChar(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_KeyChar@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_KeyDown */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_KeyDown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_KeyDown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_KeyUp */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_KeyUp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_KeyUp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_LoadURL */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_LoadURL(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_LoadURL@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_MouseDoubleClick */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_MouseDoubleClick(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_MouseDown */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_MouseDown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_MouseDown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_MouseMove */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_MouseMove(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_MouseMove@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_MouseUp */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_MouseUp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_MouseUp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_MouseWheel */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_MouseWheel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_MouseWheel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_OpenDeveloperTools */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_OpenDeveloperTools(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_PasteFromClipboard */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_PasteFromClipboard(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_Reload */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_Reload(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_Reload@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_RemoveBrowser */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_RemoveBrowser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetBackgroundMode */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetBackgroundMode(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetCookie */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetCookie(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetCookie@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetHorizontalScroll */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetHorizontalScroll(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetKeyFocus */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetKeyFocus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetPageScaleFactor */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetPageScaleFactor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetSize */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_SetVerticalScroll */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_SetVerticalScroll(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_Shutdown */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_Shutdown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_Shutdown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_StopFind */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_StopFind(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_StopFind@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_StopLoad */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_StopLoad(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_StopLoad@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTMLSurface_ViewSource */
__attribute__((naked)) void SteamAPI_ISteamHTMLSurface_ViewSource(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTMLSurface_ViewSource@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_CreateCookieContainer */
__attribute__((naked)) void SteamAPI_ISteamHTTP_CreateCookieContainer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_CreateCookieContainer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_CreateHTTPRequest */
__attribute__((naked)) void SteamAPI_ISteamHTTP_CreateHTTPRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_DeferHTTPRequest */
__attribute__((naked)) void SteamAPI_ISteamHTTP_DeferHTTPRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPResponseBodyData */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPResponseBodyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPResponseBodySize */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPResponseBodySize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData */
__attribute__((naked)) void SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_PrioritizeHTTPRequest */
__attribute__((naked)) void SteamAPI_ISteamHTTP_PrioritizeHTTPRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_ReleaseCookieContainer */
__attribute__((naked)) void SteamAPI_ISteamHTTP_ReleaseCookieContainer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_ReleaseHTTPRequest */
__attribute__((naked)) void SteamAPI_ISteamHTTP_ReleaseHTTPRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SendHTTPRequest */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SendHTTPRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SendHTTPRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetCookie */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetCookie(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetCookie@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestContextValue */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestContextValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo */
__attribute__((naked)) void SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_ActivateActionSet */
__attribute__((naked)) void SteamAPI_ISteamInput_ActivateActionSet(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_ActivateActionSet@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_ActivateActionSetLayer */
__attribute__((naked)) void SteamAPI_ISteamInput_ActivateActionSetLayer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_ActivateActionSetLayer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_BNewDataAvailable */
__attribute__((naked)) void SteamAPI_ISteamInput_BNewDataAvailable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_BNewDataAvailable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_BWaitForData */
__attribute__((naked)) void SteamAPI_ISteamInput_BWaitForData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_BWaitForData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_DeactivateActionSetLayer */
__attribute__((naked)) void SteamAPI_ISteamInput_DeactivateActionSetLayer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_DeactivateAllActionSetLayers */
__attribute__((naked)) void SteamAPI_ISteamInput_DeactivateAllActionSetLayers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_EnableActionEventCallbacks */
__attribute__((naked)) void SteamAPI_ISteamInput_EnableActionEventCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_EnableDeviceCallbacks */
__attribute__((naked)) void SteamAPI_ISteamInput_EnableDeviceCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetActionSetHandle */
__attribute__((naked)) void SteamAPI_ISteamInput_GetActionSetHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetActionSetHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetActiveActionSetLayers */
__attribute__((naked)) void SteamAPI_ISteamInput_GetActiveActionSetLayers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetAnalogActionData */
__attribute__((naked)) void SteamAPI_ISteamInput_GetAnalogActionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetAnalogActionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetAnalogActionHandle */
__attribute__((naked)) void SteamAPI_ISteamInput_GetAnalogActionHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetAnalogActionHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetAnalogActionOrigins */
__attribute__((naked)) void SteamAPI_ISteamInput_GetAnalogActionOrigins(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetConnectedControllers */
__attribute__((naked)) void SteamAPI_ISteamInput_GetConnectedControllers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetConnectedControllers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetControllerForGamepadIndex */
__attribute__((naked)) void SteamAPI_ISteamInput_GetControllerForGamepadIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetCurrentActionSet */
__attribute__((naked)) void SteamAPI_ISteamInput_GetCurrentActionSet(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetCurrentActionSet@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetDeviceBindingRevision */
__attribute__((naked)) void SteamAPI_ISteamInput_GetDeviceBindingRevision(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetDigitalActionData */
__attribute__((naked)) void SteamAPI_ISteamInput_GetDigitalActionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetDigitalActionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetDigitalActionHandle */
__attribute__((naked)) void SteamAPI_ISteamInput_GetDigitalActionHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetDigitalActionHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetDigitalActionOrigins */
__attribute__((naked)) void SteamAPI_ISteamInput_GetDigitalActionOrigins(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetGamepadIndexForController */
__attribute__((naked)) void SteamAPI_ISteamInput_GetGamepadIndexForController(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetGamepadIndexForController@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy */
__attribute__((naked)) void SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetGlyphForXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetGlyphForXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetInputTypeForHandle */
__attribute__((naked)) void SteamAPI_ISteamInput_GetInputTypeForHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetInputTypeForHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetMotionData */
__attribute__((naked)) void SteamAPI_ISteamInput_GetMotionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetMotionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetRemotePlaySessionID */
__attribute__((naked)) void SteamAPI_ISteamInput_GetRemotePlaySessionID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetSessionInputConfigurationSettings */
__attribute__((naked)) void SteamAPI_ISteamInput_GetSessionInputConfigurationSettings(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetStringForActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetStringForActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetStringForActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetStringForAnalogActionName */
__attribute__((naked)) void SteamAPI_ISteamInput_GetStringForAnalogActionName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetStringForDigitalActionName */
__attribute__((naked)) void SteamAPI_ISteamInput_GetStringForDigitalActionName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_GetStringForXboxOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_GetStringForXboxOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_Init */
__attribute__((naked)) void SteamAPI_ISteamInput_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_Legacy_TriggerHapticPulse */
__attribute__((naked)) void SteamAPI_ISteamInput_Legacy_TriggerHapticPulse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse */
__attribute__((naked)) void SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_RunFrame */
__attribute__((naked)) void SteamAPI_ISteamInput_RunFrame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_RunFrame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_SetDualSenseTriggerEffect */
__attribute__((naked)) void SteamAPI_ISteamInput_SetDualSenseTriggerEffect(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_SetInputActionManifestFilePath */
__attribute__((naked)) void SteamAPI_ISteamInput_SetInputActionManifestFilePath(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_SetLEDColor */
__attribute__((naked)) void SteamAPI_ISteamInput_SetLEDColor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_SetLEDColor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_ShowBindingPanel */
__attribute__((naked)) void SteamAPI_ISteamInput_ShowBindingPanel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_ShowBindingPanel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_Shutdown */
__attribute__((naked)) void SteamAPI_ISteamInput_Shutdown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_Shutdown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_StopAnalogActionMomentum */
__attribute__((naked)) void SteamAPI_ISteamInput_StopAnalogActionMomentum(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_TranslateActionOrigin */
__attribute__((naked)) void SteamAPI_ISteamInput_TranslateActionOrigin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_TranslateActionOrigin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_TriggerSimpleHapticEvent */
__attribute__((naked)) void SteamAPI_ISteamInput_TriggerSimpleHapticEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_TriggerVibration */
__attribute__((naked)) void SteamAPI_ISteamInput_TriggerVibration(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_TriggerVibration@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInput_TriggerVibrationExtended */
__attribute__((naked)) void SteamAPI_ISteamInput_TriggerVibrationExtended(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInput_TriggerVibrationExtended@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_AddPromoItem */
__attribute__((naked)) void SteamAPI_ISteamInventory_AddPromoItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_AddPromoItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_AddPromoItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_AddPromoItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_AddPromoItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_CheckResultSteamID */
__attribute__((naked)) void SteamAPI_ISteamInventory_CheckResultSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_CheckResultSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_ConsumeItem */
__attribute__((naked)) void SteamAPI_ISteamInventory_ConsumeItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_ConsumeItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_DeserializeResult */
__attribute__((naked)) void SteamAPI_ISteamInventory_DeserializeResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_DeserializeResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_DestroyResult */
__attribute__((naked)) void SteamAPI_ISteamInventory_DestroyResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_DestroyResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_ExchangeItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_ExchangeItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_ExchangeItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GenerateItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_GenerateItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GenerateItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetAllItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetAllItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetAllItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetItemDefinitionIDs */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetItemDefinitionIDs(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetItemDefinitionProperty */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetItemDefinitionProperty(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetItemPrice */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetItemPrice(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetItemPrice@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetItemsByID */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetItemsByID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetItemsByID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetItemsWithPrices */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetItemsWithPrices(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetItemsWithPrices@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetNumItemsWithPrices */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetNumItemsWithPrices(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetResultItemProperty */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetResultItemProperty(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetResultItemProperty@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetResultItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetResultItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetResultItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetResultStatus */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetResultStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetResultStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GetResultTimestamp */
__attribute__((naked)) void SteamAPI_ISteamInventory_GetResultTimestamp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GetResultTimestamp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_GrantPromoItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_GrantPromoItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_GrantPromoItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_InspectItem */
__attribute__((naked)) void SteamAPI_ISteamInventory_InspectItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_InspectItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_LoadItemDefinitions */
__attribute__((naked)) void SteamAPI_ISteamInventory_LoadItemDefinitions(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_LoadItemDefinitions@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_RemoveProperty */
__attribute__((naked)) void SteamAPI_ISteamInventory_RemoveProperty(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_RemoveProperty@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs */
__attribute__((naked)) void SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_RequestPrices */
__attribute__((naked)) void SteamAPI_ISteamInventory_RequestPrices(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_RequestPrices@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SendItemDropHeartbeat */
__attribute__((naked)) void SteamAPI_ISteamInventory_SendItemDropHeartbeat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SerializeResult */
__attribute__((naked)) void SteamAPI_ISteamInventory_SerializeResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SerializeResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SetPropertyBool */
__attribute__((naked)) void SteamAPI_ISteamInventory_SetPropertyBool(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SetPropertyBool@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SetPropertyFloat */
__attribute__((naked)) void SteamAPI_ISteamInventory_SetPropertyFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SetPropertyFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SetPropertyInt64 */
__attribute__((naked)) void SteamAPI_ISteamInventory_SetPropertyInt64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SetPropertyInt64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SetPropertyString */
__attribute__((naked)) void SteamAPI_ISteamInventory_SetPropertyString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SetPropertyString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_StartPurchase */
__attribute__((naked)) void SteamAPI_ISteamInventory_StartPurchase(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_StartPurchase@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_StartUpdateProperties */
__attribute__((naked)) void SteamAPI_ISteamInventory_StartUpdateProperties(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_StartUpdateProperties@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_SubmitUpdateProperties */
__attribute__((naked)) void SteamAPI_ISteamInventory_SubmitUpdateProperties(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_TradeItems */
__attribute__((naked)) void SteamAPI_ISteamInventory_TradeItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_TradeItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_TransferItemQuantity */
__attribute__((naked)) void SteamAPI_ISteamInventory_TransferItemQuantity(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_TransferItemQuantity@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamInventory_TriggerItemDrop */
__attribute__((naked)) void SteamAPI_ISteamInventory_TriggerItemDrop(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamInventory_TriggerItemDrop@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddFavoriteGame */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddFavoriteGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_CreateLobby */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_CreateLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_CreateLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_DeleteLobbyData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_DeleteLobbyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetFavoriteGame */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetFavoriteGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetFavoriteGameCount */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetFavoriteGameCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyByIndex */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyChatEntry */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyDataCount */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyDataCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyGameServer */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyGameServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyMemberData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetLobbyOwner */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetLobbyOwner(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_GetNumLobbyMembers */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_InviteUserToLobby */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_InviteUserToLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_JoinLobby */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_JoinLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_JoinLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_LeaveLobby */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_LeaveLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_LeaveLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingPingResponse_ServerResponded */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingPingResponse_ServerResponded(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_RemoveFavoriteGame */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_RemoveFavoriteGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_RequestLobbyData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_RequestLobbyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_RequestLobbyList */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_RequestLobbyList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SendLobbyChatMsg */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_CancelQuery */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_CancelQuery(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_CancelServerQuery */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_CancelServerQuery(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_GetServerCount */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_GetServerCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_GetServerDetails */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_GetServerDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_IsRefreshing */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_IsRefreshing(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_PingServer */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_PingServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_PingServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_PlayerDetails */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_PlayerDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RefreshQuery */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RefreshQuery(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RefreshServer */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RefreshServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_ReleaseRequest */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_ReleaseRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestInternetServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestInternetServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestLANServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestLANServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmakingServers_ServerRules */
__attribute__((naked)) void SteamAPI_ISteamMatchmakingServers_ServerRules(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmakingServers_ServerRules@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLinkedLobby */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLinkedLobby(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyGameServer */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyGameServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyJoinable */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyMemberData */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyMemberData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyOwner */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyOwner(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMatchmaking_SetLobbyType */
__attribute__((naked)) void SteamAPI_ISteamMatchmaking_SetLobbyType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMatchmaking_SetLobbyType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_BIsEnabled */
__attribute__((naked)) void SteamAPI_ISteamMusic_BIsEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_BIsEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_BIsPlaying */
__attribute__((naked)) void SteamAPI_ISteamMusic_BIsPlaying(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_BIsPlaying@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_GetPlaybackStatus */
__attribute__((naked)) void SteamAPI_ISteamMusic_GetPlaybackStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_GetPlaybackStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_GetVolume */
__attribute__((naked)) void SteamAPI_ISteamMusic_GetVolume(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_GetVolume@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_Pause */
__attribute__((naked)) void SteamAPI_ISteamMusic_Pause(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_Pause@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_Play */
__attribute__((naked)) void SteamAPI_ISteamMusic_Play(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_Play@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_PlayNext */
__attribute__((naked)) void SteamAPI_ISteamMusic_PlayNext(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_PlayNext@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_PlayPrevious */
__attribute__((naked)) void SteamAPI_ISteamMusic_PlayPrevious(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_PlayPrevious@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_BActivationSuccess */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_BActivationSuccess(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_CurrentEntryDidChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_CurrentEntryDidChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_CurrentEntryWillChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_CurrentEntryWillChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnableLooped */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnableLooped(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnableLooped@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnablePlaylists */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnablePlaylists(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnablePlayNext */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnablePlayNext(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnablePlayPrevious */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnablePlayPrevious(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnableQueue */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnableQueue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnableQueue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_EnableShuffled */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_EnableShuffled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_EnableShuffled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_PlaylistDidChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_PlaylistDidChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_PlaylistWillChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_PlaylistWillChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_QueueDidChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_QueueDidChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_QueueDidChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_QueueWillChange */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_QueueWillChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_QueueWillChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_ResetPlaylistEntries */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_ResetPlaylistEntries(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_ResetQueueEntries */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_ResetQueueEntries(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetDisplayName */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetDisplayName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetDisplayName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetPlaylistEntry */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetPlaylistEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_SetQueueEntry */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_SetQueueEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateLooped */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateLooped(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateLooped@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateShuffled */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateShuffled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusicRemote_UpdateVolume */
__attribute__((naked)) void SteamAPI_ISteamMusicRemote_UpdateVolume(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusicRemote_UpdateVolume@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamMusic_SetVolume */
__attribute__((naked)) void SteamAPI_ISteamMusic_SetVolume(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamMusic_SetVolume@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_AllowP2PPacketRelay */
__attribute__((naked)) void SteamAPI_ISteamNetworking_AllowP2PPacketRelay(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_CloseP2PChannelWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworking_CloseP2PChannelWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_CloseP2PSessionWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_CreateConnectionSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_CreateConnectionSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_CreateListenSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_CreateListenSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_CreateListenSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_CreateP2PConnectionSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_CreateP2PConnectionSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_DestroyListenSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_DestroyListenSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_DestroyListenSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_DestroySocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_DestroySocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_DestroySocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort */
__attribute__((naked)) void SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages */
__attribute__((naked)) void SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup */
__attribute__((naked)) void SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_GetListenSocketInfo */
__attribute__((naked)) void SteamAPI_ISteamNetworking_GetListenSocketInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_GetMaxPacketSize */
__attribute__((naked)) void SteamAPI_ISteamNetworking_GetMaxPacketSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_GetP2PSessionState */
__attribute__((naked)) void SteamAPI_ISteamNetworking_GetP2PSessionState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_GetP2PSessionState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_GetSocketConnectionType */
__attribute__((naked)) void SteamAPI_ISteamNetworking_GetSocketConnectionType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_GetSocketInfo */
__attribute__((naked)) void SteamAPI_ISteamNetworking_GetSocketInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_GetSocketInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_IsDataAvailable */
__attribute__((naked)) void SteamAPI_ISteamNetworking_IsDataAvailable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_IsDataAvailable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_IsDataAvailableOnSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_IsDataAvailableOnSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_IsP2PPacketAvailable */
__attribute__((naked)) void SteamAPI_ISteamNetworking_IsP2PPacketAvailable(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingMessages_SendMessageToUser */
__attribute__((naked)) void SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_ReadP2PPacket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_ReadP2PPacket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_ReadP2PPacket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_RetrieveData */
__attribute__((naked)) void SteamAPI_ISteamNetworking_RetrieveData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_RetrieveData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_RetrieveDataFromSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_RetrieveDataFromSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_SendDataOnSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_SendDataOnSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_SendDataOnSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworking_SendP2PPacket */
__attribute__((naked)) void SteamAPI_ISteamNetworking_SendP2PPacket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworking_SendP2PPacket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_AcceptConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_AcceptConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CloseConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CloseConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CloseListenSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CloseListenSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ConnectP2P */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ConnectP2P(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreatePollGroup */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreatePollGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_CreateSocketPair */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_CreateSocketPair(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_DestroyPollGroup */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_DestroyPollGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetCertificateRequest */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetCertificateRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetConnectionInfo */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetConnectionInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetConnectionName */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetConnectionName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetConnectionUserData */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetConnectionUserData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetFakeIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetIdentity */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetIdentity(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_InitAuthentication */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_InitAuthentication(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_ResetIdentity */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_ResetIdentity(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_RunCallbacks */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_RunCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SendMessages */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SendMessages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SendMessages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SendMessageToConnection */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SendMessageToConnection(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SetCertificate */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SetCertificate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SetConnectionName */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SetConnectionName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingSockets_SetConnectionUserData */
__attribute__((naked)) void SteamAPI_ISteamNetworkingSockets_SetConnectionUserData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_AllocateMessage */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_AllocateMessage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetConfigValue */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetConfigValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetPOPCount */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetPOPCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetPOPList */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetPOPList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_IsFakeIPv4(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_ParsePingLocationString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_ParsePingLocationString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetConfigValue */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetConfigValue(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString */
__attribute__((naked)) void SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsAppBlocked */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsAppBlocked(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsAppInBlockList */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsAppInBlockList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsFeatureBlocked */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsFeatureBlocked(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParentalSettings_BIsParentalLockLocked */
__attribute__((naked)) void SteamAPI_ISteamParentalSettings_BIsParentalLockLocked(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_CancelReservation */
__attribute__((naked)) void SteamAPI_ISteamParties_CancelReservation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_CancelReservation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_ChangeNumOpenSlots */
__attribute__((naked)) void SteamAPI_ISteamParties_ChangeNumOpenSlots(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_CreateBeacon */
__attribute__((naked)) void SteamAPI_ISteamParties_CreateBeacon(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_CreateBeacon@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_DestroyBeacon */
__attribute__((naked)) void SteamAPI_ISteamParties_DestroyBeacon(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_DestroyBeacon@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetAvailableBeaconLocations */
__attribute__((naked)) void SteamAPI_ISteamParties_GetAvailableBeaconLocations(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetBeaconByIndex */
__attribute__((naked)) void SteamAPI_ISteamParties_GetBeaconByIndex(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetBeaconByIndex@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetBeaconDetails */
__attribute__((naked)) void SteamAPI_ISteamParties_GetBeaconDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetBeaconDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetBeaconLocationData */
__attribute__((naked)) void SteamAPI_ISteamParties_GetBeaconLocationData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetBeaconLocationData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetNumActiveBeacons */
__attribute__((naked)) void SteamAPI_ISteamParties_GetNumActiveBeacons(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetNumActiveBeacons@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_GetNumAvailableBeaconLocations */
__attribute__((naked)) void SteamAPI_ISteamParties_GetNumAvailableBeaconLocations(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_JoinParty */
__attribute__((naked)) void SteamAPI_ISteamParties_JoinParty(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_JoinParty@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamParties_OnReservationCompleted */
__attribute__((naked)) void SteamAPI_ISteamParties_OnReservationCompleted(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamParties_OnReservationCompleted@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_BGetSessionClientResolution */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_BGetSessionClientResolution(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_GetSessionClientName */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_GetSessionClientName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_GetSessionCount */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_GetSessionCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_GetSessionCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_GetSessionID */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_GetSessionID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_GetSessionID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemotePlay_GetSessionSteamID */
__attribute__((naked)) void SteamAPI_ISteamRemotePlay_GetSessionSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_DeletePublishedFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_DeletePublishedFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EndFileWriteBatch */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EndFileWriteBatch(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileDelete */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileDelete(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileDelete@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileExists */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileExists(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileExists@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileForget */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileForget(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileForget@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FilePersisted */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FilePersisted(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FilePersisted@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileRead */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileRead(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileRead@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileReadAsync */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileReadAsync(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileShare */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileShare(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileShare@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWrite */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWrite(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWrite@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWriteAsync */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWriteAsync(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamClose */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWriteStreamClose(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetCachedUGCCount */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetCachedUGCCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetFileCount */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetFileCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetFileCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetFileNameAndSize */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetFileNameAndSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetFileSize */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetFileSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetFileSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetFileTimestamp */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetFileTimestamp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetLocalFileChange */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetLocalFileChange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetQuota */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetQuota(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetQuota@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetSyncPlatforms */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetSyncPlatforms(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetUGCDetails */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetUGCDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_PublishVideo */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_PublishVideo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_PublishVideo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_PublishWorkshopFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_PublishWorkshopFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_SetSyncPlatforms */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_SetSyncPlatforms(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_SubscribePublishedFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_SubscribePublishedFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UGCDownload */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UGCDownload(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UGCDownload@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UGCRead */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UGCRead(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UGCRead@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote */
__attribute__((naked)) void SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_AddScreenshotToLibrary */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_AddScreenshotToLibrary(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_HookScreenshots */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_HookScreenshots(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_HookScreenshots@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_IsScreenshotsHooked */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_IsScreenshotsHooked(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_SetLocation */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_SetLocation(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_SetLocation@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_TagPublishedFile */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_TagPublishedFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_TagPublishedFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_TagUser */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_TagUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_TagUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_TriggerScreenshot */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_TriggerScreenshot(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamScreenshots_WriteScreenshot */
__attribute__((naked)) void SteamAPI_ISteamScreenshots_WriteScreenshot(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamScreenshots_WriteScreenshot@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_AddGamePhaseTag */
__attribute__((naked)) void SteamAPI_ISteamTimeline_AddGamePhaseTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_AddRangeTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_AddRangeTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_ClearTimelineTooltip */
__attribute__((naked)) void SteamAPI_ISteamTimeline_ClearTimelineTooltip(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_DoesEventRecordingExist */
__attribute__((naked)) void SteamAPI_ISteamTimeline_DoesEventRecordingExist(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist */
__attribute__((naked)) void SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_EndGamePhase */
__attribute__((naked)) void SteamAPI_ISteamTimeline_EndGamePhase(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_EndGamePhase@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_EndRangeTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_EndRangeTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_OpenOverlayToGamePhase */
__attribute__((naked)) void SteamAPI_ISteamTimeline_OpenOverlayToGamePhase(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_RemoveTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_RemoveTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_SetGamePhaseAttribute */
__attribute__((naked)) void SteamAPI_ISteamTimeline_SetGamePhaseAttribute(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_SetGamePhaseID */
__attribute__((naked)) void SteamAPI_ISteamTimeline_SetGamePhaseID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_SetGamePhaseID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_SetTimelineGameMode */
__attribute__((naked)) void SteamAPI_ISteamTimeline_SetTimelineGameMode(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_SetTimelineTooltip */
__attribute__((naked)) void SteamAPI_ISteamTimeline_SetTimelineTooltip(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_StartGamePhase */
__attribute__((naked)) void SteamAPI_ISteamTimeline_StartGamePhase(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_StartGamePhase@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_StartRangeTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_StartRangeTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent */
__attribute__((naked)) void SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddAppDependency */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddAppDependency(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddAppDependency@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddContentDescriptor */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddContentDescriptor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddContentDescriptor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddDependency */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddDependency(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddDependency@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddExcludedTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddExcludedTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddExcludedTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddItemKeyValueTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddItemKeyValueTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddItemPreviewFile */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddItemPreviewFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddItemPreviewFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddItemPreviewVideo */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddItemPreviewVideo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddItemToFavorites */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddItemToFavorites(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddItemToFavorites@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddRequiredKeyValueTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddRequiredKeyValueTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddRequiredTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddRequiredTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddRequiredTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_AddRequiredTagGroup */
__attribute__((naked)) void SteamAPI_ISteamUGC_AddRequiredTagGroup(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_BInitWorkshopForGameServer */
__attribute__((naked)) void SteamAPI_ISteamUGC_BInitWorkshopForGameServer(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_CreateItem */
__attribute__((naked)) void SteamAPI_ISteamUGC_CreateItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_CreateItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor */
__attribute__((naked)) void SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage */
__attribute__((naked)) void SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest */
__attribute__((naked)) void SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_CreateQueryUserUGCRequest */
__attribute__((naked)) void SteamAPI_ISteamUGC_CreateQueryUserUGCRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_DeleteItem */
__attribute__((naked)) void SteamAPI_ISteamUGC_DeleteItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_DeleteItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_DownloadItem */
__attribute__((naked)) void SteamAPI_ISteamUGC_DownloadItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_DownloadItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetAppDependencies */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetAppDependencies(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetAppDependencies@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetItemDownloadInfo */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetItemDownloadInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetItemInstallInfo */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetItemInstallInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetItemInstallInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetItemState */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetItemState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetItemState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetItemUpdateProgress */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetItemUpdateProgress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetNumSubscribedItems */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetNumSubscribedItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetNumSupportedGameVersions */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetNumSupportedGameVersions(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCChildren */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCChildren(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCMetadata */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCMetadata(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCNumTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCNumTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCPreviewURL */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCPreviewURL(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCResult */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCStatistic */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCStatistic(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetSubscribedItems */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetSubscribedItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetSubscribedItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetSupportedGameVersionData */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetSupportedGameVersionData(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetUserItemVote */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetUserItemVote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetUserItemVote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_GetWorkshopEULAStatus */
__attribute__((naked)) void SteamAPI_ISteamUGC_GetWorkshopEULAStatus(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_ReleaseQueryUGCRequest */
__attribute__((naked)) void SteamAPI_ISteamUGC_ReleaseQueryUGCRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveAppDependency */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveAppDependency(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveAppDependency@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveContentDescriptor */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveContentDescriptor(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveDependency */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveDependency(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveDependency@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveItemFromFavorites */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveItemFromFavorites(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveItemKeyValueTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveItemKeyValueTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RemoveItemPreview */
__attribute__((naked)) void SteamAPI_ISteamUGC_RemoveItemPreview(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RemoveItemPreview@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_RequestUGCDetails */
__attribute__((naked)) void SteamAPI_ISteamUGC_RequestUGCDetails(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_RequestUGCDetails@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SendQueryUGCRequest */
__attribute__((naked)) void SteamAPI_ISteamUGC_SendQueryUGCRequest(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetAdminQuery */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetAdminQuery(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetAdminQuery@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetAllowCachedResponse */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetAllowCachedResponse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetAllowLegacyUpload */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetAllowLegacyUpload(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetCloudFileNameFilter */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetCloudFileNameFilter(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemContent */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemContent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemContent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemDescription */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemDescription(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemDescription@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemMetadata */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemMetadata(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemMetadata@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemPreview */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemPreview(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemPreview@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemTitle */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemTitle(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemTitle@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemUpdateLanguage */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemUpdateLanguage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetItemVisibility */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetItemVisibility(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetItemVisibility@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetLanguage */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetLanguage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetLanguage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetMatchAnyTag */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetMatchAnyTag(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetMatchAnyTag@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetRankedByTrendDays */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetRankedByTrendDays(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetRequiredGameVersions */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetRequiredGameVersions(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnAdditionalPreviews */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnAdditionalPreviews(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnChildren */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnChildren(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnChildren@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnKeyValueTags */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnKeyValueTags(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnLongDescription */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnLongDescription(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnLongDescription@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnMetadata */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnMetadata(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnMetadata@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnOnlyIDs */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnOnlyIDs(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnPlaytimeStats */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnPlaytimeStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetReturnTotalOnly */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetReturnTotalOnly(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetSearchText */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetSearchText(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetSearchText@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetTimeCreatedDateRange */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetTimeCreatedDateRange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetTimeUpdatedDateRange */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetTimeUpdatedDateRange(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SetUserItemVote */
__attribute__((naked)) void SteamAPI_ISteamUGC_SetUserItemVote(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SetUserItemVote@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_ShowWorkshopEULA */
__attribute__((naked)) void SteamAPI_ISteamUGC_ShowWorkshopEULA(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_StartItemUpdate */
__attribute__((naked)) void SteamAPI_ISteamUGC_StartItemUpdate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_StartItemUpdate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_StartPlaytimeTracking */
__attribute__((naked)) void SteamAPI_ISteamUGC_StartPlaytimeTracking(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_StopPlaytimeTracking */
__attribute__((naked)) void SteamAPI_ISteamUGC_StopPlaytimeTracking(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems */
__attribute__((naked)) void SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SubmitItemUpdate */
__attribute__((naked)) void SteamAPI_ISteamUGC_SubmitItemUpdate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SubmitItemUpdate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SubscribeItem */
__attribute__((naked)) void SteamAPI_ISteamUGC_SubscribeItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SubscribeItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_SuspendDownloads */
__attribute__((naked)) void SteamAPI_ISteamUGC_SuspendDownloads(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_SuspendDownloads@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_UnsubscribeItem */
__attribute__((naked)) void SteamAPI_ISteamUGC_UnsubscribeItem(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_UnsubscribeItem@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_UpdateItemPreviewFile */
__attribute__((naked)) void SteamAPI_ISteamUGC_UpdateItemPreviewFile(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUGC_UpdateItemPreviewVideo */
__attribute__((naked)) void SteamAPI_ISteamUGC_UpdateItemPreviewVideo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_AdvertiseGame */
__attribute__((naked)) void SteamAPI_ISteamUser_AdvertiseGame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_AdvertiseGame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BeginAuthSession */
__attribute__((naked)) void SteamAPI_ISteamUser_BeginAuthSession(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BeginAuthSession@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BIsBehindNAT */
__attribute__((naked)) void SteamAPI_ISteamUser_BIsBehindNAT(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BIsBehindNAT@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BIsPhoneIdentifying */
__attribute__((naked)) void SteamAPI_ISteamUser_BIsPhoneIdentifying(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BIsPhoneRequiringVerification */
__attribute__((naked)) void SteamAPI_ISteamUser_BIsPhoneRequiringVerification(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BIsPhoneVerified */
__attribute__((naked)) void SteamAPI_ISteamUser_BIsPhoneVerified(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BIsPhoneVerified@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BIsTwoFactorEnabled */
__attribute__((naked)) void SteamAPI_ISteamUser_BIsTwoFactorEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BLoggedOn */
__attribute__((naked)) void SteamAPI_ISteamUser_BLoggedOn(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BLoggedOn@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_BSetDurationControlOnlineState */
__attribute__((naked)) void SteamAPI_ISteamUser_BSetDurationControlOnlineState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_CancelAuthTicket */
__attribute__((naked)) void SteamAPI_ISteamUser_CancelAuthTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_CancelAuthTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_DecompressVoice */
__attribute__((naked)) void SteamAPI_ISteamUser_DecompressVoice(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_DecompressVoice@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_EndAuthSession */
__attribute__((naked)) void SteamAPI_ISteamUser_EndAuthSession(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_EndAuthSession@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetAuthSessionTicket */
__attribute__((naked)) void SteamAPI_ISteamUser_GetAuthSessionTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetAuthSessionTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetAuthTicketForWebApi */
__attribute__((naked)) void SteamAPI_ISteamUser_GetAuthTicketForWebApi(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetAvailableVoice */
__attribute__((naked)) void SteamAPI_ISteamUser_GetAvailableVoice(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetAvailableVoice@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetDurationControl */
__attribute__((naked)) void SteamAPI_ISteamUser_GetDurationControl(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetDurationControl@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetEncryptedAppTicket */
__attribute__((naked)) void SteamAPI_ISteamUser_GetEncryptedAppTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetGameBadgeLevel */
__attribute__((naked)) void SteamAPI_ISteamUser_GetGameBadgeLevel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetGameBadgeLevel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetHSteamUser */
__attribute__((naked)) void SteamAPI_ISteamUser_GetHSteamUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetHSteamUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetMarketEligibility */
__attribute__((naked)) void SteamAPI_ISteamUser_GetMarketEligibility(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetMarketEligibility@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetPlayerSteamLevel */
__attribute__((naked)) void SteamAPI_ISteamUser_GetPlayerSteamLevel(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetSteamID */
__attribute__((naked)) void SteamAPI_ISteamUser_GetSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetUserDataFolder */
__attribute__((naked)) void SteamAPI_ISteamUser_GetUserDataFolder(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetUserDataFolder@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetVoice */
__attribute__((naked)) void SteamAPI_ISteamUser_GetVoice(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetVoice@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_GetVoiceOptimalSampleRate */
__attribute__((naked)) void SteamAPI_ISteamUser_GetVoiceOptimalSampleRate(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED */
__attribute__((naked)) void SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_RequestEncryptedAppTicket */
__attribute__((naked)) void SteamAPI_ISteamUser_RequestEncryptedAppTicket(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_RequestStoreAuthURL */
__attribute__((naked)) void SteamAPI_ISteamUser_RequestStoreAuthURL(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_RequestStoreAuthURL@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_StartVoiceRecording */
__attribute__((naked)) void SteamAPI_ISteamUser_StartVoiceRecording(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_StartVoiceRecording@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_AttachLeaderboardUGC */
__attribute__((naked)) void SteamAPI_ISteamUserStats_AttachLeaderboardUGC(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_ClearAchievement */
__attribute__((naked)) void SteamAPI_ISteamUserStats_ClearAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_ClearAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_DownloadLeaderboardEntries */
__attribute__((naked)) void SteamAPI_ISteamUserStats_DownloadLeaderboardEntries(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers */
__attribute__((naked)) void SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_FindLeaderboard */
__attribute__((naked)) void SteamAPI_ISteamUserStats_FindLeaderboard(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_FindLeaderboard@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_FindOrCreateLeaderboard */
__attribute__((naked)) void SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievement */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementAchievedPercent */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementAchievedPercent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementIcon */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementIcon(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementIcon@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementName */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetGlobalStatDouble */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetGlobalStatDouble(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetGlobalStatInt64 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetGlobalStatInt64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetLeaderboardDisplayType */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetLeaderboardDisplayType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetLeaderboardEntryCount */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetLeaderboardEntryCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetLeaderboardName */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetLeaderboardName(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetLeaderboardName@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetLeaderboardSortMethod */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetLeaderboardSortMethod(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetNumAchievements */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetNumAchievements(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetNumAchievements@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetStatFloat */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetStatFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetStatFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetStatInt32 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetStatInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetStatInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetUserAchievement */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetUserAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetUserAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetUserStatFloat */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetUserStatFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetUserStatFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_GetUserStatInt32 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_GetUserStatInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_GetUserStatInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_IndicateAchievementProgress */
__attribute__((naked)) void SteamAPI_ISteamUserStats_IndicateAchievementProgress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages */
__attribute__((naked)) void SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_RequestGlobalStats */
__attribute__((naked)) void SteamAPI_ISteamUserStats_RequestGlobalStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_RequestGlobalStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_RequestUserStats */
__attribute__((naked)) void SteamAPI_ISteamUserStats_RequestUserStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_RequestUserStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_ResetAllStats */
__attribute__((naked)) void SteamAPI_ISteamUserStats_ResetAllStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_ResetAllStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_SetAchievement */
__attribute__((naked)) void SteamAPI_ISteamUserStats_SetAchievement(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_SetAchievement@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_SetStatFloat */
__attribute__((naked)) void SteamAPI_ISteamUserStats_SetStatFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_SetStatFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_SetStatInt32 */
__attribute__((naked)) void SteamAPI_ISteamUserStats_SetStatInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_SetStatInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_StoreStats */
__attribute__((naked)) void SteamAPI_ISteamUserStats_StoreStats(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_StoreStats@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_UpdateAvgRateStat */
__attribute__((naked)) void SteamAPI_ISteamUserStats_UpdateAvgRateStat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUserStats_UploadLeaderboardScore */
__attribute__((naked)) void SteamAPI_ISteamUserStats_UploadLeaderboardScore(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_StopVoiceRecording */
__attribute__((naked)) void SteamAPI_ISteamUser_StopVoiceRecording(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_StopVoiceRecording@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED */
__attribute__((naked)) void SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUser_TrackAppUsageEvent */
__attribute__((naked)) void SteamAPI_ISteamUser_TrackAppUsageEvent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUser_TrackAppUsageEvent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_BOverlayNeedsPresent */
__attribute__((naked)) void SteamAPI_ISteamUtils_BOverlayNeedsPresent(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_CheckFileSignature */
__attribute__((naked)) void SteamAPI_ISteamUtils_CheckFileSignature(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_CheckFileSignature@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput */
__attribute__((naked)) void SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_DismissGamepadTextInput */
__attribute__((naked)) void SteamAPI_ISteamUtils_DismissGamepadTextInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_FilterText */
__attribute__((naked)) void SteamAPI_ISteamUtils_FilterText(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_FilterText@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetAPICallFailureReason */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetAPICallFailureReason(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetAPICallResult */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetAPICallResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetAPICallResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetAppID */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetAppID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetAppID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetConnectedUniverse */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetConnectedUniverse(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetConnectedUniverse@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetCurrentBatteryPower */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetCurrentBatteryPower(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetEnteredGamepadTextInput */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetEnteredGamepadTextInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetEnteredGamepadTextLength */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetEnteredGamepadTextLength(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetImageRGBA */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetImageRGBA(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetImageRGBA@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetImageSize */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetImageSize(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetImageSize@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetIPCCallCount */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetIPCCallCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetIPCCallCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetIPCountry */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetIPCountry(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetIPCountry@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetIPv6ConnectivityState */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetIPv6ConnectivityState(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetSecondsSinceAppActive */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetSecondsSinceAppActive(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetSecondsSinceComputerActive */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetSecondsSinceComputerActive(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetServerRealTime */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetServerRealTime(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetServerRealTime@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_GetSteamUILanguage */
__attribute__((naked)) void SteamAPI_ISteamUtils_GetSteamUILanguage(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_GetSteamUILanguage@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_InitFilterText */
__attribute__((naked)) void SteamAPI_ISteamUtils_InitFilterText(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_InitFilterText@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsAPICallCompleted */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsAPICallCompleted(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsAPICallCompleted@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsOverlayEnabled */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsOverlayEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsOverlayEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsSteamChinaLauncher */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsSteamChinaLauncher(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsSteamInBigPictureMode */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsSteamInBigPictureMode(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsSteamRunningInVR */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsSteamRunningInVR(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled */
__attribute__((naked)) void SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_SetGameLauncherMode */
__attribute__((naked)) void SteamAPI_ISteamUtils_SetGameLauncherMode(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_SetGameLauncherMode@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_SetOverlayNotificationInset */
__attribute__((naked)) void SteamAPI_ISteamUtils_SetOverlayNotificationInset(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_SetOverlayNotificationPosition */
__attribute__((naked)) void SteamAPI_ISteamUtils_SetOverlayNotificationPosition(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled */
__attribute__((naked)) void SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_SetWarningMessageHook */
__attribute__((naked)) void SteamAPI_ISteamUtils_SetWarningMessageHook(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_SetWarningMessageHook@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput */
__attribute__((naked)) void SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_ShowGamepadTextInput */
__attribute__((naked)) void SteamAPI_ISteamUtils_ShowGamepadTextInput(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamUtils_StartVRDashboard */
__attribute__((naked)) void SteamAPI_ISteamUtils_StartVRDashboard(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamUtils_StartVRDashboard@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamVideo_GetOPFSettings */
__attribute__((naked)) void SteamAPI_ISteamVideo_GetOPFSettings(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamVideo_GetOPFSettings@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamVideo_GetOPFStringForApp */
__attribute__((naked)) void SteamAPI_ISteamVideo_GetOPFStringForApp(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamVideo_GetOPFStringForApp@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamVideo_GetVideoURL */
__attribute__((naked)) void SteamAPI_ISteamVideo_GetVideoURL(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamVideo_GetVideoURL@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ISteamVideo_IsBroadcasting */
__attribute__((naked)) void SteamAPI_ISteamVideo_IsBroadcasting(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ISteamVideo_IsBroadcasting@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ManualDispatch_FreeLastCallback */
__attribute__((naked)) void SteamAPI_ManualDispatch_FreeLastCallback(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ManualDispatch_FreeLastCallback@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ManualDispatch_GetAPICallResult */
__attribute__((naked)) void SteamAPI_ManualDispatch_GetAPICallResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ManualDispatch_GetAPICallResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ManualDispatch_GetNextCallback */
__attribute__((naked)) void SteamAPI_ManualDispatch_GetNextCallback(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ManualDispatch_GetNextCallback@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ManualDispatch_Init */
__attribute__((naked)) void SteamAPI_ManualDispatch_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ManualDispatch_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ManualDispatch_RunFrame */
__attribute__((naked)) void SteamAPI_ManualDispatch_RunFrame(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ManualDispatch_RunFrame@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_MatchMakingKeyValuePair_t_Construct */
__attribute__((naked)) void SteamAPI_MatchMakingKeyValuePair_t_Construct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_RegisterCallback */
__attribute__((naked)) void SteamAPI_RegisterCallback(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_RegisterCallback@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_RegisterCallResult */
__attribute__((naked)) void SteamAPI_RegisterCallResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_RegisterCallResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_ReleaseCurrentThreadMemory */
__attribute__((naked)) void SteamAPI_ReleaseCurrentThreadMemory(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_ReleaseCurrentThreadMemory@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_RestartAppIfNecessary */
__attribute__((naked)) void SteamAPI_RestartAppIfNecessary(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_RestartAppIfNecessary@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_RunCallbacks */
__attribute__((naked)) void SteamAPI_RunCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_RunCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_Assign */
__attribute__((naked)) void SteamAPI_servernetadr_t_Assign(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_Assign@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_Construct */
__attribute__((naked)) void SteamAPI_servernetadr_t_Construct(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_Construct@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_GetConnectionAddressString */
__attribute__((naked)) void SteamAPI_servernetadr_t_GetConnectionAddressString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_GetConnectionAddressString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_GetConnectionPort */
__attribute__((naked)) void SteamAPI_servernetadr_t_GetConnectionPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_GetConnectionPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_GetIP */
__attribute__((naked)) void SteamAPI_servernetadr_t_GetIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_GetIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_GetQueryAddressString */
__attribute__((naked)) void SteamAPI_servernetadr_t_GetQueryAddressString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_GetQueryAddressString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_GetQueryPort */
__attribute__((naked)) void SteamAPI_servernetadr_t_GetQueryPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_GetQueryPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_Init */
__attribute__((naked)) void SteamAPI_servernetadr_t_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_IsLessThan */
__attribute__((naked)) void SteamAPI_servernetadr_t_IsLessThan(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_IsLessThan@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_SetConnectionPort */
__attribute__((naked)) void SteamAPI_servernetadr_t_SetConnectionPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_SetConnectionPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_SetIP */
__attribute__((naked)) void SteamAPI_servernetadr_t_SetIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_SetIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_servernetadr_t_SetQueryPort */
__attribute__((naked)) void SteamAPI_servernetadr_t_SetQueryPort(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_servernetadr_t_SetQueryPort@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SetBreakpadAppID */
__attribute__((naked)) void SteamAPI_SetBreakpadAppID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SetBreakpadAppID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SetMiniDumpComment */
__attribute__((naked)) void SteamAPI_SetMiniDumpComment(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SetMiniDumpComment@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SetTryCatchCallbacks */
__attribute__((naked)) void SteamAPI_SetTryCatchCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SetTryCatchCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_Shutdown */
__attribute__((naked)) void SteamAPI_Shutdown(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_Shutdown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamApps_v008 */
__attribute__((naked)) void SteamAPI_SteamApps_v008(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamApps_v008@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamController_v008 */
__attribute__((naked)) void SteamAPI_SteamController_v008(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamController_v008@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamDatagramHostedAddress_Clear */
__attribute__((naked)) void SteamAPI_SteamDatagramHostedAddress_Clear(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamDatagramHostedAddress_Clear@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamDatagramHostedAddress_GetPopID */
__attribute__((naked)) void SteamAPI_SteamDatagramHostedAddress_GetPopID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamDatagramHostedAddress_SetDevAddress */
__attribute__((naked)) void SteamAPI_SteamDatagramHostedAddress_SetDevAddress(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamFriends_v017 */
__attribute__((naked)) void SteamAPI_SteamFriends_v017(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamFriends_v017@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameSearch_v001 */
__attribute__((naked)) void SteamAPI_SteamGameSearch_v001(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameSearch_v001@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerHTTP_v003 */
__attribute__((naked)) void SteamAPI_SteamGameServerHTTP_v003(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerHTTP_v003@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerInventory_v003 */
__attribute__((naked)) void SteamAPI_SteamGameServerInventory_v003(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerInventory_v003@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 */
__attribute__((naked)) void SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 */
__attribute__((naked)) void SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerNetworking_v006 */
__attribute__((naked)) void SteamAPI_SteamGameServerNetworking_v006(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerNetworking_v006@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerStats_v001 */
__attribute__((naked)) void SteamAPI_SteamGameServerStats_v001(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerStats_v001@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerUGC_v020 */
__attribute__((naked)) void SteamAPI_SteamGameServerUGC_v020(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerUGC_v020@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServerUtils_v010 */
__attribute__((naked)) void SteamAPI_SteamGameServerUtils_v010(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServerUtils_v010@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamGameServer_v015 */
__attribute__((naked)) void SteamAPI_SteamGameServer_v015(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamGameServer_v015@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamHTMLSurface_v005 */
__attribute__((naked)) void SteamAPI_SteamHTMLSurface_v005(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamHTMLSurface_v005@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamHTTP_v003 */
__attribute__((naked)) void SteamAPI_SteamHTTP_v003(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamHTTP_v003@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamInput_v006 */
__attribute__((naked)) void SteamAPI_SteamInput_v006(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamInput_v006@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamInventory_v003 */
__attribute__((naked)) void SteamAPI_SteamInventory_v003(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamInventory_v003@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamIPAddress_t_IsSet */
__attribute__((naked)) void SteamAPI_SteamIPAddress_t_IsSet(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamIPAddress_t_IsSet@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamMatchmakingServers_v002 */
__attribute__((naked)) void SteamAPI_SteamMatchmakingServers_v002(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamMatchmakingServers_v002@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamMatchmaking_v009 */
__attribute__((naked)) void SteamAPI_SteamMatchmaking_v009(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamMatchmaking_v009@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamMusicRemote_v001 */
__attribute__((naked)) void SteamAPI_SteamMusicRemote_v001(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamMusicRemote_v001@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamMusic_v001 */
__attribute__((naked)) void SteamAPI_SteamMusic_v001(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamMusic_v001@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingConfigValue_t_SetFloat */
__attribute__((naked)) void SteamAPI_SteamNetworkingConfigValue_t_SetFloat(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingConfigValue_t_SetInt32 */
__attribute__((naked)) void SteamAPI_SteamNetworkingConfigValue_t_SetInt32(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingConfigValue_t_SetInt64 */
__attribute__((naked)) void SteamAPI_SteamNetworkingConfigValue_t_SetInt64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingConfigValue_t_SetPtr */
__attribute__((naked)) void SteamAPI_SteamNetworkingConfigValue_t_SetPtr(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingConfigValue_t_SetString */
__attribute__((naked)) void SteamAPI_SteamNetworkingConfigValue_t_SetString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_Clear */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_Clear(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_Clear@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetFakeIPType */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetFakeIPType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetGenericBytes */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetGenericBytes(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetGenericString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetGenericString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetIPAddr */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetIPAddr(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetIPv4 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetIPv4(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetPSNID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetPSNID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetSteamID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetSteamID64 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetSteamID64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_IsEqualTo */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_IsEqualTo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_IsFakeIP */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_IsFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_IsInvalid */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_IsInvalid(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_IsLocalHost */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_IsLocalHost(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_ParseString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_ParseString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_ParseString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetGenericBytes */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetGenericBytes(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetGenericString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetGenericString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetIPAddr */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetIPAddr(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetIPv4Addr */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetIPv4Addr(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetLocalHost */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetLocalHost(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetPSNID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetPSNID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetSteamID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetSteamID64 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetSteamID64(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIdentity_ToString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIdentity_ToString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIdentity_ToString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_Clear */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_Clear(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_Clear@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_GetFakeIPType */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_GetFakeIPType(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_GetIPv4 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_GetIPv4(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_IsEqualTo */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_IsEqualTo(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_IsFakeIP */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_IsFakeIP(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_IsIPv4 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_IsIPv4(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_IsLocalHost */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_IsLocalHost(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_ParseString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_ParseString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_ParseString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_SetIPv4 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_SetIPv4(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_SetIPv6 */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_SetIPv6(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingIPAddr_ToString */
__attribute__((naked)) void SteamAPI_SteamNetworkingIPAddr_ToString(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingIPAddr_ToString@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingMessages_SteamAPI_v002 */
__attribute__((naked)) void SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingMessage_t_Release */
__attribute__((naked)) void SteamAPI_SteamNetworkingMessage_t_Release(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingMessage_t_Release@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingSockets_SteamAPI_v012 */
__attribute__((naked)) void SteamAPI_SteamNetworkingSockets_SteamAPI_v012(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworkingUtils_SteamAPI_v004 */
__attribute__((naked)) void SteamAPI_SteamNetworkingUtils_SteamAPI_v004(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamNetworking_v006 */
__attribute__((naked)) void SteamAPI_SteamNetworking_v006(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamNetworking_v006@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamParentalSettings_v001 */
__attribute__((naked)) void SteamAPI_SteamParentalSettings_v001(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamParentalSettings_v001@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamParties_v002 */
__attribute__((naked)) void SteamAPI_SteamParties_v002(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamParties_v002@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamRemotePlay_v002 */
__attribute__((naked)) void SteamAPI_SteamRemotePlay_v002(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamRemotePlay_v002@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamRemoteStorage_v016 */
__attribute__((naked)) void SteamAPI_SteamRemoteStorage_v016(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamRemoteStorage_v016@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamScreenshots_v003 */
__attribute__((naked)) void SteamAPI_SteamScreenshots_v003(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamScreenshots_v003@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamTimeline_v004 */
__attribute__((naked)) void SteamAPI_SteamTimeline_v004(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamTimeline_v004@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamUGC_v020 */
__attribute__((naked)) void SteamAPI_SteamUGC_v020(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamUGC_v020@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamUserStats_v013 */
__attribute__((naked)) void SteamAPI_SteamUserStats_v013(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamUserStats_v013@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamUser_v023 */
__attribute__((naked)) void SteamAPI_SteamUser_v023(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamUser_v023@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamUtils_v010 */
__attribute__((naked)) void SteamAPI_SteamUtils_v010(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamUtils_v010@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_SteamVideo_v007 */
__attribute__((naked)) void SteamAPI_SteamVideo_v007(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_SteamVideo_v007@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_UnregisterCallback */
__attribute__((naked)) void SteamAPI_UnregisterCallback(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_UnregisterCallback@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_UnregisterCallResult */
__attribute__((naked)) void SteamAPI_UnregisterCallResult(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_UnregisterCallResult@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_UseBreakpadCrashHandler */
__attribute__((naked)) void SteamAPI_UseBreakpadCrashHandler(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_UseBreakpadCrashHandler@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamAPI_WriteMiniDump */
__attribute__((naked)) void SteamAPI_WriteMiniDump(void) {
    __asm__ volatile (
        "movq _fwd_SteamAPI_WriteMiniDump@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamClient */
__attribute__((naked)) void SteamClient(void) {
    __asm__ volatile (
        "movq _fwd_SteamClient@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_BSecure */
__attribute__((naked)) void SteamGameServer_BSecure(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_BSecure@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_GetHSteamPipe */
__attribute__((naked)) void SteamGameServer_GetHSteamPipe(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_GetHSteamPipe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_GetHSteamUser */
__attribute__((naked)) void SteamGameServer_GetHSteamUser(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_GetHSteamUser@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_GetIPCCallCount */
__attribute__((naked)) void SteamGameServer_GetIPCCallCount(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_GetIPCCallCount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_GetSteamID */
__attribute__((naked)) void SteamGameServer_GetSteamID(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_GetSteamID@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_InitSafe */
__attribute__((naked)) void SteamGameServer_InitSafe(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_InitSafe@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_RunCallbacks */
__attribute__((naked)) void SteamGameServer_RunCallbacks(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_RunCallbacks@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamGameServer_Shutdown */
__attribute__((naked)) void SteamGameServer_Shutdown(void) {
    __asm__ volatile (
        "movq _fwd_SteamGameServer_Shutdown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_ContextInit */
__attribute__((naked)) void SteamInternal_ContextInit(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_ContextInit@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_CreateInterface */
__attribute__((naked)) void SteamInternal_CreateInterface(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_CreateInterface@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_FindOrCreateGameServerInterface */
__attribute__((naked)) void SteamInternal_FindOrCreateGameServerInterface(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_FindOrCreateGameServerInterface@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_FindOrCreateUserInterface */
__attribute__((naked)) void SteamInternal_FindOrCreateUserInterface(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_FindOrCreateUserInterface@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_GameServer_Init_V2 */
__attribute__((naked)) void SteamInternal_GameServer_Init_V2(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_GameServer_Init_V2@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamInternal_SteamAPI_Init */
__attribute__((naked)) void SteamInternal_SteamAPI_Init(void) {
    __asm__ volatile (
        "movq _fwd_SteamInternal_SteamAPI_Init@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* SteamRealPath */
__attribute__((naked)) void SteamRealPath(void) {
    __asm__ volatile (
        "movq _fwd_SteamRealPath@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_access */
__attribute__((naked)) void __wrap_access(void) {
    __asm__ volatile (
        "movq _fwd___wrap_access@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_chdir */
__attribute__((naked)) void __wrap_chdir(void) {
    __asm__ volatile (
        "movq _fwd___wrap_chdir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_chmod */
__attribute__((naked)) void __wrap_chmod(void) {
    __asm__ volatile (
        "movq _fwd___wrap_chmod@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_chown */
__attribute__((naked)) void __wrap_chown(void) {
    __asm__ volatile (
        "movq _fwd___wrap_chown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_dlmopen */
__attribute__((naked)) void __wrap_dlmopen(void) {
    __asm__ volatile (
        "movq _fwd___wrap_dlmopen@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_dlopen */
__attribute__((naked)) void __wrap_dlopen(void) {
    __asm__ volatile (
        "movq _fwd___wrap_dlopen@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_fopen */
__attribute__((naked)) void __wrap_fopen(void) {
    __asm__ volatile (
        "movq _fwd___wrap_fopen@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_fopen64 */
__attribute__((naked)) void __wrap_fopen64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_fopen64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_freopen */
__attribute__((naked)) void __wrap_freopen(void) {
    __asm__ volatile (
        "movq _fwd___wrap_freopen@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_lchown */
__attribute__((naked)) void __wrap_lchown(void) {
    __asm__ volatile (
        "movq _fwd___wrap_lchown@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_link */
__attribute__((naked)) void __wrap_link(void) {
    __asm__ volatile (
        "movq _fwd___wrap_link@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_lstat */
__attribute__((naked)) void __wrap_lstat(void) {
    __asm__ volatile (
        "movq _fwd___wrap_lstat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_lstat64 */
__attribute__((naked)) void __wrap_lstat64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_lstat64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap___lxstat */
__attribute__((naked)) void __wrap___lxstat(void) {
    __asm__ volatile (
        "movq _fwd___wrap___lxstat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap___lxstat64 */
__attribute__((naked)) void __wrap___lxstat64(void) {
    __asm__ volatile (
        "movq _fwd___wrap___lxstat64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_mkdir */
__attribute__((naked)) void __wrap_mkdir(void) {
    __asm__ volatile (
        "movq _fwd___wrap_mkdir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_mkfifo */
__attribute__((naked)) void __wrap_mkfifo(void) {
    __asm__ volatile (
        "movq _fwd___wrap_mkfifo@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_mknod */
__attribute__((naked)) void __wrap_mknod(void) {
    __asm__ volatile (
        "movq _fwd___wrap_mknod@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_mount */
__attribute__((naked)) void __wrap_mount(void) {
    __asm__ volatile (
        "movq _fwd___wrap_mount@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_open */
__attribute__((naked)) void __wrap_open(void) {
    __asm__ volatile (
        "movq _fwd___wrap_open@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_open64 */
__attribute__((naked)) void __wrap_open64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_open64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_opendir */
__attribute__((naked)) void __wrap_opendir(void) {
    __asm__ volatile (
        "movq _fwd___wrap_opendir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_rename */
__attribute__((naked)) void __wrap_rename(void) {
    __asm__ volatile (
        "movq _fwd___wrap_rename@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_rmdir */
__attribute__((naked)) void __wrap_rmdir(void) {
    __asm__ volatile (
        "movq _fwd___wrap_rmdir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_scandir */
__attribute__((naked)) void __wrap_scandir(void) {
    __asm__ volatile (
        "movq _fwd___wrap_scandir@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_scandir64 */
__attribute__((naked)) void __wrap_scandir64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_scandir64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_stat */
__attribute__((naked)) void __wrap_stat(void) {
    __asm__ volatile (
        "movq _fwd___wrap_stat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_stat64 */
__attribute__((naked)) void __wrap_stat64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_stat64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_statfs */
__attribute__((naked)) void __wrap_statfs(void) {
    __asm__ volatile (
        "movq _fwd___wrap_statfs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_statfs64 */
__attribute__((naked)) void __wrap_statfs64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_statfs64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_statvfs */
__attribute__((naked)) void __wrap_statvfs(void) {
    __asm__ volatile (
        "movq _fwd___wrap_statvfs@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_statvfs64 */
__attribute__((naked)) void __wrap_statvfs64(void) {
    __asm__ volatile (
        "movq _fwd___wrap_statvfs64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_symlink */
__attribute__((naked)) void __wrap_symlink(void) {
    __asm__ volatile (
        "movq _fwd___wrap_symlink@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_unlink */
__attribute__((naked)) void __wrap_unlink(void) {
    __asm__ volatile (
        "movq _fwd___wrap_unlink@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_utime */
__attribute__((naked)) void __wrap_utime(void) {
    __asm__ volatile (
        "movq _fwd___wrap_utime@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap_utimes */
__attribute__((naked)) void __wrap_utimes(void) {
    __asm__ volatile (
        "movq _fwd___wrap_utimes@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap___xstat */
__attribute__((naked)) void __wrap___xstat(void) {
    __asm__ volatile (
        "movq _fwd___wrap___xstat@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

/* __wrap___xstat64 */
__attribute__((naked)) void __wrap___xstat64(void) {
    __asm__ volatile (
        "movq _fwd___wrap___xstat64@GOTPCREL(%%rip), %%rax\n"
        "movq (%%rax), %%rax\n"
        "jmp *%%rax\n"
        ::: "rax"
    );
}

static void init_forwards(void *real_lib);

__attribute__((constructor))
static void proxy_constructor(void) {
    ensure_real_lib();
    if (g_real_lib) {
        init_forwards(g_real_lib);
    }
}

static void init_forwards(void *real_lib) {
    _fwd_GetHSteamPipe = dlsym(real_lib, "GetHSteamPipe");
    _fwd_GetHSteamUser = dlsym(real_lib, "GetHSteamUser");
    _fwd_SteamAPI_gameserveritem_t_Construct = dlsym(real_lib, "SteamAPI_gameserveritem_t_Construct");
    _fwd_SteamAPI_gameserveritem_t_GetName = dlsym(real_lib, "SteamAPI_gameserveritem_t_GetName");
    _fwd_SteamAPI_gameserveritem_t_SetName = dlsym(real_lib, "SteamAPI_gameserveritem_t_SetName");
    _fwd_SteamAPI_GetHSteamPipe = dlsym(real_lib, "SteamAPI_GetHSteamPipe");
    _fwd_SteamAPI_GetHSteamUser = dlsym(real_lib, "SteamAPI_GetHSteamUser");
    _fwd_SteamAPI_GetSteamInstallPath = dlsym(real_lib, "SteamAPI_GetSteamInstallPath");
    _fwd_SteamAPI_InitAnonymousUser = dlsym(real_lib, "SteamAPI_InitAnonymousUser");
    _fwd_SteamAPI_InitFlat = dlsym(real_lib, "SteamAPI_InitFlat");
    _fwd_SteamAPI_InitSafe = dlsym(real_lib, "SteamAPI_InitSafe");
    _fwd_SteamAPI_IsSteamRunning = dlsym(real_lib, "SteamAPI_IsSteamRunning");
    _fwd_SteamAPI_ISteamApps_BIsCybercafe = dlsym(real_lib, "SteamAPI_ISteamApps_BIsCybercafe");
    _fwd_SteamAPI_ISteamApps_BIsLowViolence = dlsym(real_lib, "SteamAPI_ISteamApps_BIsLowViolence");
    _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing = dlsym(real_lib, "SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing");
    _fwd_SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend = dlsym(real_lib, "SteamAPI_ISteamApps_BIsSubscribedFromFreeWeekend");
    _fwd_SteamAPI_ISteamApps_BIsTimedTrial = dlsym(real_lib, "SteamAPI_ISteamApps_BIsTimedTrial");
    _fwd_SteamAPI_ISteamApps_BIsVACBanned = dlsym(real_lib, "SteamAPI_ISteamApps_BIsVACBanned");
    _fwd_SteamAPI_ISteamApps_GetAppBuildId = dlsym(real_lib, "SteamAPI_ISteamApps_GetAppBuildId");
    _fwd_SteamAPI_ISteamApps_GetAppInstallDir = dlsym(real_lib, "SteamAPI_ISteamApps_GetAppInstallDir");
    _fwd_SteamAPI_ISteamApps_GetAppOwner = dlsym(real_lib, "SteamAPI_ISteamApps_GetAppOwner");
    _fwd_SteamAPI_ISteamApps_GetAvailableGameLanguages = dlsym(real_lib, "SteamAPI_ISteamApps_GetAvailableGameLanguages");
    _fwd_SteamAPI_ISteamApps_GetBetaInfo = dlsym(real_lib, "SteamAPI_ISteamApps_GetBetaInfo");
    _fwd_SteamAPI_ISteamApps_GetCurrentBetaName = dlsym(real_lib, "SteamAPI_ISteamApps_GetCurrentBetaName");
    _fwd_SteamAPI_ISteamApps_GetCurrentGameLanguage = dlsym(real_lib, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    _fwd_SteamAPI_ISteamApps_GetDlcDownloadProgress = dlsym(real_lib, "SteamAPI_ISteamApps_GetDlcDownloadProgress");
    _fwd_SteamAPI_ISteamApps_GetFileDetails = dlsym(real_lib, "SteamAPI_ISteamApps_GetFileDetails");
    _fwd_SteamAPI_ISteamApps_GetInstalledDepots = dlsym(real_lib, "SteamAPI_ISteamApps_GetInstalledDepots");
    _fwd_SteamAPI_ISteamApps_GetLaunchCommandLine = dlsym(real_lib, "SteamAPI_ISteamApps_GetLaunchCommandLine");
    _fwd_SteamAPI_ISteamApps_GetLaunchQueryParam = dlsym(real_lib, "SteamAPI_ISteamApps_GetLaunchQueryParam");
    _fwd_SteamAPI_ISteamApps_GetNumBetas = dlsym(real_lib, "SteamAPI_ISteamApps_GetNumBetas");
    _fwd_SteamAPI_ISteamApps_InstallDLC = dlsym(real_lib, "SteamAPI_ISteamApps_InstallDLC");
    _fwd_SteamAPI_ISteamApps_MarkContentCorrupt = dlsym(real_lib, "SteamAPI_ISteamApps_MarkContentCorrupt");
    _fwd_SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys = dlsym(real_lib, "SteamAPI_ISteamApps_RequestAllProofOfPurchaseKeys");
    _fwd_SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey = dlsym(real_lib, "SteamAPI_ISteamApps_RequestAppProofOfPurchaseKey");
    _fwd_SteamAPI_ISteamApps_SetActiveBeta = dlsym(real_lib, "SteamAPI_ISteamApps_SetActiveBeta");
    _fwd_SteamAPI_ISteamApps_SetDlcContext = dlsym(real_lib, "SteamAPI_ISteamApps_SetDlcContext");
    _fwd_SteamAPI_ISteamApps_UninstallDLC = dlsym(real_lib, "SteamAPI_ISteamApps_UninstallDLC");
    _fwd_SteamAPI_ISteamClient_BReleaseSteamPipe = dlsym(real_lib, "SteamAPI_ISteamClient_BReleaseSteamPipe");
    _fwd_SteamAPI_ISteamClient_BShutdownIfAllPipesClosed = dlsym(real_lib, "SteamAPI_ISteamClient_BShutdownIfAllPipesClosed");
    _fwd_SteamAPI_ISteamClient_ConnectToGlobalUser = dlsym(real_lib, "SteamAPI_ISteamClient_ConnectToGlobalUser");
    _fwd_SteamAPI_ISteamClient_CreateLocalUser = dlsym(real_lib, "SteamAPI_ISteamClient_CreateLocalUser");
    _fwd_SteamAPI_ISteamClient_CreateSteamPipe = dlsym(real_lib, "SteamAPI_ISteamClient_CreateSteamPipe");
    _fwd_SteamAPI_ISteamClient_GetIPCCallCount = dlsym(real_lib, "SteamAPI_ISteamClient_GetIPCCallCount");
    _fwd_SteamAPI_ISteamClient_GetISteamApps = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamApps");
    _fwd_SteamAPI_ISteamClient_GetISteamController = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamController");
    _fwd_SteamAPI_ISteamClient_GetISteamFriends = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamFriends");
    _fwd_SteamAPI_ISteamClient_GetISteamGameSearch = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamGameSearch");
    _fwd_SteamAPI_ISteamClient_GetISteamGameServer = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamGameServer");
    _fwd_SteamAPI_ISteamClient_GetISteamGameServerStats = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamGameServerStats");
    _fwd_SteamAPI_ISteamClient_GetISteamGenericInterface = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    _fwd_SteamAPI_ISteamClient_GetISteamHTMLSurface = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamHTMLSurface");
    _fwd_SteamAPI_ISteamClient_GetISteamHTTP = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamHTTP");
    _fwd_SteamAPI_ISteamClient_GetISteamInput = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamInput");
    _fwd_SteamAPI_ISteamClient_GetISteamInventory = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamInventory");
    _fwd_SteamAPI_ISteamClient_GetISteamMatchmaking = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    _fwd_SteamAPI_ISteamClient_GetISteamMatchmakingServers = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamMatchmakingServers");
    _fwd_SteamAPI_ISteamClient_GetISteamMusic = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamMusic");
    _fwd_SteamAPI_ISteamClient_GetISteamMusicRemote = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamMusicRemote");
    _fwd_SteamAPI_ISteamClient_GetISteamNetworking = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamNetworking");
    _fwd_SteamAPI_ISteamClient_GetISteamParentalSettings = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamParentalSettings");
    _fwd_SteamAPI_ISteamClient_GetISteamParties = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamParties");
    _fwd_SteamAPI_ISteamClient_GetISteamRemotePlay = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamRemotePlay");
    _fwd_SteamAPI_ISteamClient_GetISteamRemoteStorage = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamRemoteStorage");
    _fwd_SteamAPI_ISteamClient_GetISteamScreenshots = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamScreenshots");
    _fwd_SteamAPI_ISteamClient_GetISteamUGC = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamUGC");
    _fwd_SteamAPI_ISteamClient_GetISteamUser = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamUser");
    _fwd_SteamAPI_ISteamClient_GetISteamUserStats = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamUserStats");
    _fwd_SteamAPI_ISteamClient_GetISteamUtils = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamUtils");
    _fwd_SteamAPI_ISteamClient_GetISteamVideo = dlsym(real_lib, "SteamAPI_ISteamClient_GetISteamVideo");
    _fwd_SteamAPI_ISteamClient_ReleaseUser = dlsym(real_lib, "SteamAPI_ISteamClient_ReleaseUser");
    _fwd_SteamAPI_ISteamClient_SetLocalIPBinding = dlsym(real_lib, "SteamAPI_ISteamClient_SetLocalIPBinding");
    _fwd_SteamAPI_ISteamClient_SetWarningMessageHook = dlsym(real_lib, "SteamAPI_ISteamClient_SetWarningMessageHook");
    _fwd_SteamAPI_ISteamController_ActivateActionSet = dlsym(real_lib, "SteamAPI_ISteamController_ActivateActionSet");
    _fwd_SteamAPI_ISteamController_ActivateActionSetLayer = dlsym(real_lib, "SteamAPI_ISteamController_ActivateActionSetLayer");
    _fwd_SteamAPI_ISteamController_DeactivateActionSetLayer = dlsym(real_lib, "SteamAPI_ISteamController_DeactivateActionSetLayer");
    _fwd_SteamAPI_ISteamController_DeactivateAllActionSetLayers = dlsym(real_lib, "SteamAPI_ISteamController_DeactivateAllActionSetLayers");
    _fwd_SteamAPI_ISteamController_GetActionOriginFromXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamController_GetActionOriginFromXboxOrigin");
    _fwd_SteamAPI_ISteamController_GetActionSetHandle = dlsym(real_lib, "SteamAPI_ISteamController_GetActionSetHandle");
    _fwd_SteamAPI_ISteamController_GetActiveActionSetLayers = dlsym(real_lib, "SteamAPI_ISteamController_GetActiveActionSetLayers");
    _fwd_SteamAPI_ISteamController_GetAnalogActionData = dlsym(real_lib, "SteamAPI_ISteamController_GetAnalogActionData");
    _fwd_SteamAPI_ISteamController_GetAnalogActionHandle = dlsym(real_lib, "SteamAPI_ISteamController_GetAnalogActionHandle");
    _fwd_SteamAPI_ISteamController_GetAnalogActionOrigins = dlsym(real_lib, "SteamAPI_ISteamController_GetAnalogActionOrigins");
    _fwd_SteamAPI_ISteamController_GetConnectedControllers = dlsym(real_lib, "SteamAPI_ISteamController_GetConnectedControllers");
    _fwd_SteamAPI_ISteamController_GetControllerBindingRevision = dlsym(real_lib, "SteamAPI_ISteamController_GetControllerBindingRevision");
    _fwd_SteamAPI_ISteamController_GetControllerForGamepadIndex = dlsym(real_lib, "SteamAPI_ISteamController_GetControllerForGamepadIndex");
    _fwd_SteamAPI_ISteamController_GetCurrentActionSet = dlsym(real_lib, "SteamAPI_ISteamController_GetCurrentActionSet");
    _fwd_SteamAPI_ISteamController_GetDigitalActionData = dlsym(real_lib, "SteamAPI_ISteamController_GetDigitalActionData");
    _fwd_SteamAPI_ISteamController_GetDigitalActionHandle = dlsym(real_lib, "SteamAPI_ISteamController_GetDigitalActionHandle");
    _fwd_SteamAPI_ISteamController_GetDigitalActionOrigins = dlsym(real_lib, "SteamAPI_ISteamController_GetDigitalActionOrigins");
    _fwd_SteamAPI_ISteamController_GetGamepadIndexForController = dlsym(real_lib, "SteamAPI_ISteamController_GetGamepadIndexForController");
    _fwd_SteamAPI_ISteamController_GetGlyphForActionOrigin = dlsym(real_lib, "SteamAPI_ISteamController_GetGlyphForActionOrigin");
    _fwd_SteamAPI_ISteamController_GetGlyphForXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamController_GetGlyphForXboxOrigin");
    _fwd_SteamAPI_ISteamController_GetInputTypeForHandle = dlsym(real_lib, "SteamAPI_ISteamController_GetInputTypeForHandle");
    _fwd_SteamAPI_ISteamController_GetMotionData = dlsym(real_lib, "SteamAPI_ISteamController_GetMotionData");
    _fwd_SteamAPI_ISteamController_GetStringForActionOrigin = dlsym(real_lib, "SteamAPI_ISteamController_GetStringForActionOrigin");
    _fwd_SteamAPI_ISteamController_GetStringForXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamController_GetStringForXboxOrigin");
    _fwd_SteamAPI_ISteamController_Init = dlsym(real_lib, "SteamAPI_ISteamController_Init");
    _fwd_SteamAPI_ISteamController_RunFrame = dlsym(real_lib, "SteamAPI_ISteamController_RunFrame");
    _fwd_SteamAPI_ISteamController_SetLEDColor = dlsym(real_lib, "SteamAPI_ISteamController_SetLEDColor");
    _fwd_SteamAPI_ISteamController_ShowBindingPanel = dlsym(real_lib, "SteamAPI_ISteamController_ShowBindingPanel");
    _fwd_SteamAPI_ISteamController_Shutdown = dlsym(real_lib, "SteamAPI_ISteamController_Shutdown");
    _fwd_SteamAPI_ISteamController_StopAnalogActionMomentum = dlsym(real_lib, "SteamAPI_ISteamController_StopAnalogActionMomentum");
    _fwd_SteamAPI_ISteamController_TranslateActionOrigin = dlsym(real_lib, "SteamAPI_ISteamController_TranslateActionOrigin");
    _fwd_SteamAPI_ISteamController_TriggerHapticPulse = dlsym(real_lib, "SteamAPI_ISteamController_TriggerHapticPulse");
    _fwd_SteamAPI_ISteamController_TriggerRepeatedHapticPulse = dlsym(real_lib, "SteamAPI_ISteamController_TriggerRepeatedHapticPulse");
    _fwd_SteamAPI_ISteamController_TriggerVibration = dlsym(real_lib, "SteamAPI_ISteamController_TriggerVibration");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlay = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlay");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialogConnectString");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayRemotePlayTogetherInviteDialog");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToStore = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToStore");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToUser = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
    _fwd_SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage = dlsym(real_lib, "SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage");
    _fwd_SteamAPI_ISteamFriends_BHasEquippedProfileItem = dlsym(real_lib, "SteamAPI_ISteamFriends_BHasEquippedProfileItem");
    _fwd_SteamAPI_ISteamFriends_ClearRichPresence = dlsym(real_lib, "SteamAPI_ISteamFriends_ClearRichPresence");
    _fwd_SteamAPI_ISteamFriends_CloseClanChatWindowInSteam = dlsym(real_lib, "SteamAPI_ISteamFriends_CloseClanChatWindowInSteam");
    _fwd_SteamAPI_ISteamFriends_DownloadClanActivityCounts = dlsym(real_lib, "SteamAPI_ISteamFriends_DownloadClanActivityCounts");
    _fwd_SteamAPI_ISteamFriends_EnumerateFollowingList = dlsym(real_lib, "SteamAPI_ISteamFriends_EnumerateFollowingList");
    _fwd_SteamAPI_ISteamFriends_GetChatMemberByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetChatMemberByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanActivityCounts = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanActivityCounts");
    _fwd_SteamAPI_ISteamFriends_GetClanByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanChatMemberCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanChatMemberCount");
    _fwd_SteamAPI_ISteamFriends_GetClanChatMessage = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanChatMessage");
    _fwd_SteamAPI_ISteamFriends_GetClanCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanCount");
    _fwd_SteamAPI_ISteamFriends_GetClanName = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanName");
    _fwd_SteamAPI_ISteamFriends_GetClanOfficerByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanOfficerByIndex");
    _fwd_SteamAPI_ISteamFriends_GetClanOfficerCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanOfficerCount");
    _fwd_SteamAPI_ISteamFriends_GetClanOwner = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanOwner");
    _fwd_SteamAPI_ISteamFriends_GetClanTag = dlsym(real_lib, "SteamAPI_ISteamFriends_GetClanTag");
    _fwd_SteamAPI_ISteamFriends_GetCoplayFriend = dlsym(real_lib, "SteamAPI_ISteamFriends_GetCoplayFriend");
    _fwd_SteamAPI_ISteamFriends_GetCoplayFriendCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetCoplayFriendCount");
    _fwd_SteamAPI_ISteamFriends_GetFollowerCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFollowerCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendCoplayGame = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendCoplayGame");
    _fwd_SteamAPI_ISteamFriends_GetFriendCoplayTime = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendCoplayTime");
    _fwd_SteamAPI_ISteamFriends_GetFriendCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendCountFromSource = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendCountFromSource");
    _fwd_SteamAPI_ISteamFriends_GetFriendFromSourceByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendFromSourceByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendGamePlayed = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    _fwd_SteamAPI_ISteamFriends_GetFriendMessage = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendMessage");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaName = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaNameHistory = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaNameHistory");
    _fwd_SteamAPI_ISteamFriends_GetFriendPersonaState = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendPersonaState");
    _fwd_SteamAPI_ISteamFriends_GetFriendRelationship = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendRelationship");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresence = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresence");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendRichPresenceKeyCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupIDByIndex");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersCount = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupMembersCount");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupMembersList = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupMembersList");
    _fwd_SteamAPI_ISteamFriends_GetFriendsGroupName = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendsGroupName");
    _fwd_SteamAPI_ISteamFriends_GetFriendSteamLevel = dlsym(real_lib, "SteamAPI_ISteamFriends_GetFriendSteamLevel");
    _fwd_SteamAPI_ISteamFriends_GetLargeFriendAvatar = dlsym(real_lib, "SteamAPI_ISteamFriends_GetLargeFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetMediumFriendAvatar = dlsym(real_lib, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages = dlsym(real_lib, "SteamAPI_ISteamFriends_GetNumChatsWithUnreadPriorityMessages");
    _fwd_SteamAPI_ISteamFriends_GetPersonaName = dlsym(real_lib, "SteamAPI_ISteamFriends_GetPersonaName");
    _fwd_SteamAPI_ISteamFriends_GetPersonaState = dlsym(real_lib, "SteamAPI_ISteamFriends_GetPersonaState");
    _fwd_SteamAPI_ISteamFriends_GetPlayerNickname = dlsym(real_lib, "SteamAPI_ISteamFriends_GetPlayerNickname");
    _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyString = dlsym(real_lib, "SteamAPI_ISteamFriends_GetProfileItemPropertyString");
    _fwd_SteamAPI_ISteamFriends_GetProfileItemPropertyUint = dlsym(real_lib, "SteamAPI_ISteamFriends_GetProfileItemPropertyUint");
    _fwd_SteamAPI_ISteamFriends_GetSmallFriendAvatar = dlsym(real_lib, "SteamAPI_ISteamFriends_GetSmallFriendAvatar");
    _fwd_SteamAPI_ISteamFriends_GetUserRestrictions = dlsym(real_lib, "SteamAPI_ISteamFriends_GetUserRestrictions");
    _fwd_SteamAPI_ISteamFriends_HasFriend = dlsym(real_lib, "SteamAPI_ISteamFriends_HasFriend");
    _fwd_SteamAPI_ISteamFriends_InviteUserToGame = dlsym(real_lib, "SteamAPI_ISteamFriends_InviteUserToGame");
    _fwd_SteamAPI_ISteamFriends_IsClanChatAdmin = dlsym(real_lib, "SteamAPI_ISteamFriends_IsClanChatAdmin");
    _fwd_SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam = dlsym(real_lib, "SteamAPI_ISteamFriends_IsClanChatWindowOpenInSteam");
    _fwd_SteamAPI_ISteamFriends_IsClanOfficialGameGroup = dlsym(real_lib, "SteamAPI_ISteamFriends_IsClanOfficialGameGroup");
    _fwd_SteamAPI_ISteamFriends_IsClanPublic = dlsym(real_lib, "SteamAPI_ISteamFriends_IsClanPublic");
    _fwd_SteamAPI_ISteamFriends_IsFollowing = dlsym(real_lib, "SteamAPI_ISteamFriends_IsFollowing");
    _fwd_SteamAPI_ISteamFriends_IsUserInSource = dlsym(real_lib, "SteamAPI_ISteamFriends_IsUserInSource");
    _fwd_SteamAPI_ISteamFriends_JoinClanChatRoom = dlsym(real_lib, "SteamAPI_ISteamFriends_JoinClanChatRoom");
    _fwd_SteamAPI_ISteamFriends_LeaveClanChatRoom = dlsym(real_lib, "SteamAPI_ISteamFriends_LeaveClanChatRoom");
    _fwd_SteamAPI_ISteamFriends_OpenClanChatWindowInSteam = dlsym(real_lib, "SteamAPI_ISteamFriends_OpenClanChatWindowInSteam");
    _fwd_SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser = dlsym(real_lib, "SteamAPI_ISteamFriends_RegisterProtocolInOverlayBrowser");
    _fwd_SteamAPI_ISteamFriends_ReplyToFriendMessage = dlsym(real_lib, "SteamAPI_ISteamFriends_ReplyToFriendMessage");
    _fwd_SteamAPI_ISteamFriends_RequestClanOfficerList = dlsym(real_lib, "SteamAPI_ISteamFriends_RequestClanOfficerList");
    _fwd_SteamAPI_ISteamFriends_RequestEquippedProfileItems = dlsym(real_lib, "SteamAPI_ISteamFriends_RequestEquippedProfileItems");
    _fwd_SteamAPI_ISteamFriends_RequestFriendRichPresence = dlsym(real_lib, "SteamAPI_ISteamFriends_RequestFriendRichPresence");
    _fwd_SteamAPI_ISteamFriends_RequestUserInformation = dlsym(real_lib, "SteamAPI_ISteamFriends_RequestUserInformation");
    _fwd_SteamAPI_ISteamFriends_SendClanChatMessage = dlsym(real_lib, "SteamAPI_ISteamFriends_SendClanChatMessage");
    _fwd_SteamAPI_ISteamFriends_SetInGameVoiceSpeaking = dlsym(real_lib, "SteamAPI_ISteamFriends_SetInGameVoiceSpeaking");
    _fwd_SteamAPI_ISteamFriends_SetListenForFriendsMessages = dlsym(real_lib, "SteamAPI_ISteamFriends_SetListenForFriendsMessages");
    _fwd_SteamAPI_ISteamFriends_SetPersonaName = dlsym(real_lib, "SteamAPI_ISteamFriends_SetPersonaName");
    _fwd_SteamAPI_ISteamFriends_SetPlayedWith = dlsym(real_lib, "SteamAPI_ISteamFriends_SetPlayedWith");
    _fwd_SteamAPI_ISteamFriends_SetRichPresence = dlsym(real_lib, "SteamAPI_ISteamFriends_SetRichPresence");
    _fwd_SteamAPI_ISteamGameSearch_AcceptGame = dlsym(real_lib, "SteamAPI_ISteamGameSearch_AcceptGame");
    _fwd_SteamAPI_ISteamGameSearch_AddGameSearchParams = dlsym(real_lib, "SteamAPI_ISteamGameSearch_AddGameSearchParams");
    _fwd_SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame = dlsym(real_lib, "SteamAPI_ISteamGameSearch_CancelRequestPlayersForGame");
    _fwd_SteamAPI_ISteamGameSearch_DeclineGame = dlsym(real_lib, "SteamAPI_ISteamGameSearch_DeclineGame");
    _fwd_SteamAPI_ISteamGameSearch_EndGame = dlsym(real_lib, "SteamAPI_ISteamGameSearch_EndGame");
    _fwd_SteamAPI_ISteamGameSearch_EndGameSearch = dlsym(real_lib, "SteamAPI_ISteamGameSearch_EndGameSearch");
    _fwd_SteamAPI_ISteamGameSearch_HostConfirmGameStart = dlsym(real_lib, "SteamAPI_ISteamGameSearch_HostConfirmGameStart");
    _fwd_SteamAPI_ISteamGameSearch_RequestPlayersForGame = dlsym(real_lib, "SteamAPI_ISteamGameSearch_RequestPlayersForGame");
    _fwd_SteamAPI_ISteamGameSearch_RetrieveConnectionDetails = dlsym(real_lib, "SteamAPI_ISteamGameSearch_RetrieveConnectionDetails");
    _fwd_SteamAPI_ISteamGameSearch_SearchForGameSolo = dlsym(real_lib, "SteamAPI_ISteamGameSearch_SearchForGameSolo");
    _fwd_SteamAPI_ISteamGameSearch_SearchForGameWithLobby = dlsym(real_lib, "SteamAPI_ISteamGameSearch_SearchForGameWithLobby");
    _fwd_SteamAPI_ISteamGameSearch_SetConnectionDetails = dlsym(real_lib, "SteamAPI_ISteamGameSearch_SetConnectionDetails");
    _fwd_SteamAPI_ISteamGameSearch_SetGameHostParams = dlsym(real_lib, "SteamAPI_ISteamGameSearch_SetGameHostParams");
    _fwd_SteamAPI_ISteamGameSearch_SubmitPlayerResult = dlsym(real_lib, "SteamAPI_ISteamGameSearch_SubmitPlayerResult");
    _fwd_SteamAPI_ISteamGameServer_AssociateWithClan = dlsym(real_lib, "SteamAPI_ISteamGameServer_AssociateWithClan");
    _fwd_SteamAPI_ISteamGameServer_BeginAuthSession = dlsym(real_lib, "SteamAPI_ISteamGameServer_BeginAuthSession");
    _fwd_SteamAPI_ISteamGameServer_BLoggedOn = dlsym(real_lib, "SteamAPI_ISteamGameServer_BLoggedOn");
    _fwd_SteamAPI_ISteamGameServer_BSecure = dlsym(real_lib, "SteamAPI_ISteamGameServer_BSecure");
    _fwd_SteamAPI_ISteamGameServer_BUpdateUserData = dlsym(real_lib, "SteamAPI_ISteamGameServer_BUpdateUserData");
    _fwd_SteamAPI_ISteamGameServer_CancelAuthTicket = dlsym(real_lib, "SteamAPI_ISteamGameServer_CancelAuthTicket");
    _fwd_SteamAPI_ISteamGameServer_ClearAllKeyValues = dlsym(real_lib, "SteamAPI_ISteamGameServer_ClearAllKeyValues");
    _fwd_SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility = dlsym(real_lib, "SteamAPI_ISteamGameServer_ComputeNewPlayerCompatibility");
    _fwd_SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection = dlsym(real_lib, "SteamAPI_ISteamGameServer_CreateUnauthenticatedUserConnection");
    _fwd_SteamAPI_ISteamGameServer_EndAuthSession = dlsym(real_lib, "SteamAPI_ISteamGameServer_EndAuthSession");
    _fwd_SteamAPI_ISteamGameServer_GetAuthSessionTicket = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetAuthSessionTicket");
    _fwd_SteamAPI_ISteamGameServer_GetGameplayStats = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetGameplayStats");
    _fwd_SteamAPI_ISteamGameServer_GetNextOutgoingPacket = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetNextOutgoingPacket");
    _fwd_SteamAPI_ISteamGameServer_GetPublicIP = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetPublicIP");
    _fwd_SteamAPI_ISteamGameServer_GetServerReputation = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetServerReputation");
    _fwd_SteamAPI_ISteamGameServer_GetSteamID = dlsym(real_lib, "SteamAPI_ISteamGameServer_GetSteamID");
    _fwd_SteamAPI_ISteamGameServer_HandleIncomingPacket = dlsym(real_lib, "SteamAPI_ISteamGameServer_HandleIncomingPacket");
    _fwd_SteamAPI_ISteamGameServer_LogOff = dlsym(real_lib, "SteamAPI_ISteamGameServer_LogOff");
    _fwd_SteamAPI_ISteamGameServer_LogOn = dlsym(real_lib, "SteamAPI_ISteamGameServer_LogOn");
    _fwd_SteamAPI_ISteamGameServer_LogOnAnonymous = dlsym(real_lib, "SteamAPI_ISteamGameServer_LogOnAnonymous");
    _fwd_SteamAPI_ISteamGameServer_RequestUserGroupStatus = dlsym(real_lib, "SteamAPI_ISteamGameServer_RequestUserGroupStatus");
    _fwd_SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED = dlsym(real_lib, "SteamAPI_ISteamGameServer_SendUserConnectAndAuthenticate_DEPRECATED");
    _fwd_SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED = dlsym(real_lib, "SteamAPI_ISteamGameServer_SendUserDisconnect_DEPRECATED");
    _fwd_SteamAPI_ISteamGameServer_SetAdvertiseServerActive = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetAdvertiseServerActive");
    _fwd_SteamAPI_ISteamGameServer_SetBotPlayerCount = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetBotPlayerCount");
    _fwd_SteamAPI_ISteamGameServer_SetDedicatedServer = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetDedicatedServer");
    _fwd_SteamAPI_ISteamGameServer_SetGameData = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetGameData");
    _fwd_SteamAPI_ISteamGameServer_SetGameDescription = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetGameDescription");
    _fwd_SteamAPI_ISteamGameServer_SetGameTags = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetGameTags");
    _fwd_SteamAPI_ISteamGameServer_SetKeyValue = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetKeyValue");
    _fwd_SteamAPI_ISteamGameServer_SetMapName = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetMapName");
    _fwd_SteamAPI_ISteamGameServer_SetMaxPlayerCount = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetMaxPlayerCount");
    _fwd_SteamAPI_ISteamGameServer_SetModDir = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetModDir");
    _fwd_SteamAPI_ISteamGameServer_SetPasswordProtected = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetPasswordProtected");
    _fwd_SteamAPI_ISteamGameServer_SetProduct = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetProduct");
    _fwd_SteamAPI_ISteamGameServer_SetRegion = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetRegion");
    _fwd_SteamAPI_ISteamGameServer_SetServerName = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetServerName");
    _fwd_SteamAPI_ISteamGameServer_SetSpectatorPort = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetSpectatorPort");
    _fwd_SteamAPI_ISteamGameServer_SetSpectatorServerName = dlsym(real_lib, "SteamAPI_ISteamGameServer_SetSpectatorServerName");
    _fwd_SteamAPI_ISteamGameServerStats_ClearUserAchievement = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_ClearUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserAchievement = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_GetUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserStatFloat = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_GetUserStatFloat");
    _fwd_SteamAPI_ISteamGameServerStats_GetUserStatInt32 = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_GetUserStatInt32");
    _fwd_SteamAPI_ISteamGameServerStats_RequestUserStats = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_RequestUserStats");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserAchievement = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_SetUserAchievement");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserStatFloat = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_SetUserStatFloat");
    _fwd_SteamAPI_ISteamGameServerStats_SetUserStatInt32 = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_SetUserStatInt32");
    _fwd_SteamAPI_ISteamGameServerStats_StoreUserStats = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_StoreUserStats");
    _fwd_SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat = dlsym(real_lib, "SteamAPI_ISteamGameServerStats_UpdateUserAvgRateStat");
    _fwd_SteamAPI_ISteamGameServer_UserHasLicenseForApp = dlsym(real_lib, "SteamAPI_ISteamGameServer_UserHasLicenseForApp");
    _fwd_SteamAPI_ISteamGameServer_WasRestartRequested = dlsym(real_lib, "SteamAPI_ISteamGameServer_WasRestartRequested");
    _fwd_SteamAPI_ISteamHTMLSurface_AddHeader = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_AddHeader");
    _fwd_SteamAPI_ISteamHTMLSurface_AllowStartRequest = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_AllowStartRequest");
    _fwd_SteamAPI_ISteamHTMLSurface_CopyToClipboard = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_CopyToClipboard");
    _fwd_SteamAPI_ISteamHTMLSurface_CreateBrowser = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_CreateBrowser");
    _fwd_SteamAPI_ISteamHTMLSurface_ExecuteJavascript = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_ExecuteJavascript");
    _fwd_SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_FileLoadDialogResponse");
    _fwd_SteamAPI_ISteamHTMLSurface_Find = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_Find");
    _fwd_SteamAPI_ISteamHTMLSurface_GetLinkAtPosition = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_GetLinkAtPosition");
    _fwd_SteamAPI_ISteamHTMLSurface_GoBack = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_GoBack");
    _fwd_SteamAPI_ISteamHTMLSurface_GoForward = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_GoForward");
    _fwd_SteamAPI_ISteamHTMLSurface_Init = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_Init");
    _fwd_SteamAPI_ISteamHTMLSurface_JSDialogResponse = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_JSDialogResponse");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyChar = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_KeyChar");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyDown = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_KeyDown");
    _fwd_SteamAPI_ISteamHTMLSurface_KeyUp = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_KeyUp");
    _fwd_SteamAPI_ISteamHTMLSurface_LoadURL = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_LoadURL");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseDoubleClick = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_MouseDoubleClick");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseDown = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_MouseDown");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseMove = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_MouseMove");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseUp = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_MouseUp");
    _fwd_SteamAPI_ISteamHTMLSurface_MouseWheel = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_MouseWheel");
    _fwd_SteamAPI_ISteamHTMLSurface_OpenDeveloperTools = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_OpenDeveloperTools");
    _fwd_SteamAPI_ISteamHTMLSurface_PasteFromClipboard = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_PasteFromClipboard");
    _fwd_SteamAPI_ISteamHTMLSurface_Reload = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_Reload");
    _fwd_SteamAPI_ISteamHTMLSurface_RemoveBrowser = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_RemoveBrowser");
    _fwd_SteamAPI_ISteamHTMLSurface_SetBackgroundMode = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetBackgroundMode");
    _fwd_SteamAPI_ISteamHTMLSurface_SetCookie = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetCookie");
    _fwd_SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetDPIScalingFactor");
    _fwd_SteamAPI_ISteamHTMLSurface_SetHorizontalScroll = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetHorizontalScroll");
    _fwd_SteamAPI_ISteamHTMLSurface_SetKeyFocus = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetKeyFocus");
    _fwd_SteamAPI_ISteamHTMLSurface_SetPageScaleFactor = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetPageScaleFactor");
    _fwd_SteamAPI_ISteamHTMLSurface_SetSize = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetSize");
    _fwd_SteamAPI_ISteamHTMLSurface_SetVerticalScroll = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_SetVerticalScroll");
    _fwd_SteamAPI_ISteamHTMLSurface_Shutdown = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_Shutdown");
    _fwd_SteamAPI_ISteamHTMLSurface_StopFind = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_StopFind");
    _fwd_SteamAPI_ISteamHTMLSurface_StopLoad = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_StopLoad");
    _fwd_SteamAPI_ISteamHTMLSurface_ViewSource = dlsym(real_lib, "SteamAPI_ISteamHTMLSurface_ViewSource");
    _fwd_SteamAPI_ISteamHTTP_CreateCookieContainer = dlsym(real_lib, "SteamAPI_ISteamHTTP_CreateCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_CreateHTTPRequest = dlsym(real_lib, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_DeferHTTPRequest = dlsym(real_lib, "SteamAPI_ISteamHTTP_DeferHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPDownloadProgressPct");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPRequestWasTimedOut");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodyData = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseBodySize = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue");
    _fwd_SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData = dlsym(real_lib, "SteamAPI_ISteamHTTP_GetHTTPStreamingResponseBodyData");
    _fwd_SteamAPI_ISteamHTTP_PrioritizeHTTPRequest = dlsym(real_lib, "SteamAPI_ISteamHTTP_PrioritizeHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_ReleaseCookieContainer = dlsym(real_lib, "SteamAPI_ISteamHTTP_ReleaseCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_ReleaseHTTPRequest = dlsym(real_lib, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_SendHTTPRequest = dlsym(real_lib, "SteamAPI_ISteamHTTP_SendHTTPRequest");
    _fwd_SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse = dlsym(real_lib, "SteamAPI_ISteamHTTP_SendHTTPRequestAndStreamResponse");
    _fwd_SteamAPI_ISteamHTTP_SetCookie = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetCookie");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestAbsoluteTimeoutMS");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestContextValue = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestContextValue");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestCookieContainer");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestGetOrPostParameter");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestNetworkActivityTimeout");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestRequiresVerifiedCertificate");
    _fwd_SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo = dlsym(real_lib, "SteamAPI_ISteamHTTP_SetHTTPRequestUserAgentInfo");
    _fwd_SteamAPI_ISteamInput_ActivateActionSet = dlsym(real_lib, "SteamAPI_ISteamInput_ActivateActionSet");
    _fwd_SteamAPI_ISteamInput_ActivateActionSetLayer = dlsym(real_lib, "SteamAPI_ISteamInput_ActivateActionSetLayer");
    _fwd_SteamAPI_ISteamInput_BNewDataAvailable = dlsym(real_lib, "SteamAPI_ISteamInput_BNewDataAvailable");
    _fwd_SteamAPI_ISteamInput_BWaitForData = dlsym(real_lib, "SteamAPI_ISteamInput_BWaitForData");
    _fwd_SteamAPI_ISteamInput_DeactivateActionSetLayer = dlsym(real_lib, "SteamAPI_ISteamInput_DeactivateActionSetLayer");
    _fwd_SteamAPI_ISteamInput_DeactivateAllActionSetLayers = dlsym(real_lib, "SteamAPI_ISteamInput_DeactivateAllActionSetLayers");
    _fwd_SteamAPI_ISteamInput_EnableActionEventCallbacks = dlsym(real_lib, "SteamAPI_ISteamInput_EnableActionEventCallbacks");
    _fwd_SteamAPI_ISteamInput_EnableDeviceCallbacks = dlsym(real_lib, "SteamAPI_ISteamInput_EnableDeviceCallbacks");
    _fwd_SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetActionOriginFromXboxOrigin");
    _fwd_SteamAPI_ISteamInput_GetActionSetHandle = dlsym(real_lib, "SteamAPI_ISteamInput_GetActionSetHandle");
    _fwd_SteamAPI_ISteamInput_GetActiveActionSetLayers = dlsym(real_lib, "SteamAPI_ISteamInput_GetActiveActionSetLayers");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionData = dlsym(real_lib, "SteamAPI_ISteamInput_GetAnalogActionData");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionHandle = dlsym(real_lib, "SteamAPI_ISteamInput_GetAnalogActionHandle");
    _fwd_SteamAPI_ISteamInput_GetAnalogActionOrigins = dlsym(real_lib, "SteamAPI_ISteamInput_GetAnalogActionOrigins");
    _fwd_SteamAPI_ISteamInput_GetConnectedControllers = dlsym(real_lib, "SteamAPI_ISteamInput_GetConnectedControllers");
    _fwd_SteamAPI_ISteamInput_GetControllerForGamepadIndex = dlsym(real_lib, "SteamAPI_ISteamInput_GetControllerForGamepadIndex");
    _fwd_SteamAPI_ISteamInput_GetCurrentActionSet = dlsym(real_lib, "SteamAPI_ISteamInput_GetCurrentActionSet");
    _fwd_SteamAPI_ISteamInput_GetDeviceBindingRevision = dlsym(real_lib, "SteamAPI_ISteamInput_GetDeviceBindingRevision");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionData = dlsym(real_lib, "SteamAPI_ISteamInput_GetDigitalActionData");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionHandle = dlsym(real_lib, "SteamAPI_ISteamInput_GetDigitalActionHandle");
    _fwd_SteamAPI_ISteamInput_GetDigitalActionOrigins = dlsym(real_lib, "SteamAPI_ISteamInput_GetDigitalActionOrigins");
    _fwd_SteamAPI_ISteamInput_GetGamepadIndexForController = dlsym(real_lib, "SteamAPI_ISteamInput_GetGamepadIndexForController");
    _fwd_SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy = dlsym(real_lib, "SteamAPI_ISteamInput_GetGlyphForActionOrigin_Legacy");
    _fwd_SteamAPI_ISteamInput_GetGlyphForXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetGlyphForXboxOrigin");
    _fwd_SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetGlyphPNGForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetGlyphSVGForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetInputTypeForHandle = dlsym(real_lib, "SteamAPI_ISteamInput_GetInputTypeForHandle");
    _fwd_SteamAPI_ISteamInput_GetMotionData = dlsym(real_lib, "SteamAPI_ISteamInput_GetMotionData");
    _fwd_SteamAPI_ISteamInput_GetRemotePlaySessionID = dlsym(real_lib, "SteamAPI_ISteamInput_GetRemotePlaySessionID");
    _fwd_SteamAPI_ISteamInput_GetSessionInputConfigurationSettings = dlsym(real_lib, "SteamAPI_ISteamInput_GetSessionInputConfigurationSettings");
    _fwd_SteamAPI_ISteamInput_GetStringForActionOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetStringForActionOrigin");
    _fwd_SteamAPI_ISteamInput_GetStringForAnalogActionName = dlsym(real_lib, "SteamAPI_ISteamInput_GetStringForAnalogActionName");
    _fwd_SteamAPI_ISteamInput_GetStringForDigitalActionName = dlsym(real_lib, "SteamAPI_ISteamInput_GetStringForDigitalActionName");
    _fwd_SteamAPI_ISteamInput_GetStringForXboxOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_GetStringForXboxOrigin");
    _fwd_SteamAPI_ISteamInput_Init = dlsym(real_lib, "SteamAPI_ISteamInput_Init");
    _fwd_SteamAPI_ISteamInput_Legacy_TriggerHapticPulse = dlsym(real_lib, "SteamAPI_ISteamInput_Legacy_TriggerHapticPulse");
    _fwd_SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse = dlsym(real_lib, "SteamAPI_ISteamInput_Legacy_TriggerRepeatedHapticPulse");
    _fwd_SteamAPI_ISteamInput_RunFrame = dlsym(real_lib, "SteamAPI_ISteamInput_RunFrame");
    _fwd_SteamAPI_ISteamInput_SetDualSenseTriggerEffect = dlsym(real_lib, "SteamAPI_ISteamInput_SetDualSenseTriggerEffect");
    _fwd_SteamAPI_ISteamInput_SetInputActionManifestFilePath = dlsym(real_lib, "SteamAPI_ISteamInput_SetInputActionManifestFilePath");
    _fwd_SteamAPI_ISteamInput_SetLEDColor = dlsym(real_lib, "SteamAPI_ISteamInput_SetLEDColor");
    _fwd_SteamAPI_ISteamInput_ShowBindingPanel = dlsym(real_lib, "SteamAPI_ISteamInput_ShowBindingPanel");
    _fwd_SteamAPI_ISteamInput_Shutdown = dlsym(real_lib, "SteamAPI_ISteamInput_Shutdown");
    _fwd_SteamAPI_ISteamInput_StopAnalogActionMomentum = dlsym(real_lib, "SteamAPI_ISteamInput_StopAnalogActionMomentum");
    _fwd_SteamAPI_ISteamInput_TranslateActionOrigin = dlsym(real_lib, "SteamAPI_ISteamInput_TranslateActionOrigin");
    _fwd_SteamAPI_ISteamInput_TriggerSimpleHapticEvent = dlsym(real_lib, "SteamAPI_ISteamInput_TriggerSimpleHapticEvent");
    _fwd_SteamAPI_ISteamInput_TriggerVibration = dlsym(real_lib, "SteamAPI_ISteamInput_TriggerVibration");
    _fwd_SteamAPI_ISteamInput_TriggerVibrationExtended = dlsym(real_lib, "SteamAPI_ISteamInput_TriggerVibrationExtended");
    _fwd_SteamAPI_ISteamInventory_AddPromoItem = dlsym(real_lib, "SteamAPI_ISteamInventory_AddPromoItem");
    _fwd_SteamAPI_ISteamInventory_AddPromoItems = dlsym(real_lib, "SteamAPI_ISteamInventory_AddPromoItems");
    _fwd_SteamAPI_ISteamInventory_CheckResultSteamID = dlsym(real_lib, "SteamAPI_ISteamInventory_CheckResultSteamID");
    _fwd_SteamAPI_ISteamInventory_ConsumeItem = dlsym(real_lib, "SteamAPI_ISteamInventory_ConsumeItem");
    _fwd_SteamAPI_ISteamInventory_DeserializeResult = dlsym(real_lib, "SteamAPI_ISteamInventory_DeserializeResult");
    _fwd_SteamAPI_ISteamInventory_DestroyResult = dlsym(real_lib, "SteamAPI_ISteamInventory_DestroyResult");
    _fwd_SteamAPI_ISteamInventory_ExchangeItems = dlsym(real_lib, "SteamAPI_ISteamInventory_ExchangeItems");
    _fwd_SteamAPI_ISteamInventory_GenerateItems = dlsym(real_lib, "SteamAPI_ISteamInventory_GenerateItems");
    _fwd_SteamAPI_ISteamInventory_GetAllItems = dlsym(real_lib, "SteamAPI_ISteamInventory_GetAllItems");
    _fwd_SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs = dlsym(real_lib, "SteamAPI_ISteamInventory_GetEligiblePromoItemDefinitionIDs");
    _fwd_SteamAPI_ISteamInventory_GetItemDefinitionIDs = dlsym(real_lib, "SteamAPI_ISteamInventory_GetItemDefinitionIDs");
    _fwd_SteamAPI_ISteamInventory_GetItemDefinitionProperty = dlsym(real_lib, "SteamAPI_ISteamInventory_GetItemDefinitionProperty");
    _fwd_SteamAPI_ISteamInventory_GetItemPrice = dlsym(real_lib, "SteamAPI_ISteamInventory_GetItemPrice");
    _fwd_SteamAPI_ISteamInventory_GetItemsByID = dlsym(real_lib, "SteamAPI_ISteamInventory_GetItemsByID");
    _fwd_SteamAPI_ISteamInventory_GetItemsWithPrices = dlsym(real_lib, "SteamAPI_ISteamInventory_GetItemsWithPrices");
    _fwd_SteamAPI_ISteamInventory_GetNumItemsWithPrices = dlsym(real_lib, "SteamAPI_ISteamInventory_GetNumItemsWithPrices");
    _fwd_SteamAPI_ISteamInventory_GetResultItemProperty = dlsym(real_lib, "SteamAPI_ISteamInventory_GetResultItemProperty");
    _fwd_SteamAPI_ISteamInventory_GetResultItems = dlsym(real_lib, "SteamAPI_ISteamInventory_GetResultItems");
    _fwd_SteamAPI_ISteamInventory_GetResultStatus = dlsym(real_lib, "SteamAPI_ISteamInventory_GetResultStatus");
    _fwd_SteamAPI_ISteamInventory_GetResultTimestamp = dlsym(real_lib, "SteamAPI_ISteamInventory_GetResultTimestamp");
    _fwd_SteamAPI_ISteamInventory_GrantPromoItems = dlsym(real_lib, "SteamAPI_ISteamInventory_GrantPromoItems");
    _fwd_SteamAPI_ISteamInventory_InspectItem = dlsym(real_lib, "SteamAPI_ISteamInventory_InspectItem");
    _fwd_SteamAPI_ISteamInventory_LoadItemDefinitions = dlsym(real_lib, "SteamAPI_ISteamInventory_LoadItemDefinitions");
    _fwd_SteamAPI_ISteamInventory_RemoveProperty = dlsym(real_lib, "SteamAPI_ISteamInventory_RemoveProperty");
    _fwd_SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs = dlsym(real_lib, "SteamAPI_ISteamInventory_RequestEligiblePromoItemDefinitionsIDs");
    _fwd_SteamAPI_ISteamInventory_RequestPrices = dlsym(real_lib, "SteamAPI_ISteamInventory_RequestPrices");
    _fwd_SteamAPI_ISteamInventory_SendItemDropHeartbeat = dlsym(real_lib, "SteamAPI_ISteamInventory_SendItemDropHeartbeat");
    _fwd_SteamAPI_ISteamInventory_SerializeResult = dlsym(real_lib, "SteamAPI_ISteamInventory_SerializeResult");
    _fwd_SteamAPI_ISteamInventory_SetPropertyBool = dlsym(real_lib, "SteamAPI_ISteamInventory_SetPropertyBool");
    _fwd_SteamAPI_ISteamInventory_SetPropertyFloat = dlsym(real_lib, "SteamAPI_ISteamInventory_SetPropertyFloat");
    _fwd_SteamAPI_ISteamInventory_SetPropertyInt64 = dlsym(real_lib, "SteamAPI_ISteamInventory_SetPropertyInt64");
    _fwd_SteamAPI_ISteamInventory_SetPropertyString = dlsym(real_lib, "SteamAPI_ISteamInventory_SetPropertyString");
    _fwd_SteamAPI_ISteamInventory_StartPurchase = dlsym(real_lib, "SteamAPI_ISteamInventory_StartPurchase");
    _fwd_SteamAPI_ISteamInventory_StartUpdateProperties = dlsym(real_lib, "SteamAPI_ISteamInventory_StartUpdateProperties");
    _fwd_SteamAPI_ISteamInventory_SubmitUpdateProperties = dlsym(real_lib, "SteamAPI_ISteamInventory_SubmitUpdateProperties");
    _fwd_SteamAPI_ISteamInventory_TradeItems = dlsym(real_lib, "SteamAPI_ISteamInventory_TradeItems");
    _fwd_SteamAPI_ISteamInventory_TransferItemQuantity = dlsym(real_lib, "SteamAPI_ISteamInventory_TransferItemQuantity");
    _fwd_SteamAPI_ISteamInventory_TriggerItemDrop = dlsym(real_lib, "SteamAPI_ISteamInventory_TriggerItemDrop");
    _fwd_SteamAPI_ISteamMatchmaking_AddFavoriteGame = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListCompatibleMembersFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNearValueFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    _fwd_SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    _fwd_SteamAPI_ISteamMatchmaking_CreateLobby = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_CreateLobby");
    _fwd_SteamAPI_ISteamMatchmaking_DeleteLobbyData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_DeleteLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGame = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_GetFavoriteGameCount = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetFavoriteGameCount");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyByIndex = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyChatEntry = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyDataCount = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyDataCount");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyGameServer = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyGameServer");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
    _fwd_SteamAPI_ISteamMatchmaking_GetLobbyOwner = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    _fwd_SteamAPI_ISteamMatchmaking_GetNumLobbyMembers = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    _fwd_SteamAPI_ISteamMatchmaking_InviteUserToLobby = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
    _fwd_SteamAPI_ISteamMatchmaking_JoinLobby = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_JoinLobby");
    _fwd_SteamAPI_ISteamMatchmaking_LeaveLobby = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond = dlsym(real_lib, "SteamAPI_ISteamMatchmakingPingResponse_ServerFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingPingResponse_ServerResponded = dlsym(real_lib, "SteamAPI_ISteamMatchmakingPingResponse_ServerResponded");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_AddPlayerToList");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond = dlsym(real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_PlayersFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete = dlsym(real_lib, "SteamAPI_ISteamMatchmakingPlayersResponse_PlayersRefreshComplete");
    _fwd_SteamAPI_ISteamMatchmaking_RemoveFavoriteGame = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_RemoveFavoriteGame");
    _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_RequestLobbyList = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond = dlsym(real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete = dlsym(real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesRefreshComplete");
    _fwd_SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded = dlsym(real_lib, "SteamAPI_ISteamMatchmakingRulesResponse_RulesResponded");
    _fwd_SteamAPI_ISteamMatchmaking_SendLobbyChatMsg = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_RefreshComplete");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_ServerFailedToRespond");
    _fwd_SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServerListResponse_ServerResponded");
    _fwd_SteamAPI_ISteamMatchmakingServers_CancelQuery = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_CancelQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_CancelServerQuery = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_CancelServerQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_GetServerCount = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_GetServerCount");
    _fwd_SteamAPI_ISteamMatchmakingServers_GetServerDetails = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_GetServerDetails");
    _fwd_SteamAPI_ISteamMatchmakingServers_IsRefreshing = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_IsRefreshing");
    _fwd_SteamAPI_ISteamMatchmakingServers_PingServer = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_PingServer");
    _fwd_SteamAPI_ISteamMatchmakingServers_PlayerDetails = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_PlayerDetails");
    _fwd_SteamAPI_ISteamMatchmakingServers_RefreshQuery = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RefreshQuery");
    _fwd_SteamAPI_ISteamMatchmakingServers_RefreshServer = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RefreshServer");
    _fwd_SteamAPI_ISteamMatchmakingServers_ReleaseRequest = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_ReleaseRequest");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestFavoritesServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestFriendsServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestHistoryServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestInternetServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestInternetServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestLANServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestLANServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_RequestSpectatorServerList");
    _fwd_SteamAPI_ISteamMatchmakingServers_ServerRules = dlsym(real_lib, "SteamAPI_ISteamMatchmakingServers_ServerRules");
    _fwd_SteamAPI_ISteamMatchmaking_SetLinkedLobby = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLinkedLobby");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyGameServer = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyGameServer");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyJoinable = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberData = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyOwner = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyOwner");
    _fwd_SteamAPI_ISteamMatchmaking_SetLobbyType = dlsym(real_lib, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    _fwd_SteamAPI_ISteamMusic_BIsEnabled = dlsym(real_lib, "SteamAPI_ISteamMusic_BIsEnabled");
    _fwd_SteamAPI_ISteamMusic_BIsPlaying = dlsym(real_lib, "SteamAPI_ISteamMusic_BIsPlaying");
    _fwd_SteamAPI_ISteamMusic_GetPlaybackStatus = dlsym(real_lib, "SteamAPI_ISteamMusic_GetPlaybackStatus");
    _fwd_SteamAPI_ISteamMusic_GetVolume = dlsym(real_lib, "SteamAPI_ISteamMusic_GetVolume");
    _fwd_SteamAPI_ISteamMusic_Pause = dlsym(real_lib, "SteamAPI_ISteamMusic_Pause");
    _fwd_SteamAPI_ISteamMusic_Play = dlsym(real_lib, "SteamAPI_ISteamMusic_Play");
    _fwd_SteamAPI_ISteamMusic_PlayNext = dlsym(real_lib, "SteamAPI_ISteamMusic_PlayNext");
    _fwd_SteamAPI_ISteamMusic_PlayPrevious = dlsym(real_lib, "SteamAPI_ISteamMusic_PlayPrevious");
    _fwd_SteamAPI_ISteamMusicRemote_BActivationSuccess = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_BActivationSuccess");
    _fwd_SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_BIsCurrentMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryDidChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryIsAvailable");
    _fwd_SteamAPI_ISteamMusicRemote_CurrentEntryWillChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_CurrentEntryWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_DeregisterSteamMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_EnableLooped = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnableLooped");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlaylists = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnablePlaylists");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlayNext = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnablePlayNext");
    _fwd_SteamAPI_ISteamMusicRemote_EnablePlayPrevious = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnablePlayPrevious");
    _fwd_SteamAPI_ISteamMusicRemote_EnableQueue = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnableQueue");
    _fwd_SteamAPI_ISteamMusicRemote_EnableShuffled = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_EnableShuffled");
    _fwd_SteamAPI_ISteamMusicRemote_PlaylistDidChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_PlaylistDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_PlaylistWillChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_PlaylistWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_QueueDidChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_QueueDidChange");
    _fwd_SteamAPI_ISteamMusicRemote_QueueWillChange = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_QueueWillChange");
    _fwd_SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_RegisterSteamMusicRemote");
    _fwd_SteamAPI_ISteamMusicRemote_ResetPlaylistEntries = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_ResetPlaylistEntries");
    _fwd_SteamAPI_ISteamMusicRemote_ResetQueueEntries = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_ResetQueueEntries");
    _fwd_SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetCurrentPlaylistEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetCurrentQueueEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetDisplayName = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetDisplayName");
    _fwd_SteamAPI_ISteamMusicRemote_SetPlaylistEntry = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetPlaylistEntry");
    _fwd_SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64 = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetPNGIcon_64x64");
    _fwd_SteamAPI_ISteamMusicRemote_SetQueueEntry = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_SetQueueEntry");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryCoverArt");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryElapsedSeconds");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateCurrentEntryText");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateLooped = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateLooped");
    _fwd_SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdatePlaybackStatus");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateShuffled = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateShuffled");
    _fwd_SteamAPI_ISteamMusicRemote_UpdateVolume = dlsym(real_lib, "SteamAPI_ISteamMusicRemote_UpdateVolume");
    _fwd_SteamAPI_ISteamMusic_SetVolume = dlsym(real_lib, "SteamAPI_ISteamMusic_SetVolume");
    _fwd_SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser");
    _fwd_SteamAPI_ISteamNetworking_AllowP2PPacketRelay = dlsym(real_lib, "SteamAPI_ISteamNetworking_AllowP2PPacketRelay");
    _fwd_SteamAPI_ISteamNetworking_CloseP2PChannelWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworking_CloseP2PChannelWithUser");
    _fwd_SteamAPI_ISteamNetworking_CloseP2PSessionWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser");
    _fwd_SteamAPI_ISteamNetworking_CreateConnectionSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_CreateConnectionSocket");
    _fwd_SteamAPI_ISteamNetworking_CreateListenSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_CreateListenSocket");
    _fwd_SteamAPI_ISteamNetworking_CreateP2PConnectionSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_CreateP2PConnectionSocket");
    _fwd_SteamAPI_ISteamNetworking_DestroyListenSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_DestroyListenSocket");
    _fwd_SteamAPI_ISteamNetworking_DestroySocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_DestroySocket");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort = dlsym(real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_DestroyFakeUDPPort");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages = dlsym(real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_ReceiveMessages");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup = dlsym(real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_ScheduleCleanup");
    _fwd_SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingFakeUDPPort_SendMessageToFakeIP");
    _fwd_SteamAPI_ISteamNetworking_GetListenSocketInfo = dlsym(real_lib, "SteamAPI_ISteamNetworking_GetListenSocketInfo");
    _fwd_SteamAPI_ISteamNetworking_GetMaxPacketSize = dlsym(real_lib, "SteamAPI_ISteamNetworking_GetMaxPacketSize");
    _fwd_SteamAPI_ISteamNetworking_GetP2PSessionState = dlsym(real_lib, "SteamAPI_ISteamNetworking_GetP2PSessionState");
    _fwd_SteamAPI_ISteamNetworking_GetSocketConnectionType = dlsym(real_lib, "SteamAPI_ISteamNetworking_GetSocketConnectionType");
    _fwd_SteamAPI_ISteamNetworking_GetSocketInfo = dlsym(real_lib, "SteamAPI_ISteamNetworking_GetSocketInfo");
    _fwd_SteamAPI_ISteamNetworking_IsDataAvailable = dlsym(real_lib, "SteamAPI_ISteamNetworking_IsDataAvailable");
    _fwd_SteamAPI_ISteamNetworking_IsDataAvailableOnSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_IsDataAvailableOnSocket");
    _fwd_SteamAPI_ISteamNetworking_IsP2PPacketAvailable = dlsym(real_lib, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable");
    _fwd_SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_CloseChannelWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
    _fwd_SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo");
    _fwd_SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    _fwd_SteamAPI_ISteamNetworkingMessages_SendMessageToUser = dlsym(real_lib, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    _fwd_SteamAPI_ISteamNetworking_ReadP2PPacket = dlsym(real_lib, "SteamAPI_ISteamNetworking_ReadP2PPacket");
    _fwd_SteamAPI_ISteamNetworking_RetrieveData = dlsym(real_lib, "SteamAPI_ISteamNetworking_RetrieveData");
    _fwd_SteamAPI_ISteamNetworking_RetrieveDataFromSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_RetrieveDataFromSocket");
    _fwd_SteamAPI_ISteamNetworking_SendDataOnSocket = dlsym(real_lib, "SteamAPI_ISteamNetworking_SendDataOnSocket");
    _fwd_SteamAPI_ISteamNetworking_SendP2PPacket = dlsym(real_lib, "SteamAPI_ISteamNetworking_SendP2PPacket");
    _fwd_SteamAPI_ISteamNetworkingSockets_AcceptConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_AcceptConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_BeginAsyncRequestFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CloseConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CloseConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_CloseListenSocket = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CloseListenSocket");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ConfigureConnectionLanes");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectByIPAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2P = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectP2P");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectP2PCustomSignaling");
    _fwd_SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ConnectToHostedDedicatedServer");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateFakeUDPPort");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateHostedDedicatedServerListenSocket");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2P");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateListenSocketP2PFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreatePollGroup = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreatePollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_CreateSocketPair = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_CreateSocketPair");
    _fwd_SteamAPI_ISteamNetworkingSockets_DestroyPollGroup = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_DestroyPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_FindRelayAuthTicketForServer");
    _fwd_SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_FlushMessagesOnConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetAuthenticationStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetCertificateRequest = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetCertificateRequest");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionInfo = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionInfo");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionName = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionName");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionRealTimeStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetConnectionUserData = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetConnectionUserData");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetDetailedConnectionStatus");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetFakeIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetFakeIP");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetGameCoordinatorServerLogin");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPOPID");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetHostedDedicatedServerPort");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetIdentity = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetIdentity");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetListenSocketAddress");
    _fwd_SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_GetRemoteFakeIPForConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_InitAuthentication = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_InitAuthentication");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ReceivedP2PCustomSignal");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ReceivedRelayAuthTicket");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ReceiveMessagesOnPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_ResetIdentity = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_ResetIdentity");
    _fwd_SteamAPI_ISteamNetworkingSockets_RunCallbacks = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    _fwd_SteamAPI_ISteamNetworkingSockets_SendMessages = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SendMessages");
    _fwd_SteamAPI_ISteamNetworkingSockets_SendMessageToConnection = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SendMessageToConnection");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetCertificate = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SetCertificate");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionName = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionName");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionPollGroup");
    _fwd_SteamAPI_ISteamNetworkingSockets_SetConnectionUserData = dlsym(real_lib, "SteamAPI_ISteamNetworkingSockets_SetConnectionUserData");
    _fwd_SteamAPI_ISteamNetworkingUtils_AllocateMessage = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_AllocateMessage");
    _fwd_SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_CheckPingDataUpToDate");
    _fwd_SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_ConvertPingLocationToString");
    _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeBetweenTwoLocations");
    _fwd_SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_EstimatePingTimeFromLocalHost");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValue = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetConfigValue");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetConfigValueInfo");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetDirectPingToPOP");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetIPv4FakeIPType");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetLocalPingLocation");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetPingToDataCenter");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPCount = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetPOPCount");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetPOPList = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetPOPList");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetRealIdentityForFakeIP");
    _fwd_SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
    _fwd_SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    _fwd_SteamAPI_ISteamNetworkingUtils_IsFakeIPv4 = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_IsFakeIPv4");
    _fwd_SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_IterateGenericEditableConfigValues");
    _fwd_SteamAPI_ISteamNetworkingUtils_ParsePingLocationString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_ParsePingLocationString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValue = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetConfigValueStruct");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueFloat");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32 = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueInt32");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetConnectionConfigValueString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetDebugOutputFunction");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_FakeIPResult");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionFailed");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_MessagesSessionRequest");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetAuthenticationStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamNetConnectionStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalCallback_SteamRelayNetworkStatusChanged");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueFloat");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32 = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValuePtr");
    _fwd_SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ParseString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIdentity_ToString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_GetFakeIPType");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ParseString");
    _fwd_SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString = dlsym(real_lib, "SteamAPI_ISteamNetworkingUtils_SteamNetworkingIPAddr_ToString");
    _fwd_SteamAPI_ISteamParentalSettings_BIsAppBlocked = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsAppBlocked");
    _fwd_SteamAPI_ISteamParentalSettings_BIsAppInBlockList = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsAppInBlockList");
    _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureBlocked = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsFeatureBlocked");
    _fwd_SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsFeatureInBlockList");
    _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsParentalLockEnabled");
    _fwd_SteamAPI_ISteamParentalSettings_BIsParentalLockLocked = dlsym(real_lib, "SteamAPI_ISteamParentalSettings_BIsParentalLockLocked");
    _fwd_SteamAPI_ISteamParties_CancelReservation = dlsym(real_lib, "SteamAPI_ISteamParties_CancelReservation");
    _fwd_SteamAPI_ISteamParties_ChangeNumOpenSlots = dlsym(real_lib, "SteamAPI_ISteamParties_ChangeNumOpenSlots");
    _fwd_SteamAPI_ISteamParties_CreateBeacon = dlsym(real_lib, "SteamAPI_ISteamParties_CreateBeacon");
    _fwd_SteamAPI_ISteamParties_DestroyBeacon = dlsym(real_lib, "SteamAPI_ISteamParties_DestroyBeacon");
    _fwd_SteamAPI_ISteamParties_GetAvailableBeaconLocations = dlsym(real_lib, "SteamAPI_ISteamParties_GetAvailableBeaconLocations");
    _fwd_SteamAPI_ISteamParties_GetBeaconByIndex = dlsym(real_lib, "SteamAPI_ISteamParties_GetBeaconByIndex");
    _fwd_SteamAPI_ISteamParties_GetBeaconDetails = dlsym(real_lib, "SteamAPI_ISteamParties_GetBeaconDetails");
    _fwd_SteamAPI_ISteamParties_GetBeaconLocationData = dlsym(real_lib, "SteamAPI_ISteamParties_GetBeaconLocationData");
    _fwd_SteamAPI_ISteamParties_GetNumActiveBeacons = dlsym(real_lib, "SteamAPI_ISteamParties_GetNumActiveBeacons");
    _fwd_SteamAPI_ISteamParties_GetNumAvailableBeaconLocations = dlsym(real_lib, "SteamAPI_ISteamParties_GetNumAvailableBeaconLocations");
    _fwd_SteamAPI_ISteamParties_JoinParty = dlsym(real_lib, "SteamAPI_ISteamParties_JoinParty");
    _fwd_SteamAPI_ISteamParties_OnReservationCompleted = dlsym(real_lib, "SteamAPI_ISteamParties_OnReservationCompleted");
    _fwd_SteamAPI_ISteamRemotePlay_BGetSessionClientResolution = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_BGetSessionClientResolution");
    _fwd_SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_BSendRemotePlayTogetherInvite");
    _fwd_SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_BStartRemotePlayTogether");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_GetSessionClientFormFactor");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionClientName = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_GetSessionClientName");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionCount = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_GetSessionCount");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionID = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_GetSessionID");
    _fwd_SteamAPI_ISteamRemotePlay_GetSessionSteamID = dlsym(real_lib, "SteamAPI_ISteamRemotePlay_GetSessionSteamID");
    _fwd_SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_BeginFileWriteBatch");
    _fwd_SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_CommitPublishedFileUpdate");
    _fwd_SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_CreatePublishedFileUpdateRequest");
    _fwd_SteamAPI_ISteamRemoteStorage_DeletePublishedFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_DeletePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_EndFileWriteBatch = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EndFileWriteBatch");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EnumeratePublishedFilesByUserAction");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EnumeratePublishedWorkshopFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserPublishedFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserSharedWorkshopFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles");
    _fwd_SteamAPI_ISteamRemoteStorage_FileDelete = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileDelete");
    _fwd_SteamAPI_ISteamRemoteStorage_FileExists = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileExists");
    _fwd_SteamAPI_ISteamRemoteStorage_FileForget = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileForget");
    _fwd_SteamAPI_ISteamRemoteStorage_FilePersisted = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FilePersisted");
    _fwd_SteamAPI_ISteamRemoteStorage_FileRead = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileRead");
    _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsync = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileReadAsync");
    _fwd_SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileReadAsyncComplete");
    _fwd_SteamAPI_ISteamRemoteStorage_FileShare = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileShare");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWrite = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWrite");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteAsync = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteAsync");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamCancel");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamClose = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamClose");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamOpen");
    _fwd_SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_FileWriteStreamWriteChunk");
    _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCCount = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetCachedUGCCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetCachedUGCHandle");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileCount = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetFileCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileNameAndSize = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileSize = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetFileSize");
    _fwd_SteamAPI_ISteamRemoteStorage_GetFileTimestamp = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetFileTimestamp");
    _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChange = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetLocalFileChange");
    _fwd_SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetLocalFileChangeCount");
    _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetPublishedItemVoteDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetQuota = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetQuota");
    _fwd_SteamAPI_ISteamRemoteStorage_GetSyncPlatforms = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetSyncPlatforms");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDetails = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetUGCDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetUGCDownloadProgress");
    _fwd_SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_GetUserPublishedItemVoteDetails");
    _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount");
    _fwd_SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp");
    _fwd_SteamAPI_ISteamRemoteStorage_PublishVideo = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_PublishVideo");
    _fwd_SteamAPI_ISteamRemoteStorage_PublishWorkshopFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_PublishWorkshopFile");
    _fwd_SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_SetCloudEnabledForApp");
    _fwd_SteamAPI_ISteamRemoteStorage_SetSyncPlatforms = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_SetSyncPlatforms");
    _fwd_SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_SetUserPublishedFileAction");
    _fwd_SteamAPI_ISteamRemoteStorage_SubscribePublishedFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_SubscribePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCDownload = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UGCDownload");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UGCDownloadToLocation");
    _fwd_SteamAPI_ISteamRemoteStorage_UGCRead = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UGCRead");
    _fwd_SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UnsubscribePublishedFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileDescription");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFilePreviewFile");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileSetChangeDescription");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTags");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileTitle");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdatePublishedFileVisibility");
    _fwd_SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote = dlsym(real_lib, "SteamAPI_ISteamRemoteStorage_UpdateUserPublishedItemVote");
    _fwd_SteamAPI_ISteamScreenshots_AddScreenshotToLibrary = dlsym(real_lib, "SteamAPI_ISteamScreenshots_AddScreenshotToLibrary");
    _fwd_SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary = dlsym(real_lib, "SteamAPI_ISteamScreenshots_AddVRScreenshotToLibrary");
    _fwd_SteamAPI_ISteamScreenshots_HookScreenshots = dlsym(real_lib, "SteamAPI_ISteamScreenshots_HookScreenshots");
    _fwd_SteamAPI_ISteamScreenshots_IsScreenshotsHooked = dlsym(real_lib, "SteamAPI_ISteamScreenshots_IsScreenshotsHooked");
    _fwd_SteamAPI_ISteamScreenshots_SetLocation = dlsym(real_lib, "SteamAPI_ISteamScreenshots_SetLocation");
    _fwd_SteamAPI_ISteamScreenshots_TagPublishedFile = dlsym(real_lib, "SteamAPI_ISteamScreenshots_TagPublishedFile");
    _fwd_SteamAPI_ISteamScreenshots_TagUser = dlsym(real_lib, "SteamAPI_ISteamScreenshots_TagUser");
    _fwd_SteamAPI_ISteamScreenshots_TriggerScreenshot = dlsym(real_lib, "SteamAPI_ISteamScreenshots_TriggerScreenshot");
    _fwd_SteamAPI_ISteamScreenshots_WriteScreenshot = dlsym(real_lib, "SteamAPI_ISteamScreenshots_WriteScreenshot");
    _fwd_SteamAPI_ISteamTimeline_AddGamePhaseTag = dlsym(real_lib, "SteamAPI_ISteamTimeline_AddGamePhaseTag");
    _fwd_SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_AddInstantaneousTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_AddRangeTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_AddRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_ClearTimelineTooltip = dlsym(real_lib, "SteamAPI_ISteamTimeline_ClearTimelineTooltip");
    _fwd_SteamAPI_ISteamTimeline_DoesEventRecordingExist = dlsym(real_lib, "SteamAPI_ISteamTimeline_DoesEventRecordingExist");
    _fwd_SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist = dlsym(real_lib, "SteamAPI_ISteamTimeline_DoesGamePhaseRecordingExist");
    _fwd_SteamAPI_ISteamTimeline_EndGamePhase = dlsym(real_lib, "SteamAPI_ISteamTimeline_EndGamePhase");
    _fwd_SteamAPI_ISteamTimeline_EndRangeTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_EndRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_OpenOverlayToGamePhase = dlsym(real_lib, "SteamAPI_ISteamTimeline_OpenOverlayToGamePhase");
    _fwd_SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_OpenOverlayToTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_RemoveTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_RemoveTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_SetGamePhaseAttribute = dlsym(real_lib, "SteamAPI_ISteamTimeline_SetGamePhaseAttribute");
    _fwd_SteamAPI_ISteamTimeline_SetGamePhaseID = dlsym(real_lib, "SteamAPI_ISteamTimeline_SetGamePhaseID");
    _fwd_SteamAPI_ISteamTimeline_SetTimelineGameMode = dlsym(real_lib, "SteamAPI_ISteamTimeline_SetTimelineGameMode");
    _fwd_SteamAPI_ISteamTimeline_SetTimelineTooltip = dlsym(real_lib, "SteamAPI_ISteamTimeline_SetTimelineTooltip");
    _fwd_SteamAPI_ISteamTimeline_StartGamePhase = dlsym(real_lib, "SteamAPI_ISteamTimeline_StartGamePhase");
    _fwd_SteamAPI_ISteamTimeline_StartRangeTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_StartRangeTimelineEvent");
    _fwd_SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent = dlsym(real_lib, "SteamAPI_ISteamTimeline_UpdateRangeTimelineEvent");
    _fwd_SteamAPI_ISteamUGC_AddAppDependency = dlsym(real_lib, "SteamAPI_ISteamUGC_AddAppDependency");
    _fwd_SteamAPI_ISteamUGC_AddContentDescriptor = dlsym(real_lib, "SteamAPI_ISteamUGC_AddContentDescriptor");
    _fwd_SteamAPI_ISteamUGC_AddDependency = dlsym(real_lib, "SteamAPI_ISteamUGC_AddDependency");
    _fwd_SteamAPI_ISteamUGC_AddExcludedTag = dlsym(real_lib, "SteamAPI_ISteamUGC_AddExcludedTag");
    _fwd_SteamAPI_ISteamUGC_AddItemKeyValueTag = dlsym(real_lib, "SteamAPI_ISteamUGC_AddItemKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_AddItemPreviewFile = dlsym(real_lib, "SteamAPI_ISteamUGC_AddItemPreviewFile");
    _fwd_SteamAPI_ISteamUGC_AddItemPreviewVideo = dlsym(real_lib, "SteamAPI_ISteamUGC_AddItemPreviewVideo");
    _fwd_SteamAPI_ISteamUGC_AddItemToFavorites = dlsym(real_lib, "SteamAPI_ISteamUGC_AddItemToFavorites");
    _fwd_SteamAPI_ISteamUGC_AddRequiredKeyValueTag = dlsym(real_lib, "SteamAPI_ISteamUGC_AddRequiredKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_AddRequiredTag = dlsym(real_lib, "SteamAPI_ISteamUGC_AddRequiredTag");
    _fwd_SteamAPI_ISteamUGC_AddRequiredTagGroup = dlsym(real_lib, "SteamAPI_ISteamUGC_AddRequiredTagGroup");
    _fwd_SteamAPI_ISteamUGC_BInitWorkshopForGameServer = dlsym(real_lib, "SteamAPI_ISteamUGC_BInitWorkshopForGameServer");
    _fwd_SteamAPI_ISteamUGC_CreateItem = dlsym(real_lib, "SteamAPI_ISteamUGC_CreateItem");
    _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor = dlsym(real_lib, "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestCursor");
    _fwd_SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage = dlsym(real_lib, "SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage");
    _fwd_SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest = dlsym(real_lib, "SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest");
    _fwd_SteamAPI_ISteamUGC_CreateQueryUserUGCRequest = dlsym(real_lib, "SteamAPI_ISteamUGC_CreateQueryUserUGCRequest");
    _fwd_SteamAPI_ISteamUGC_DeleteItem = dlsym(real_lib, "SteamAPI_ISteamUGC_DeleteItem");
    _fwd_SteamAPI_ISteamUGC_DownloadItem = dlsym(real_lib, "SteamAPI_ISteamUGC_DownloadItem");
    _fwd_SteamAPI_ISteamUGC_GetAppDependencies = dlsym(real_lib, "SteamAPI_ISteamUGC_GetAppDependencies");
    _fwd_SteamAPI_ISteamUGC_GetItemDownloadInfo = dlsym(real_lib, "SteamAPI_ISteamUGC_GetItemDownloadInfo");
    _fwd_SteamAPI_ISteamUGC_GetItemInstallInfo = dlsym(real_lib, "SteamAPI_ISteamUGC_GetItemInstallInfo");
    _fwd_SteamAPI_ISteamUGC_GetItemState = dlsym(real_lib, "SteamAPI_ISteamUGC_GetItemState");
    _fwd_SteamAPI_ISteamUGC_GetItemUpdateProgress = dlsym(real_lib, "SteamAPI_ISteamUGC_GetItemUpdateProgress");
    _fwd_SteamAPI_ISteamUGC_GetNumSubscribedItems = dlsym(real_lib, "SteamAPI_ISteamUGC_GetNumSubscribedItems");
    _fwd_SteamAPI_ISteamUGC_GetNumSupportedGameVersions = dlsym(real_lib, "SteamAPI_ISteamUGC_GetNumSupportedGameVersions");
    _fwd_SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryFirstUGCKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCAdditionalPreview");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCChildren = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCChildren");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCContentDescriptors");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCKeyValueTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCMetadata = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCMetadata");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumAdditionalPreviews");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCNumTags = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCNumTags");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCPreviewURL = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCPreviewURL");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCResult = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCResult");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCStatistic = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCStatistic");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCTag = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCTag");
    _fwd_SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName = dlsym(real_lib, "SteamAPI_ISteamUGC_GetQueryUGCTagDisplayName");
    _fwd_SteamAPI_ISteamUGC_GetSubscribedItems = dlsym(real_lib, "SteamAPI_ISteamUGC_GetSubscribedItems");
    _fwd_SteamAPI_ISteamUGC_GetSupportedGameVersionData = dlsym(real_lib, "SteamAPI_ISteamUGC_GetSupportedGameVersionData");
    _fwd_SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences = dlsym(real_lib, "SteamAPI_ISteamUGC_GetUserContentDescriptorPreferences");
    _fwd_SteamAPI_ISteamUGC_GetUserItemVote = dlsym(real_lib, "SteamAPI_ISteamUGC_GetUserItemVote");
    _fwd_SteamAPI_ISteamUGC_GetWorkshopEULAStatus = dlsym(real_lib, "SteamAPI_ISteamUGC_GetWorkshopEULAStatus");
    _fwd_SteamAPI_ISteamUGC_ReleaseQueryUGCRequest = dlsym(real_lib, "SteamAPI_ISteamUGC_ReleaseQueryUGCRequest");
    _fwd_SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveAllItemKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_RemoveAppDependency = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveAppDependency");
    _fwd_SteamAPI_ISteamUGC_RemoveContentDescriptor = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveContentDescriptor");
    _fwd_SteamAPI_ISteamUGC_RemoveDependency = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveDependency");
    _fwd_SteamAPI_ISteamUGC_RemoveItemFromFavorites = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveItemFromFavorites");
    _fwd_SteamAPI_ISteamUGC_RemoveItemKeyValueTags = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveItemKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_RemoveItemPreview = dlsym(real_lib, "SteamAPI_ISteamUGC_RemoveItemPreview");
    _fwd_SteamAPI_ISteamUGC_RequestUGCDetails = dlsym(real_lib, "SteamAPI_ISteamUGC_RequestUGCDetails");
    _fwd_SteamAPI_ISteamUGC_SendQueryUGCRequest = dlsym(real_lib, "SteamAPI_ISteamUGC_SendQueryUGCRequest");
    _fwd_SteamAPI_ISteamUGC_SetAdminQuery = dlsym(real_lib, "SteamAPI_ISteamUGC_SetAdminQuery");
    _fwd_SteamAPI_ISteamUGC_SetAllowCachedResponse = dlsym(real_lib, "SteamAPI_ISteamUGC_SetAllowCachedResponse");
    _fwd_SteamAPI_ISteamUGC_SetAllowLegacyUpload = dlsym(real_lib, "SteamAPI_ISteamUGC_SetAllowLegacyUpload");
    _fwd_SteamAPI_ISteamUGC_SetCloudFileNameFilter = dlsym(real_lib, "SteamAPI_ISteamUGC_SetCloudFileNameFilter");
    _fwd_SteamAPI_ISteamUGC_SetItemContent = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemContent");
    _fwd_SteamAPI_ISteamUGC_SetItemDescription = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemDescription");
    _fwd_SteamAPI_ISteamUGC_SetItemMetadata = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemMetadata");
    _fwd_SteamAPI_ISteamUGC_SetItemPreview = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemPreview");
    _fwd_SteamAPI_ISteamUGC_SetItemTags = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemTags");
    _fwd_SteamAPI_ISteamUGC_SetItemTitle = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemTitle");
    _fwd_SteamAPI_ISteamUGC_SetItemUpdateLanguage = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemUpdateLanguage");
    _fwd_SteamAPI_ISteamUGC_SetItemVisibility = dlsym(real_lib, "SteamAPI_ISteamUGC_SetItemVisibility");
    _fwd_SteamAPI_ISteamUGC_SetLanguage = dlsym(real_lib, "SteamAPI_ISteamUGC_SetLanguage");
    _fwd_SteamAPI_ISteamUGC_SetMatchAnyTag = dlsym(real_lib, "SteamAPI_ISteamUGC_SetMatchAnyTag");
    _fwd_SteamAPI_ISteamUGC_SetRankedByTrendDays = dlsym(real_lib, "SteamAPI_ISteamUGC_SetRankedByTrendDays");
    _fwd_SteamAPI_ISteamUGC_SetRequiredGameVersions = dlsym(real_lib, "SteamAPI_ISteamUGC_SetRequiredGameVersions");
    _fwd_SteamAPI_ISteamUGC_SetReturnAdditionalPreviews = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnAdditionalPreviews");
    _fwd_SteamAPI_ISteamUGC_SetReturnChildren = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnChildren");
    _fwd_SteamAPI_ISteamUGC_SetReturnKeyValueTags = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnKeyValueTags");
    _fwd_SteamAPI_ISteamUGC_SetReturnLongDescription = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnLongDescription");
    _fwd_SteamAPI_ISteamUGC_SetReturnMetadata = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnMetadata");
    _fwd_SteamAPI_ISteamUGC_SetReturnOnlyIDs = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnOnlyIDs");
    _fwd_SteamAPI_ISteamUGC_SetReturnPlaytimeStats = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnPlaytimeStats");
    _fwd_SteamAPI_ISteamUGC_SetReturnTotalOnly = dlsym(real_lib, "SteamAPI_ISteamUGC_SetReturnTotalOnly");
    _fwd_SteamAPI_ISteamUGC_SetSearchText = dlsym(real_lib, "SteamAPI_ISteamUGC_SetSearchText");
    _fwd_SteamAPI_ISteamUGC_SetTimeCreatedDateRange = dlsym(real_lib, "SteamAPI_ISteamUGC_SetTimeCreatedDateRange");
    _fwd_SteamAPI_ISteamUGC_SetTimeUpdatedDateRange = dlsym(real_lib, "SteamAPI_ISteamUGC_SetTimeUpdatedDateRange");
    _fwd_SteamAPI_ISteamUGC_SetUserItemVote = dlsym(real_lib, "SteamAPI_ISteamUGC_SetUserItemVote");
    _fwd_SteamAPI_ISteamUGC_ShowWorkshopEULA = dlsym(real_lib, "SteamAPI_ISteamUGC_ShowWorkshopEULA");
    _fwd_SteamAPI_ISteamUGC_StartItemUpdate = dlsym(real_lib, "SteamAPI_ISteamUGC_StartItemUpdate");
    _fwd_SteamAPI_ISteamUGC_StartPlaytimeTracking = dlsym(real_lib, "SteamAPI_ISteamUGC_StartPlaytimeTracking");
    _fwd_SteamAPI_ISteamUGC_StopPlaytimeTracking = dlsym(real_lib, "SteamAPI_ISteamUGC_StopPlaytimeTracking");
    _fwd_SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems = dlsym(real_lib, "SteamAPI_ISteamUGC_StopPlaytimeTrackingForAllItems");
    _fwd_SteamAPI_ISteamUGC_SubmitItemUpdate = dlsym(real_lib, "SteamAPI_ISteamUGC_SubmitItemUpdate");
    _fwd_SteamAPI_ISteamUGC_SubscribeItem = dlsym(real_lib, "SteamAPI_ISteamUGC_SubscribeItem");
    _fwd_SteamAPI_ISteamUGC_SuspendDownloads = dlsym(real_lib, "SteamAPI_ISteamUGC_SuspendDownloads");
    _fwd_SteamAPI_ISteamUGC_UnsubscribeItem = dlsym(real_lib, "SteamAPI_ISteamUGC_UnsubscribeItem");
    _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewFile = dlsym(real_lib, "SteamAPI_ISteamUGC_UpdateItemPreviewFile");
    _fwd_SteamAPI_ISteamUGC_UpdateItemPreviewVideo = dlsym(real_lib, "SteamAPI_ISteamUGC_UpdateItemPreviewVideo");
    _fwd_SteamAPI_ISteamUser_AdvertiseGame = dlsym(real_lib, "SteamAPI_ISteamUser_AdvertiseGame");
    _fwd_SteamAPI_ISteamUser_BeginAuthSession = dlsym(real_lib, "SteamAPI_ISteamUser_BeginAuthSession");
    _fwd_SteamAPI_ISteamUser_BIsBehindNAT = dlsym(real_lib, "SteamAPI_ISteamUser_BIsBehindNAT");
    _fwd_SteamAPI_ISteamUser_BIsPhoneIdentifying = dlsym(real_lib, "SteamAPI_ISteamUser_BIsPhoneIdentifying");
    _fwd_SteamAPI_ISteamUser_BIsPhoneRequiringVerification = dlsym(real_lib, "SteamAPI_ISteamUser_BIsPhoneRequiringVerification");
    _fwd_SteamAPI_ISteamUser_BIsPhoneVerified = dlsym(real_lib, "SteamAPI_ISteamUser_BIsPhoneVerified");
    _fwd_SteamAPI_ISteamUser_BIsTwoFactorEnabled = dlsym(real_lib, "SteamAPI_ISteamUser_BIsTwoFactorEnabled");
    _fwd_SteamAPI_ISteamUser_BLoggedOn = dlsym(real_lib, "SteamAPI_ISteamUser_BLoggedOn");
    _fwd_SteamAPI_ISteamUser_BSetDurationControlOnlineState = dlsym(real_lib, "SteamAPI_ISteamUser_BSetDurationControlOnlineState");
    _fwd_SteamAPI_ISteamUser_CancelAuthTicket = dlsym(real_lib, "SteamAPI_ISteamUser_CancelAuthTicket");
    _fwd_SteamAPI_ISteamUser_DecompressVoice = dlsym(real_lib, "SteamAPI_ISteamUser_DecompressVoice");
    _fwd_SteamAPI_ISteamUser_EndAuthSession = dlsym(real_lib, "SteamAPI_ISteamUser_EndAuthSession");
    _fwd_SteamAPI_ISteamUser_GetAuthSessionTicket = dlsym(real_lib, "SteamAPI_ISteamUser_GetAuthSessionTicket");
    _fwd_SteamAPI_ISteamUser_GetAuthTicketForWebApi = dlsym(real_lib, "SteamAPI_ISteamUser_GetAuthTicketForWebApi");
    _fwd_SteamAPI_ISteamUser_GetAvailableVoice = dlsym(real_lib, "SteamAPI_ISteamUser_GetAvailableVoice");
    _fwd_SteamAPI_ISteamUser_GetDurationControl = dlsym(real_lib, "SteamAPI_ISteamUser_GetDurationControl");
    _fwd_SteamAPI_ISteamUser_GetEncryptedAppTicket = dlsym(real_lib, "SteamAPI_ISteamUser_GetEncryptedAppTicket");
    _fwd_SteamAPI_ISteamUser_GetGameBadgeLevel = dlsym(real_lib, "SteamAPI_ISteamUser_GetGameBadgeLevel");
    _fwd_SteamAPI_ISteamUser_GetHSteamUser = dlsym(real_lib, "SteamAPI_ISteamUser_GetHSteamUser");
    _fwd_SteamAPI_ISteamUser_GetMarketEligibility = dlsym(real_lib, "SteamAPI_ISteamUser_GetMarketEligibility");
    _fwd_SteamAPI_ISteamUser_GetPlayerSteamLevel = dlsym(real_lib, "SteamAPI_ISteamUser_GetPlayerSteamLevel");
    _fwd_SteamAPI_ISteamUser_GetSteamID = dlsym(real_lib, "SteamAPI_ISteamUser_GetSteamID");
    _fwd_SteamAPI_ISteamUser_GetUserDataFolder = dlsym(real_lib, "SteamAPI_ISteamUser_GetUserDataFolder");
    _fwd_SteamAPI_ISteamUser_GetVoice = dlsym(real_lib, "SteamAPI_ISteamUser_GetVoice");
    _fwd_SteamAPI_ISteamUser_GetVoiceOptimalSampleRate = dlsym(real_lib, "SteamAPI_ISteamUser_GetVoiceOptimalSampleRate");
    _fwd_SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED = dlsym(real_lib, "SteamAPI_ISteamUser_InitiateGameConnection_DEPRECATED");
    _fwd_SteamAPI_ISteamUser_RequestEncryptedAppTicket = dlsym(real_lib, "SteamAPI_ISteamUser_RequestEncryptedAppTicket");
    _fwd_SteamAPI_ISteamUser_RequestStoreAuthURL = dlsym(real_lib, "SteamAPI_ISteamUser_RequestStoreAuthURL");
    _fwd_SteamAPI_ISteamUser_StartVoiceRecording = dlsym(real_lib, "SteamAPI_ISteamUser_StartVoiceRecording");
    _fwd_SteamAPI_ISteamUserStats_AttachLeaderboardUGC = dlsym(real_lib, "SteamAPI_ISteamUserStats_AttachLeaderboardUGC");
    _fwd_SteamAPI_ISteamUserStats_ClearAchievement = dlsym(real_lib, "SteamAPI_ISteamUserStats_ClearAchievement");
    _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntries = dlsym(real_lib, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntries");
    _fwd_SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers = dlsym(real_lib, "SteamAPI_ISteamUserStats_DownloadLeaderboardEntriesForUsers");
    _fwd_SteamAPI_ISteamUserStats_FindLeaderboard = dlsym(real_lib, "SteamAPI_ISteamUserStats_FindLeaderboard");
    _fwd_SteamAPI_ISteamUserStats_FindOrCreateLeaderboard = dlsym(real_lib, "SteamAPI_ISteamUserStats_FindOrCreateLeaderboard");
    _fwd_SteamAPI_ISteamUserStats_GetAchievement = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievement");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementAchievedPercent = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementAchievedPercent");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementIcon = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementIcon");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementName = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementName");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsFloat");
    _fwd_SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32 = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetAchievementProgressLimitsInt32");
    _fwd_SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatDouble = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatDouble");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatHistoryDouble");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64 = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatHistoryInt64");
    _fwd_SteamAPI_ISteamUserStats_GetGlobalStatInt64 = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetGlobalStatInt64");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardDisplayType = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardDisplayType");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardEntryCount = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardEntryCount");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardName = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardName");
    _fwd_SteamAPI_ISteamUserStats_GetLeaderboardSortMethod = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetLeaderboardSortMethod");
    _fwd_SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetMostAchievedAchievementInfo");
    _fwd_SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetNextMostAchievedAchievementInfo");
    _fwd_SteamAPI_ISteamUserStats_GetNumAchievements = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetNumAchievements");
    _fwd_SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetNumberOfCurrentPlayers");
    _fwd_SteamAPI_ISteamUserStats_GetStatFloat = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetStatFloat");
    _fwd_SteamAPI_ISteamUserStats_GetStatInt32 = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetStatInt32");
    _fwd_SteamAPI_ISteamUserStats_GetUserAchievement = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetUserAchievement");
    _fwd_SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetUserAchievementAndUnlockTime");
    _fwd_SteamAPI_ISteamUserStats_GetUserStatFloat = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetUserStatFloat");
    _fwd_SteamAPI_ISteamUserStats_GetUserStatInt32 = dlsym(real_lib, "SteamAPI_ISteamUserStats_GetUserStatInt32");
    _fwd_SteamAPI_ISteamUserStats_IndicateAchievementProgress = dlsym(real_lib, "SteamAPI_ISteamUserStats_IndicateAchievementProgress");
    _fwd_SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages = dlsym(real_lib, "SteamAPI_ISteamUserStats_RequestGlobalAchievementPercentages");
    _fwd_SteamAPI_ISteamUserStats_RequestGlobalStats = dlsym(real_lib, "SteamAPI_ISteamUserStats_RequestGlobalStats");
    _fwd_SteamAPI_ISteamUserStats_RequestUserStats = dlsym(real_lib, "SteamAPI_ISteamUserStats_RequestUserStats");
    _fwd_SteamAPI_ISteamUserStats_ResetAllStats = dlsym(real_lib, "SteamAPI_ISteamUserStats_ResetAllStats");
    _fwd_SteamAPI_ISteamUserStats_SetAchievement = dlsym(real_lib, "SteamAPI_ISteamUserStats_SetAchievement");
    _fwd_SteamAPI_ISteamUserStats_SetStatFloat = dlsym(real_lib, "SteamAPI_ISteamUserStats_SetStatFloat");
    _fwd_SteamAPI_ISteamUserStats_SetStatInt32 = dlsym(real_lib, "SteamAPI_ISteamUserStats_SetStatInt32");
    _fwd_SteamAPI_ISteamUserStats_StoreStats = dlsym(real_lib, "SteamAPI_ISteamUserStats_StoreStats");
    _fwd_SteamAPI_ISteamUserStats_UpdateAvgRateStat = dlsym(real_lib, "SteamAPI_ISteamUserStats_UpdateAvgRateStat");
    _fwd_SteamAPI_ISteamUserStats_UploadLeaderboardScore = dlsym(real_lib, "SteamAPI_ISteamUserStats_UploadLeaderboardScore");
    _fwd_SteamAPI_ISteamUser_StopVoiceRecording = dlsym(real_lib, "SteamAPI_ISteamUser_StopVoiceRecording");
    _fwd_SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED = dlsym(real_lib, "SteamAPI_ISteamUser_TerminateGameConnection_DEPRECATED");
    _fwd_SteamAPI_ISteamUser_TrackAppUsageEvent = dlsym(real_lib, "SteamAPI_ISteamUser_TrackAppUsageEvent");
    _fwd_SteamAPI_ISteamUtils_BOverlayNeedsPresent = dlsym(real_lib, "SteamAPI_ISteamUtils_BOverlayNeedsPresent");
    _fwd_SteamAPI_ISteamUtils_CheckFileSignature = dlsym(real_lib, "SteamAPI_ISteamUtils_CheckFileSignature");
    _fwd_SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput = dlsym(real_lib, "SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_DismissGamepadTextInput = dlsym(real_lib, "SteamAPI_ISteamUtils_DismissGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_FilterText = dlsym(real_lib, "SteamAPI_ISteamUtils_FilterText");
    _fwd_SteamAPI_ISteamUtils_GetAPICallFailureReason = dlsym(real_lib, "SteamAPI_ISteamUtils_GetAPICallFailureReason");
    _fwd_SteamAPI_ISteamUtils_GetAPICallResult = dlsym(real_lib, "SteamAPI_ISteamUtils_GetAPICallResult");
    _fwd_SteamAPI_ISteamUtils_GetAppID = dlsym(real_lib, "SteamAPI_ISteamUtils_GetAppID");
    _fwd_SteamAPI_ISteamUtils_GetConnectedUniverse = dlsym(real_lib, "SteamAPI_ISteamUtils_GetConnectedUniverse");
    _fwd_SteamAPI_ISteamUtils_GetCurrentBatteryPower = dlsym(real_lib, "SteamAPI_ISteamUtils_GetCurrentBatteryPower");
    _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextInput = dlsym(real_lib, "SteamAPI_ISteamUtils_GetEnteredGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_GetEnteredGamepadTextLength = dlsym(real_lib, "SteamAPI_ISteamUtils_GetEnteredGamepadTextLength");
    _fwd_SteamAPI_ISteamUtils_GetImageRGBA = dlsym(real_lib, "SteamAPI_ISteamUtils_GetImageRGBA");
    _fwd_SteamAPI_ISteamUtils_GetImageSize = dlsym(real_lib, "SteamAPI_ISteamUtils_GetImageSize");
    _fwd_SteamAPI_ISteamUtils_GetIPCCallCount = dlsym(real_lib, "SteamAPI_ISteamUtils_GetIPCCallCount");
    _fwd_SteamAPI_ISteamUtils_GetIPCountry = dlsym(real_lib, "SteamAPI_ISteamUtils_GetIPCountry");
    _fwd_SteamAPI_ISteamUtils_GetIPv6ConnectivityState = dlsym(real_lib, "SteamAPI_ISteamUtils_GetIPv6ConnectivityState");
    _fwd_SteamAPI_ISteamUtils_GetSecondsSinceAppActive = dlsym(real_lib, "SteamAPI_ISteamUtils_GetSecondsSinceAppActive");
    _fwd_SteamAPI_ISteamUtils_GetSecondsSinceComputerActive = dlsym(real_lib, "SteamAPI_ISteamUtils_GetSecondsSinceComputerActive");
    _fwd_SteamAPI_ISteamUtils_GetServerRealTime = dlsym(real_lib, "SteamAPI_ISteamUtils_GetServerRealTime");
    _fwd_SteamAPI_ISteamUtils_GetSteamUILanguage = dlsym(real_lib, "SteamAPI_ISteamUtils_GetSteamUILanguage");
    _fwd_SteamAPI_ISteamUtils_InitFilterText = dlsym(real_lib, "SteamAPI_ISteamUtils_InitFilterText");
    _fwd_SteamAPI_ISteamUtils_IsAPICallCompleted = dlsym(real_lib, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    _fwd_SteamAPI_ISteamUtils_IsOverlayEnabled = dlsym(real_lib, "SteamAPI_ISteamUtils_IsOverlayEnabled");
    _fwd_SteamAPI_ISteamUtils_IsSteamChinaLauncher = dlsym(real_lib, "SteamAPI_ISteamUtils_IsSteamChinaLauncher");
    _fwd_SteamAPI_ISteamUtils_IsSteamInBigPictureMode = dlsym(real_lib, "SteamAPI_ISteamUtils_IsSteamInBigPictureMode");
    _fwd_SteamAPI_ISteamUtils_IsSteamRunningInVR = dlsym(real_lib, "SteamAPI_ISteamUtils_IsSteamRunningInVR");
    _fwd_SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck = dlsym(real_lib, "SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck");
    _fwd_SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled = dlsym(real_lib, "SteamAPI_ISteamUtils_IsVRHeadsetStreamingEnabled");
    _fwd_SteamAPI_ISteamUtils_SetGameLauncherMode = dlsym(real_lib, "SteamAPI_ISteamUtils_SetGameLauncherMode");
    _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationInset = dlsym(real_lib, "SteamAPI_ISteamUtils_SetOverlayNotificationInset");
    _fwd_SteamAPI_ISteamUtils_SetOverlayNotificationPosition = dlsym(real_lib, "SteamAPI_ISteamUtils_SetOverlayNotificationPosition");
    _fwd_SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled = dlsym(real_lib, "SteamAPI_ISteamUtils_SetVRHeadsetStreamingEnabled");
    _fwd_SteamAPI_ISteamUtils_SetWarningMessageHook = dlsym(real_lib, "SteamAPI_ISteamUtils_SetWarningMessageHook");
    _fwd_SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput = dlsym(real_lib, "SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_ShowGamepadTextInput = dlsym(real_lib, "SteamAPI_ISteamUtils_ShowGamepadTextInput");
    _fwd_SteamAPI_ISteamUtils_StartVRDashboard = dlsym(real_lib, "SteamAPI_ISteamUtils_StartVRDashboard");
    _fwd_SteamAPI_ISteamVideo_GetOPFSettings = dlsym(real_lib, "SteamAPI_ISteamVideo_GetOPFSettings");
    _fwd_SteamAPI_ISteamVideo_GetOPFStringForApp = dlsym(real_lib, "SteamAPI_ISteamVideo_GetOPFStringForApp");
    _fwd_SteamAPI_ISteamVideo_GetVideoURL = dlsym(real_lib, "SteamAPI_ISteamVideo_GetVideoURL");
    _fwd_SteamAPI_ISteamVideo_IsBroadcasting = dlsym(real_lib, "SteamAPI_ISteamVideo_IsBroadcasting");
    _fwd_SteamAPI_ManualDispatch_FreeLastCallback = dlsym(real_lib, "SteamAPI_ManualDispatch_FreeLastCallback");
    _fwd_SteamAPI_ManualDispatch_GetAPICallResult = dlsym(real_lib, "SteamAPI_ManualDispatch_GetAPICallResult");
    _fwd_SteamAPI_ManualDispatch_GetNextCallback = dlsym(real_lib, "SteamAPI_ManualDispatch_GetNextCallback");
    _fwd_SteamAPI_ManualDispatch_Init = dlsym(real_lib, "SteamAPI_ManualDispatch_Init");
    _fwd_SteamAPI_ManualDispatch_RunFrame = dlsym(real_lib, "SteamAPI_ManualDispatch_RunFrame");
    _fwd_SteamAPI_MatchMakingKeyValuePair_t_Construct = dlsym(real_lib, "SteamAPI_MatchMakingKeyValuePair_t_Construct");
    _fwd_SteamAPI_RegisterCallback = dlsym(real_lib, "SteamAPI_RegisterCallback");
    _fwd_SteamAPI_RegisterCallResult = dlsym(real_lib, "SteamAPI_RegisterCallResult");
    _fwd_SteamAPI_ReleaseCurrentThreadMemory = dlsym(real_lib, "SteamAPI_ReleaseCurrentThreadMemory");
    _fwd_SteamAPI_RestartAppIfNecessary = dlsym(real_lib, "SteamAPI_RestartAppIfNecessary");
    _fwd_SteamAPI_RunCallbacks = dlsym(real_lib, "SteamAPI_RunCallbacks");
    _fwd_SteamAPI_servernetadr_t_Assign = dlsym(real_lib, "SteamAPI_servernetadr_t_Assign");
    _fwd_SteamAPI_servernetadr_t_Construct = dlsym(real_lib, "SteamAPI_servernetadr_t_Construct");
    _fwd_SteamAPI_servernetadr_t_GetConnectionAddressString = dlsym(real_lib, "SteamAPI_servernetadr_t_GetConnectionAddressString");
    _fwd_SteamAPI_servernetadr_t_GetConnectionPort = dlsym(real_lib, "SteamAPI_servernetadr_t_GetConnectionPort");
    _fwd_SteamAPI_servernetadr_t_GetIP = dlsym(real_lib, "SteamAPI_servernetadr_t_GetIP");
    _fwd_SteamAPI_servernetadr_t_GetQueryAddressString = dlsym(real_lib, "SteamAPI_servernetadr_t_GetQueryAddressString");
    _fwd_SteamAPI_servernetadr_t_GetQueryPort = dlsym(real_lib, "SteamAPI_servernetadr_t_GetQueryPort");
    _fwd_SteamAPI_servernetadr_t_Init = dlsym(real_lib, "SteamAPI_servernetadr_t_Init");
    _fwd_SteamAPI_servernetadr_t_IsLessThan = dlsym(real_lib, "SteamAPI_servernetadr_t_IsLessThan");
    _fwd_SteamAPI_servernetadr_t_SetConnectionPort = dlsym(real_lib, "SteamAPI_servernetadr_t_SetConnectionPort");
    _fwd_SteamAPI_servernetadr_t_SetIP = dlsym(real_lib, "SteamAPI_servernetadr_t_SetIP");
    _fwd_SteamAPI_servernetadr_t_SetQueryPort = dlsym(real_lib, "SteamAPI_servernetadr_t_SetQueryPort");
    _fwd_SteamAPI_SetBreakpadAppID = dlsym(real_lib, "SteamAPI_SetBreakpadAppID");
    _fwd_SteamAPI_SetMiniDumpComment = dlsym(real_lib, "SteamAPI_SetMiniDumpComment");
    _fwd_SteamAPI_SetTryCatchCallbacks = dlsym(real_lib, "SteamAPI_SetTryCatchCallbacks");
    _fwd_SteamAPI_Shutdown = dlsym(real_lib, "SteamAPI_Shutdown");
    _fwd_SteamAPI_SteamApps_v008 = dlsym(real_lib, "SteamAPI_SteamApps_v008");
    _fwd_SteamAPI_SteamController_v008 = dlsym(real_lib, "SteamAPI_SteamController_v008");
    _fwd_SteamAPI_SteamDatagramHostedAddress_Clear = dlsym(real_lib, "SteamAPI_SteamDatagramHostedAddress_Clear");
    _fwd_SteamAPI_SteamDatagramHostedAddress_GetPopID = dlsym(real_lib, "SteamAPI_SteamDatagramHostedAddress_GetPopID");
    _fwd_SteamAPI_SteamDatagramHostedAddress_SetDevAddress = dlsym(real_lib, "SteamAPI_SteamDatagramHostedAddress_SetDevAddress");
    _fwd_SteamAPI_SteamFriends_v017 = dlsym(real_lib, "SteamAPI_SteamFriends_v017");
    _fwd_SteamAPI_SteamGameSearch_v001 = dlsym(real_lib, "SteamAPI_SteamGameSearch_v001");
    _fwd_SteamAPI_SteamGameServerHTTP_v003 = dlsym(real_lib, "SteamAPI_SteamGameServerHTTP_v003");
    _fwd_SteamAPI_SteamGameServerInventory_v003 = dlsym(real_lib, "SteamAPI_SteamGameServerInventory_v003");
    _fwd_SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002 = dlsym(real_lib, "SteamAPI_SteamGameServerNetworkingMessages_SteamAPI_v002");
    _fwd_SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012 = dlsym(real_lib, "SteamAPI_SteamGameServerNetworkingSockets_SteamAPI_v012");
    _fwd_SteamAPI_SteamGameServerNetworking_v006 = dlsym(real_lib, "SteamAPI_SteamGameServerNetworking_v006");
    _fwd_SteamAPI_SteamGameServerStats_v001 = dlsym(real_lib, "SteamAPI_SteamGameServerStats_v001");
    _fwd_SteamAPI_SteamGameServerUGC_v020 = dlsym(real_lib, "SteamAPI_SteamGameServerUGC_v020");
    _fwd_SteamAPI_SteamGameServerUtils_v010 = dlsym(real_lib, "SteamAPI_SteamGameServerUtils_v010");
    _fwd_SteamAPI_SteamGameServer_v015 = dlsym(real_lib, "SteamAPI_SteamGameServer_v015");
    _fwd_SteamAPI_SteamHTMLSurface_v005 = dlsym(real_lib, "SteamAPI_SteamHTMLSurface_v005");
    _fwd_SteamAPI_SteamHTTP_v003 = dlsym(real_lib, "SteamAPI_SteamHTTP_v003");
    _fwd_SteamAPI_SteamInput_v006 = dlsym(real_lib, "SteamAPI_SteamInput_v006");
    _fwd_SteamAPI_SteamInventory_v003 = dlsym(real_lib, "SteamAPI_SteamInventory_v003");
    _fwd_SteamAPI_SteamIPAddress_t_IsSet = dlsym(real_lib, "SteamAPI_SteamIPAddress_t_IsSet");
    _fwd_SteamAPI_SteamMatchmakingServers_v002 = dlsym(real_lib, "SteamAPI_SteamMatchmakingServers_v002");
    _fwd_SteamAPI_SteamMatchmaking_v009 = dlsym(real_lib, "SteamAPI_SteamMatchmaking_v009");
    _fwd_SteamAPI_SteamMusicRemote_v001 = dlsym(real_lib, "SteamAPI_SteamMusicRemote_v001");
    _fwd_SteamAPI_SteamMusic_v001 = dlsym(real_lib, "SteamAPI_SteamMusic_v001");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetFloat = dlsym(real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetFloat");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt32 = dlsym(real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetInt32");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetInt64 = dlsym(real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetInt64");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetPtr = dlsym(real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetPtr");
    _fwd_SteamAPI_SteamNetworkingConfigValue_t_SetString = dlsym(real_lib, "SteamAPI_SteamNetworkingConfigValue_t_SetString");
    _fwd_SteamAPI_SteamNetworkingIdentity_Clear = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_Clear");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetFakeIPType = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetFakeIPType");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericBytes = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetGenericBytes");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetGenericString = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetGenericString");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetIPAddr = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetIPAddr");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetIPv4 = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetIPv4");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetPSNID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetPSNID");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetSteamID");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetSteamID64 = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    _fwd_SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_GetXboxPairwiseID");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsEqualTo = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_IsEqualTo");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsFakeIP = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_IsFakeIP");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsInvalid = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_IsInvalid");
    _fwd_SteamAPI_SteamNetworkingIdentity_IsLocalHost = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_IsLocalHost");
    _fwd_SteamAPI_SteamNetworkingIdentity_ParseString = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_ParseString");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericBytes = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetGenericBytes");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetGenericString = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetGenericString");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetIPAddr = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetIPAddr");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetIPv4Addr = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetIPv4Addr");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetLocalHost = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetLocalHost");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetPSNID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetPSNID");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetSteamID");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetSteamID64 = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    _fwd_SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_SetXboxPairwiseID");
    _fwd_SteamAPI_SteamNetworkingIdentity_ToString = dlsym(real_lib, "SteamAPI_SteamNetworkingIdentity_ToString");
    _fwd_SteamAPI_SteamNetworkingIPAddr_Clear = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_Clear");
    _fwd_SteamAPI_SteamNetworkingIPAddr_GetFakeIPType = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_GetFakeIPType");
    _fwd_SteamAPI_SteamNetworkingIPAddr_GetIPv4 = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_GetIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsEqualTo = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_IsEqualTo");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsFakeIP = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_IsFakeIP");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv4 = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_IsIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_IsIPv6AllZeros");
    _fwd_SteamAPI_SteamNetworkingIPAddr_IsLocalHost = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_IsLocalHost");
    _fwd_SteamAPI_SteamNetworkingIPAddr_ParseString = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_ParseString");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv4 = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv4");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6 = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv6");
    _fwd_SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_SetIPv6LocalHost");
    _fwd_SteamAPI_SteamNetworkingIPAddr_ToString = dlsym(real_lib, "SteamAPI_SteamNetworkingIPAddr_ToString");
    _fwd_SteamAPI_SteamNetworkingMessages_SteamAPI_v002 = dlsym(real_lib, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    _fwd_SteamAPI_SteamNetworkingMessage_t_Release = dlsym(real_lib, "SteamAPI_SteamNetworkingMessage_t_Release");
    _fwd_SteamAPI_SteamNetworkingSockets_SteamAPI_v012 = dlsym(real_lib, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    _fwd_SteamAPI_SteamNetworkingUtils_SteamAPI_v004 = dlsym(real_lib, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    _fwd_SteamAPI_SteamNetworking_v006 = dlsym(real_lib, "SteamAPI_SteamNetworking_v006");
    _fwd_SteamAPI_SteamParentalSettings_v001 = dlsym(real_lib, "SteamAPI_SteamParentalSettings_v001");
    _fwd_SteamAPI_SteamParties_v002 = dlsym(real_lib, "SteamAPI_SteamParties_v002");
    _fwd_SteamAPI_SteamRemotePlay_v002 = dlsym(real_lib, "SteamAPI_SteamRemotePlay_v002");
    _fwd_SteamAPI_SteamRemoteStorage_v016 = dlsym(real_lib, "SteamAPI_SteamRemoteStorage_v016");
    _fwd_SteamAPI_SteamScreenshots_v003 = dlsym(real_lib, "SteamAPI_SteamScreenshots_v003");
    _fwd_SteamAPI_SteamTimeline_v004 = dlsym(real_lib, "SteamAPI_SteamTimeline_v004");
    _fwd_SteamAPI_SteamUGC_v020 = dlsym(real_lib, "SteamAPI_SteamUGC_v020");
    _fwd_SteamAPI_SteamUserStats_v013 = dlsym(real_lib, "SteamAPI_SteamUserStats_v013");
    _fwd_SteamAPI_SteamUser_v023 = dlsym(real_lib, "SteamAPI_SteamUser_v023");
    _fwd_SteamAPI_SteamUtils_v010 = dlsym(real_lib, "SteamAPI_SteamUtils_v010");
    _fwd_SteamAPI_SteamVideo_v007 = dlsym(real_lib, "SteamAPI_SteamVideo_v007");
    _fwd_SteamAPI_UnregisterCallback = dlsym(real_lib, "SteamAPI_UnregisterCallback");
    _fwd_SteamAPI_UnregisterCallResult = dlsym(real_lib, "SteamAPI_UnregisterCallResult");
    _fwd_SteamAPI_UseBreakpadCrashHandler = dlsym(real_lib, "SteamAPI_UseBreakpadCrashHandler");
    _fwd_SteamAPI_WriteMiniDump = dlsym(real_lib, "SteamAPI_WriteMiniDump");
    _fwd_SteamClient = dlsym(real_lib, "SteamClient");
    _fwd_SteamGameServer_BSecure = dlsym(real_lib, "SteamGameServer_BSecure");
    _fwd_SteamGameServer_GetHSteamPipe = dlsym(real_lib, "SteamGameServer_GetHSteamPipe");
    _fwd_SteamGameServer_GetHSteamUser = dlsym(real_lib, "SteamGameServer_GetHSteamUser");
    _fwd_SteamGameServer_GetIPCCallCount = dlsym(real_lib, "SteamGameServer_GetIPCCallCount");
    _fwd_SteamGameServer_GetSteamID = dlsym(real_lib, "SteamGameServer_GetSteamID");
    _fwd_SteamGameServer_InitSafe = dlsym(real_lib, "SteamGameServer_InitSafe");
    _fwd_SteamGameServer_RunCallbacks = dlsym(real_lib, "SteamGameServer_RunCallbacks");
    _fwd_SteamGameServer_Shutdown = dlsym(real_lib, "SteamGameServer_Shutdown");
    _fwd_SteamInternal_ContextInit = dlsym(real_lib, "SteamInternal_ContextInit");
    _fwd_SteamInternal_CreateInterface = dlsym(real_lib, "SteamInternal_CreateInterface");
    _fwd_SteamInternal_FindOrCreateGameServerInterface = dlsym(real_lib, "SteamInternal_FindOrCreateGameServerInterface");
    _fwd_SteamInternal_FindOrCreateUserInterface = dlsym(real_lib, "SteamInternal_FindOrCreateUserInterface");
    _fwd_SteamInternal_GameServer_Init_V2 = dlsym(real_lib, "SteamInternal_GameServer_Init_V2");
    _fwd_SteamInternal_SteamAPI_Init = dlsym(real_lib, "SteamInternal_SteamAPI_Init");
    _fwd_SteamRealPath = dlsym(real_lib, "SteamRealPath");
    _fwd___wrap_access = dlsym(real_lib, "__wrap_access");
    _fwd___wrap_chdir = dlsym(real_lib, "__wrap_chdir");
    _fwd___wrap_chmod = dlsym(real_lib, "__wrap_chmod");
    _fwd___wrap_chown = dlsym(real_lib, "__wrap_chown");
    _fwd___wrap_dlmopen = dlsym(real_lib, "__wrap_dlmopen");
    _fwd___wrap_dlopen = dlsym(real_lib, "__wrap_dlopen");
    _fwd___wrap_fopen = dlsym(real_lib, "__wrap_fopen");
    _fwd___wrap_fopen64 = dlsym(real_lib, "__wrap_fopen64");
    _fwd___wrap_freopen = dlsym(real_lib, "__wrap_freopen");
    _fwd___wrap_lchown = dlsym(real_lib, "__wrap_lchown");
    _fwd___wrap_link = dlsym(real_lib, "__wrap_link");
    _fwd___wrap_lstat = dlsym(real_lib, "__wrap_lstat");
    _fwd___wrap_lstat64 = dlsym(real_lib, "__wrap_lstat64");
    _fwd___wrap___lxstat = dlsym(real_lib, "__wrap___lxstat");
    _fwd___wrap___lxstat64 = dlsym(real_lib, "__wrap___lxstat64");
    _fwd___wrap_mkdir = dlsym(real_lib, "__wrap_mkdir");
    _fwd___wrap_mkfifo = dlsym(real_lib, "__wrap_mkfifo");
    _fwd___wrap_mknod = dlsym(real_lib, "__wrap_mknod");
    _fwd___wrap_mount = dlsym(real_lib, "__wrap_mount");
    _fwd___wrap_open = dlsym(real_lib, "__wrap_open");
    _fwd___wrap_open64 = dlsym(real_lib, "__wrap_open64");
    _fwd___wrap_opendir = dlsym(real_lib, "__wrap_opendir");
    _fwd___wrap_rename = dlsym(real_lib, "__wrap_rename");
    _fwd___wrap_rmdir = dlsym(real_lib, "__wrap_rmdir");
    _fwd___wrap_scandir = dlsym(real_lib, "__wrap_scandir");
    _fwd___wrap_scandir64 = dlsym(real_lib, "__wrap_scandir64");
    _fwd___wrap_stat = dlsym(real_lib, "__wrap_stat");
    _fwd___wrap_stat64 = dlsym(real_lib, "__wrap_stat64");
    _fwd___wrap_statfs = dlsym(real_lib, "__wrap_statfs");
    _fwd___wrap_statfs64 = dlsym(real_lib, "__wrap_statfs64");
    _fwd___wrap_statvfs = dlsym(real_lib, "__wrap_statvfs");
    _fwd___wrap_statvfs64 = dlsym(real_lib, "__wrap_statvfs64");
    _fwd___wrap_symlink = dlsym(real_lib, "__wrap_symlink");
    _fwd___wrap_unlink = dlsym(real_lib, "__wrap_unlink");
    _fwd___wrap_utime = dlsym(real_lib, "__wrap_utime");
    _fwd___wrap_utimes = dlsym(real_lib, "__wrap_utimes");
    _fwd___wrap___xstat = dlsym(real_lib, "__wrap___xstat");
    _fwd___wrap___xstat64 = dlsym(real_lib, "__wrap___xstat64");
}
