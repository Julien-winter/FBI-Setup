#pragma once
#include "Includes.h"

struct RGBColor
{
    int r;
    int g;
    int b;
};
enum ServiceStatus
{
    // v1.2.2 bugfix: explicit winnt.h values - getServiceStatus() casts the raw
    // SERVICE_* state (STOPPED=1 ... PAUSED=7) directly into this enum, so the
    // implicit 0-based values made EVERY comparison off by one
    // (a running service never equalled STATUS_SERVICE_RUNNING).
    STATUS_SERVICE_STOPPED = 1,
    STATUS_SERVICE_START_PENDING = 2,
    STATUS_SERVICE_STOP_PENDING = 3,
    STATUS_SERVICE_RUNNING = 4,
    STATUS_SERVICE_CONTINUE_PENDING = 5,
    STATUS_SERVICE_PAUSE_PENDING = 6,
    STATUS_SERVICE_PAUSED = 7
};

enum class ErrorCode {
    E01_Defender_SCM = 1,
    E02_Defender_Service = 2,
    E03_Defender_Query = 3,
    E04_AV_WMIC = 4,
    E05_CPUV_WMIC = 5,
    E06_Vanguard_RegOpen = 6,
    E07_Vanguard_RegEnum = 7,
    E08_Vanguard_Uninstall = 8,
    E09_VCRedist_Download_x64 = 9,
    E10_VCRedist_Download_x86 = 10,
    E11_VCRedist_Verify = 11,
    E12_SecureBoot_Read = 12,
    E13_Chrome_Check = 13,
    E14_TimeSync_SCM = 14,
    E15_TimeSync_Service = 15,
    E16_TimeSync_Config = 16,
    E17_TimeSync_Disabled = 17,
    E18_TimeSync_ChangeConfig = 18,
    E19_TimeSync_Start = 19,
    E20_Chrome_RegWrite = 20,
    E21_Chrome_RegCreate = 21,
    E22_Winver_Read = 22,
    E23_Winver_Unknown = 23,
    E24_Symbols_Delete = 24,
    E25_Exploit_Write = 25,
    E26_Exploit_Open = 26,
    E27_SmartScreen_Write = 27,
    E28_SmartScreen_Open = 28,
    E29_Gamebar_Write = 29,
    E30_Gamebar_Open = 30,
    E31_FastBoot_Write = 31,
    E32_FastBoot_Open = 32,
    E33_Defender_Tamper = 33,
    E34_AV_Tamper = 34,
};

struct CLIConfig {
    bool headless = false;
    bool quiet = false;
    std::vector<int> skipChecks;
    std::vector<int> onlyChecks;
    std::string exportPath;
    bool showHelp = false;
    bool autoApply = false;
};

struct CheckResult {
    std::string check;
    std::string status; // "OK", "FAIL", "WARN", "SKIPPED"
    std::string message;
};

namespace Checks {
    bool checkDefenderTamperProtection();
    bool checkAVTamperProtection(const std::string& avName);
    void checkWindowsDefender();
    void check3rdPartyAntiVirus();
    void uninstallRiotVanguard();
    void installVCRedist();
    void checkSecureBoot();
    void isChromeInstalled();
    void syncWindowsTime();
    void disableChromeProtection();

    void checkWinver();
    void deleteSymbols();
    void checkFastBoot();
    void checkExploitProtection();
    void checkSmartScreen();
    void checkGameBar();
    void checkModifiedOS();
    void checkTPMStatus();
    void checkCoreIsolation();
    void checkDMAProtection();

    void checkInternet();
    void checkNetworkAdapters();
    void checkVM();
    void checkForUpdate();
    void checkSecurityMitigations();
    void checkUAC();
    void checkWindowsUpdate();

    void checkCPUVirtualization();
    void checkSpoofer();
    void checkActivation();
}
namespace Helper {
    extern bool restartRequired;
    extern std::atomic<bool> vcComplete;
    extern int vcCheckSleepTimes;

    extern std::mutex consoleMutex;
    extern std::mutex logMutex;
    extern std::ofstream logFile;
    extern bool logEnabled;
    extern std::string g_logPath;
    extern CLIConfig cliConfig;

    extern std::vector<CheckResult> g_results;
    extern std::mutex g_resultsMutex;
    extern std::vector<std::string> g_pendingChanges;
    extern std::mutex g_pendingChangesMutex;
    extern bool applyChanges;
    extern std::string g_repoUrl;
    extern std::string g_appName;
    extern std::string g_appVersion;
    extern int g_exitCode;

    void setupConsole();
    void printSuccess(const std::string& message, bool changed);
    void printConcern(const std::string& message);
    void printError(const std::string& message);
    void printInfo(const std::string& message);
    // v1.2.4: adapter line with an orange warning suffix (kick risk on
    // flagged VPN/virtual adapters), also keeps the warning in --quiet
    void printAdapterLine(const std::string& message, bool used, const std::string& orangeSuffix);
    void runSystemCommand(const char* command);
    int runSystemCommandExitCode(const char* command);
    std::string runSystemCommandWithOutput(const char* command, DWORD timeoutMs);
    bool readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData);
    ServiceStatus getServiceStatus(LPCSTR serviceName);
    void backupRegistryKey(HKEY hKeyParent, const std::string& subkey);
    // Appends one applied change to %LOCALAPPDATA%\FBI-Setup\applied-changes.log;
    // Undo-FBI-Setup.ps1 reads it to revert exactly what this run changed
    void logAppliedChange(const std::string& change);

    bool isAdmin();
    void initLogging();
    void closeLogging();
    void logWrite(const std::string& message);
    CLIConfig parseCLI(int argc, char* argv[]);
    void showHelp();
    bool isCheckSkipped(int checkId);
    bool requestChange(const std::string& change);
    void clearPendingChanges();
    bool hasPendingChanges();
    void printPendingChanges();
    bool promptApplyChanges();
    void clearResults();

    void recordResult(const std::string& check, const std::string& status, const std::string& message);
    void exportResultsJSON();
    std::string escapeJSON(const std::string& s);
    std::string getTimestampISO();
    std::string fetchURL(const std::wstring& url);
}
namespace Color {
    void setBackgroundColor(const RGBColor& aColor);
    void setForegroundColor(const RGBColor& aColor);

    inline RGBColor Red = { 255, 0, 0 };
    inline RGBColor Green = { 0, 255, 0 };
    inline RGBColor Yellow = { 255, 255, 0 };
    inline RGBColor Cyan = { 0, 255, 255 };
    inline RGBColor LightGray = { 211, 211, 211 };
    inline RGBColor DarkGray = { 128, 128, 128 };
    inline RGBColor White = { 255, 255, 255 };
    // v1.2.4: kick-risk warnings on flagged network adapters
    inline RGBColor Orange = { 255, 165, 0 };
}
