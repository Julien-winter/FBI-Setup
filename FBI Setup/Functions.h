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

namespace Checks {
    void checkWindowsDefender();
    void check3rdPartyAntiVirus();
    void checkCPUV();
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
}
namespace Helper {
    inline bool restartRequired;
    inline bool vcComplete;
    inline int vcCheckSleepTimes;
    inline bool titleLoopBool = true;

    void setupConsole();
    void printSuccess(const std::string& message, bool changed);
    void printConcern(const std::string& message);
    void printError(const std::string& message);
    void runSystemCommand(const char* command);
    void titleLoop();
    bool readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData);
    ServiceStatus getServiceStatus(LPCSTR serviceName);
}
namespace Color {
    void setBackgroundColor(const RGBColor& aColor);
    void setForegroundColor(const RGBColor& aColor);

    inline RGBColor Red = { 255, 0, 0 };
    inline RGBColor Green = { 0, 255, 0 };
    inline RGBColor Yellow = { 255, 255, 0 };
    inline RGBColor Cyan = { 0, 255, 255 };
    inline RGBColor LightGray = { 211, 211, 211 };
    inline RGBColor White = { 255, 255, 255 };
}
