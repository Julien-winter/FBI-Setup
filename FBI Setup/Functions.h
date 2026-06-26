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
    STATUS_SERVICE_STOPPED,
    STATUS_SERVICE_START_PENDING,
    STATUS_SERVICE_STOP_PENDING,
    STATUS_SERVICE_RUNNING,
    STATUS_SERVICE_CONTINUE_PENDING,
    STATUS_SERVICE_PAUSE_PENDING,
    STATUS_SERVICE_PAUSED
};

enum class ErrorCode {
    E01_Defender_SCM = 1, E02_Defender_Service = 2, E03_Defender_Query = 3,
    E04_AV_WMIC = 4, E05_CPUV_WMIC = 5, E06_Vanguard_RegOpen = 6, E07_Vanguard_RegEnum = 7,
    E08_Vanguard_Uninstall = 8, E09_VCRedist_Download_x64 = 9, E10_VCRedist_Download_x86 = 10,
    E11_VCRedist_Verify = 11, E12_SecureBoot_Read = 12, E13_Chrome_Check = 13,
    E14_TimeSync_SCM = 14, E15_TimeSync_Service = 15, E16_TimeSync_Config = 16,
    E17_TimeSync_Disabled = 17, E18_TimeSync_ChangeConfig = 18, E19_TimeSync_Start = 19,
    E20_Chrome_RegWrite = 20, E21_Chrome_RegCreate = 21, E22_Winver_Read = 22,
    E23_Winver_Unknown = 23, E24_Symbols_Delete = 24, E25_Exploit_Write = 25,
    E26_Exploit_Open = 26, E27_SmartScreen_Write = 27, E28_SmartScreen_Open = 28,
    E29_Gamebar_Write = 29, E30_Gamebar_Open = 30, E31_FastBoot_Write = 31, E32_FastBoot_Open = 32,
};

struct CLIConfig {
    bool headless = false;
    bool logToFile = false;
    bool quiet = false;
    std::vector<int> skipChecks;
    std::vector<int> onlyChecks;
    std::string configPath;
    std::string exportPath;
    bool showHelp = false;
};

struct CheckResult {
    std::string check;
    std::string status;
    std::string message;
};

namespace Checks {
    void checkWindowsDefender(); void check3rdPartyAntiVirus(); void checkCPUV();
    void uninstallRiotVanguard(); void installVCRedist(); void checkSecureBoot();
    void isChromeInstalled(); void syncWindowsTime(); void disableChromeProtection();
    void checkWinver(); void deleteSymbols(); void checkFastBoot(); void checkExploitProtection();
    void checkSmartScreen(); void checkGameBar(); void checkModifiedOS(); void checkTPMStatus();
    void checkCoreIsolation(); void checkDMAProtection();
    void checkInternet(); void checkVM(); void checkForUpdate();
}
namespace Helper {
    extern bool restartRequired;
    extern std::atomic<bool> vcComplete;
    extern int vcCheckSleepTimes;
    extern bool titleLoopBool;
    extern std::mutex consoleMutex;
    extern std::mutex logMutex;
    extern std::ofstream logFile;
    extern bool logEnabled;
    extern CLIConfig cliConfig;
    extern std::vector<CheckResult> g_results;
    extern std::mutex g_resultsMutex;
    extern std::string g_repoUrl;
    extern std::string g_appName;
    extern std::string g_appVersion;

    void setupConsole();
    void printSuccess(const std::string& message, bool changed);
    void printConcern(const std::string& message);
    void printError(const std::string& message);
    void runSystemCommand(const char* command);
    std::string runSystemCommandWithOutput(const char* command, DWORD timeoutMs);
    void titleLoop();
    bool readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData);
    ServiceStatus getServiceStatus(LPCSTR serviceName);
    bool isAdmin();
    void initLogging(); void closeLogging(); void logWrite(const std::string& message);
    CLIConfig parseCLI(int argc, char* argv[]); void showHelp();
    bool isCheckSkipped(int checkId);
    void recordResult(const std::string& check, const std::string& status, const std::string& message);
    void exportResultsJSON();
    std::string escapeJSON(const std::string& s);
    std::string getTimestampISO();
    std::string fetchURL(const std::wstring& url);
}
namespace Color {
    void setBackgroundColor(const RGBColor& aColor);
    void setForegroundColor(const RGBColor& aColor);
    inline RGBColor Red = { 255, 0, 0 }; inline RGBColor Green = { 0, 255, 0 };
    inline RGBColor Yellow = { 255, 255, 0 }; inline RGBColor Cyan = { 0, 255, 255 };
    inline RGBColor LightGray = { 211, 211, 211 }; inline RGBColor White = { 255, 255, 255 };
}
