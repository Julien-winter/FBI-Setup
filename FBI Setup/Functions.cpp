#include "Functions.h"

namespace Helper {
    bool restartRequired = false;
    std::atomic<bool> vcComplete{false};
    int vcCheckSleepTimes = 0;
    bool titleLoopBool = true;
    std::mutex consoleMutex;
    std::mutex logMutex;
    std::ofstream logFile;
    bool logEnabled = false;
    CLIConfig cliConfig;
    std::vector<CheckResult> g_results;
    std::mutex g_resultsMutex;
    std::string g_repoUrl = "Julien-winter/FBI-Setup";
    std::string g_appName = "FBI-Setup";
    std::string g_appVersion = "1.1.1";
}

void Helper::recordResult(const std::string& check, const std::string& status, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.push_back({check, status, message});
}

std::string Helper::escapeJSON(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break; case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break; default: out += c;
        }
    }
    return out;
}

std::string Helper::getTimestampISO() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm; gmtime_s(&tm, &time);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void Helper::exportResultsJSON() {
    if (cliConfig.exportPath.empty()) return;
    std::ofstream f(cliConfig.exportPath);
    if (!f.is_open()) { printError("- Failed to export results to " + cliConfig.exportPath); return; }
    int passed = 0, failed = 0, warnings = 0, skipped = 0;
    for (auto& r : g_results) {
        if (r.status == "OK") passed++; else if (r.status == "FAIL") failed++;
        else if (r.status == "WARN") warnings++; else if (r.status == "SKIPPED") skipped++;
    }
    f << "{\n";
    f << "  \"timestamp\": \"" << escapeJSON(getTimestampISO()) << "\",\n";
    f << "  \"program\": \"" << escapeJSON(g_appName) << "\",\n";
    f << "  \"version\": \"" << escapeJSON(g_appVersion) << "\",\n";
    f << "  \"results\": [\n";
    for (size_t i = 0; i < g_results.size(); i++) {
        f << "    {\"check\": \"" << escapeJSON(g_results[i].check) << "\", "
          << "\"status\": \"" << escapeJSON(g_results[i].status) << "\", "
          << "\"message\": \"" << escapeJSON(g_results[i].message) << "\"}";
        if (i < g_results.size() - 1) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"summary\": {\n";
    f << "    \"passed\": " << passed << ",\n";
    f << "    \"failed\": " << failed << ",\n";
    f << "    \"warnings\": " << warnings << ",\n";
    f << "    \"skipped\": " << skipped << ",\n";
    f << "    \"restart_required\": " << (restartRequired ? "true" : "false") << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();
    printSuccess("- Results exported to " + cliConfig.exportPath, false);
}

std::string Helper::fetchURL(const std::wstring& url) {
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"FBI-Setup/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    URL_COMPONENTS urlComp; ZeroMemory(&urlComp, sizeof(urlComp)); urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {0}, path[1024] = {0};
    urlComp.lpszHostName = host; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path; urlComp.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp)) { WinHttpCloseHandle(hSession); return ""; }
    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }
    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);
    DWORD bytesRead = 0; char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead)) { if (bytesRead == 0) break; buffer[bytesRead] = '\0'; result += buffer; }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return result;
}

void Helper::showHelp() {
    std::cout << g_appName << " - System Pre-Flight Check Tool v" << g_appVersion << "\n\n";
    std::cout << "Usage: " << g_appName << ".exe [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "  --headless          Run without user interaction\n";
    std::cout << "  --quiet             Only show errors and warnings\n";
    std::cout << "  --log               Write output to %TEMP%\\fbi-setup.log\n";
    std::cout << "  --export FILE       Export results as JSON to FILE\n";
    std::cout << "  --skip N[,M,...]    Skip specific checks by number (1-22)\n";
    std::cout << "  --only N[,M,...]    Run only specific checks by number (1-22)\n\n";
    std::cout << "Check Numbers:\n";
    std::cout << "  1=WindowsDefender 2=3rdPartyAV 3=SecureBoot 4=CPU-V 5=RiotVanguard\n";
    std::cout << "  6=VCRedist 7=Chrome 8=ChromeProtection 9=TimeSync 10=Winver\n";
    std::cout << "  11=Symbols 12=FastBoot 13=ExploitProtection 14=SmartScreen\n";
    std::cout << "  15=GameBar 16=TPM 17=CoreIsolation 18=DMAProtection 19=ModifiedOS\n";
    std::cout << "  20=Internet 21=VM-Detect 22=Auto-Update\n";
}

CLIConfig Helper::parseCLI(int argc, char* argv[]) {
    CLIConfig config;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--help") { config.showHelp = true; return config; }
        else if (arg == "--headless") { config.headless = true; }
        else if (arg == "--quiet") { config.quiet = true; }
        else if (arg == "--log") { config.logToFile = true; }
        else if (arg == "--skip" && i + 1 < argc) {
            std::string val(argv[++i]); std::stringstream ss(val); std::string token;
            while (std::getline(ss, token, ',')) { try { config.skipChecks.push_back(std::stoi(token)); } catch (...) {} }
        }
        else if (arg == "--only" && i + 1 < argc) {
            std::string val(argv[++i]); std::stringstream ss(val); std::string token;
            while (std::getline(ss, token, ',')) { try { config.onlyChecks.push_back(std::stoi(token)); } catch (...) {} }
        }
        else if (arg == "--config" && i + 1 < argc) { config.configPath = argv[++i]; }
        else if (arg == "--export" && i + 1 < argc) { config.exportPath = argv[++i]; }
    }
    return config;
}

bool Helper::isCheckSkipped(int checkId) {
    if (!cliConfig.onlyChecks.empty()) return std::find(cliConfig.onlyChecks.begin(), cliConfig.onlyChecks.end(), checkId) == cliConfig.onlyChecks.end();
    if (!cliConfig.skipChecks.empty()) return std::find(cliConfig.skipChecks.begin(), cliConfig.skipChecks.end(), checkId) != cliConfig.skipChecks.end();
    return false;
}

bool Helper::isAdmin() {
    BOOL isElevated = FALSE; HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation; DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size)) isElevated = elevation.TokenIsElevated;
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

void Helper::initLogging() {
    if (!cliConfig.logToFile && !cliConfig.headless) return;
    wchar_t tempPath[MAX_PATH]; GetTempPathW(MAX_PATH, tempPath);
    std::wstring logPath = std::wstring(tempPath) + L"fbi-setup.log";
    logFile.open(logPath, std::ios::out | std::ios::app);
    if (logFile.is_open()) { logEnabled = true; logWrite("=== FBI-Setup started at " + getTimestampISO() + " ==="); }
}

void Helper::closeLogging() {
    if (logEnabled && logFile.is_open()) { logWrite("=== FBI-Setup finished ==="); logFile.close(); logEnabled = false; }
}

void Helper::logWrite(const std::string& message) {
    if (!logEnabled || !logFile.is_open()) return;
    std::lock_guard<std::mutex> lock(logMutex); logFile << message << std::endl; logFile.flush();
}

std::string Helper::runSystemCommandWithOutput(const char* command, DWORD timeoutMs) {
    auto future = std::async(std::launch::async, [command]() -> std::string {
        std::FILE* pipe = _popen(command, "r"); if (!pipe) return "";
        char buffer[256]; std::string result;
        while (std::fgets(buffer, sizeof(buffer), pipe) != NULL) result += buffer;
        _pclose(pipe); return result;
    });
    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) return "TIMEOUT";
    return future.get();
}

void Checks::checkInternet() {
    SetConsoleTitleA("Checking Internet Connection");
    auto testPing = []() -> bool {
        std::string out = Helper::runSystemCommandWithOutput("ping -n 1 -w 3000 8.8.8.8", 5000);
        return (out.find("TTL=") != std::string::npos || out.find("ttl=") != std::string::npos);
    };
    if (testPing()) { Helper::recordResult("Internet", "OK", "Connected"); return; }
    Helper::printConcern("- No internet detected, attempting fixes...");
    Helper::runSystemCommand("ipconfig /flushdns");
    Helper::runSystemCommand("netsh winsock reset");
    Sleep(1000);
    if (testPing()) { Helper::printSuccess("- Internet restored after DNS/Winsock reset", true); Helper::recordResult("Internet", "OK", "Fixed via DNS/Winsock reset"); return; }
    Helper::runSystemCommand("ipconfig /release"); Sleep(500);
    Helper::runSystemCommand("ipconfig /renew"); Sleep(2000);
    if (testPing()) { Helper::printSuccess("- Internet restored after IP renew", true); Helper::recordResult("Internet", "OK", "Fixed via IP renew"); return; }
    Helper::printError("- No internet connection - could not fix automatically");
    Helper::recordResult("Internet", "FAIL", "No connectivity - fix attempts failed");
}

void Checks::checkVM() {
    SetConsoleTitleA("Checking for Virtual Machine");
    bool isVM = false; std::string vmType;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VBoxGuest", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { isVM = true; vmType = "VirtualBox"; RegCloseKey(hKey); }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\VMware, Inc.\\VMware Tools", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { isVM = true; vmType = vmType.empty() ? "VMware" : vmType + "/VMware"; RegCloseKey(hKey); }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\ACPI\\DSDT\\VBOX__", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { isVM = true; vmType = vmType.empty() ? "VirtualBox (ACPI)" : vmType + "/VBox-ACPI"; RegCloseKey(hKey); }
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) { if (OpenService(scm, "VBoxService", SERVICE_QUERY_STATUS)) { isVM = true; vmType = vmType.empty() ? "VirtualBox" : vmType; } CloseServiceHandle(scm); }
    if (isVM) { Helper::printError("- Running inside a Virtual Machine (" + vmType + ")"); Helper::recordResult("VM Detection", "FAIL", "Detected: " + vmType); }
    else { Helper::recordResult("VM Detection", "OK", "Bare metal"); }
}

void Checks::checkForUpdate() {
    SetConsoleTitleA("Checking for Updates");
    std::string json = Helper::fetchURL(L"https://api.github.com/repos/" + std::wstring(Helper::g_repoUrl.begin(), Helper::g_repoUrl.end()) + L"/releases/latest");
    if (json.empty()) { Helper::printConcern("- Could not check for updates"); Helper::recordResult("Auto-Update", "WARN", "No response"); return; }
    std::string tag; auto tagPos = json.find("\"tag_name\":\"");
    if (tagPos != std::string::npos) { tagPos += 12; auto endPos = json.find("\"", tagPos); if (endPos != std::string::npos) tag = json.substr(tagPos, endPos - tagPos); }
    if (tag.empty()) { Helper::printConcern("- Could not parse update info"); Helper::recordResult("Auto-Update", "WARN", "Parse failed"); return; }
    if (tag != "v" + Helper::g_appVersion && tag != Helper::g_appVersion) {
        Helper::printConcern("- New version available: " + tag + " (current: v" + Helper::g_appVersion + ")");
        Helper::recordResult("Auto-Update", "WARN", "New version: " + tag);
    } else { Helper::printSuccess("- You are on the latest version (v" + Helper::g_appVersion + ")", false); Helper::recordResult("Auto-Update", "OK", "Up to date"); }
}

void Checks::checkWindowsDefender()
{
    SetConsoleTitleA("Checking Windows Defender");

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to check Windows Defender (Error 1, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Windows Defender", "FAIL", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "WinDefend", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            Helper::recordResult("Windows Defender", "OK", "Service removed");
        }
        else {
            Helper::printError("- Failed to check Windows Defender (Error 2, GLE=" + std::to_string(GetLastError()) + ")");
            Helper::recordResult("Windows Defender", "FAIL", "OpenService failed");
        }
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printError("- Failed to check Windows Defender (Error 3, GLE=" + std::to_string(GetLastError()) + ")");
        Helper::recordResult("Windows Defender", "FAIL", "QueryService failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (status.dwCurrentState != SERVICE_RUNNING && status.dwCurrentState != SERVICE_START_PENDING) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        Helper::printSuccess("- Windows Defender is disabled", false);
        Helper::recordResult("Windows Defender", "OK", "Disabled");
        return;
    }

    // Try to stop + disable
    CloseServiceHandle(service);
    service = OpenService(scm, "WinDefend", SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);

    bool serviceStopped = false;
    if (service != NULL) {
        SERVICE_STATUS stopStatus;
        if (ControlService(service, SERVICE_CONTROL_STOP, &stopStatus)) {
            Sleep(500);
            serviceStopped = true;
        }
        if (ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            serviceStopped = true;
        }
        CloseServiceHandle(service);
    }

    // Always set registry policies
    HKEY hKey; DWORD val = 1;
    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "DisableAntiSpyware", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "DisableRealtimeMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableBehaviorMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableOnAccessProtection", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegSetValueEx(hKey, "DisableScanOnRealtimeEnable", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    if (serviceStopped) {
        Helper::printSuccess("- Windows Defender disabled (service + policy)", true);
        Helper::recordResult("Windows Defender", "OK", "Disabled (changed)");
    }
    else {
        Helper::printSuccess("- Windows Defender policies set (restart required)", true);
        Helper::recordResult("Windows Defender", "OK", "Policy set, restart needed");
        Sleep(500);
        Helper::runSystemCommand("start https://www.sordum.org/9480/defender-control-v2-1/");
    }
    Helper::restartRequired = true;
}

void Checks::check3rdPartyAntiVirus()
{
    SetConsoleTitleA("Checking for 3rd Party Anti-Viruses");

    std::string command = "WMIC /Node:localhost /Namespace:\\\\root\\SecurityCenter2 Path AntiVirusProduct Get displayName /Format:List";
    std::string antivirusList;
    std::FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to check for 3rd party Anti-Viruses (Error 4, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("3rd Party AV", "FAIL", "WMIC failed");
        return;
    }

    char buffer[128]; std::string wmicOutput;
    while (std::fgets(buffer, 128, pipe) != NULL) wmicOutput += buffer;
    _pclose(pipe);

    std::vector<std::string> detectedAVs;
    std::size_t pos;
    while ((pos = wmicOutput.find("\n")) != std::string::npos) {
        std::string av = wmicOutput.substr(0, pos);
        if (av.find("Windows Defender") == std::string::npos && av.size() > 12) {
            av = av.substr(12);
            av.erase(std::remove(av.begin(), av.end(), '\n'), av.end());
            av.erase(std::remove(av.begin(), av.end(), '\r'), av.end());
            av.erase(std::remove(av.begin(), av.end(), '\b'), av.end());
            detectedAVs.push_back(av);
        }
        if (pos + 1 < wmicOutput.size()) wmicOutput = wmicOutput.substr(pos + 1);
        else break;
    }

    if (detectedAVs.empty()) {
        Helper::printSuccess("- No 3rd party Anti-Virus was detected", false);
        Helper::recordResult("3rd Party AV", "OK", "None detected");
        return;
    }

    std::string avList;
    for (auto& av : detectedAVs) { if (!avList.empty()) avList += ", "; avList += av; }

    bool anyRemoved = false;
    for (auto& av : detectedAVs) {
        // Try to find uninstall string in registry
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD idx = 0; char subkeyName[256]; DWORD subkeyNameSize = sizeof(subkeyName);
            while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY hSubkey;
                if (RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey) == ERROR_SUCCESS) {
                    char dn[256]; DWORD dnSize = sizeof(dn);
                    if (RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize) == ERROR_SUCCESS) {
                        std::string displayName(dn);
                        if (displayName.find(av) != std::string::npos || av.find(displayName) != std::string::npos) {
                            char uninstall[512]; DWORD usSize = sizeof(uninstall);
                            if (RegQueryValueEx(hSubkey, "UninstallString", NULL, NULL, (LPBYTE)uninstall, &usSize) == ERROR_SUCCESS) {
                                std::string uninstallCmd(uninstall);
                                uninstallCmd += " /S /quiet /norestart";
                                Helper::runSystemCommand(uninstallCmd.c_str());
                                Sleep(1000);
                                anyRemoved = true;
                            }
                        }
                    }
                    RegCloseKey(hSubkey);
                }
                subkeyNameSize = sizeof(subkeyName); ++idx;
            }
            RegCloseKey(hKey);
        }
    }

    if (anyRemoved) {
        Helper::printSuccess("- Attempted to remove 3rd party AV (" + avList + ")", true);
        Helper::recordResult("3rd Party AV", "OK", "Uninstall attempted: " + avList);
    }
    else {
        Helper::printError("- 3rd party Anti-Virus detected, please uninstall manually (" + avList + ")");
        Helper::recordResult("3rd Party AV", "FAIL", "Found: " + avList);
    }
}

void Checks::checkCPUV() {
    SetConsoleTitleA("Checking CPUV-V");
    std::string result = Helper::runSystemCommandWithOutput("WMIC CPU Get VirtualizationFirmwareEnabled", 10000);
    if (result == "TIMEOUT") { Helper::printError("- Timed out checking CPU-V (Error 5)"); Helper::recordResult("CPU-V", "FAIL", "Timeout"); return; }
    if (result.empty()) { Helper::printError("- Failed to check CPU-V (Error 5)"); Helper::recordResult("CPU-V", "FAIL", "WMIC failed"); return; }
    if (result.find("True") != std::string::npos) { Helper::printError("- CPU-V is enabled in BIOS"); Helper::recordResult("CPU-V", "FAIL", "Enabled"); }
    else { Helper::printSuccess("- CPU-V is disabled", false); Helper::recordResult("CPU-V", "OK", "Disabled"); }
}

void Checks::uninstallRiotVanguard() {
    SetConsoleTitleA("Checking for Riot Vanguard");
    HKEY hKey; LONG res = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ, &hKey);
    if (res != ERROR_SUCCESS) { Helper::printError("- Failed to check Riot Vanguard (Error 6)"); Helper::recordResult("Riot Vanguard", "FAIL", "RegOpen failed"); return; }
    DWORD idx = 0; char subkeyName[256]; DWORD subkeyNameSize = sizeof(subkeyName);
    while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hSubkey; res = RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey);
        if (res != ERROR_SUCCESS) { RegCloseKey(hKey); Helper::recordResult("Riot Vanguard", "FAIL", "RegEnum failed"); return; }
        char dn[256]; DWORD dnSize = sizeof(dn);
        res = RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize);
        if (res == ERROR_SUCCESS && strcmp(dn, "Riot Vanguard") == 0) {
            RegCloseKey(hSubkey); RegCloseKey(hKey);
            bool vgProgFiles = std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe");
            bool vgProgFilesX86 = std::filesystem::exists("C:\\Program Files (x86)\\Riot Vanguard\\installer.exe");
            if (vgProgFiles || vgProgFilesX86) {
                std::string vgPath = vgProgFiles ? "C:\\Program Files\\Riot Vanguard\\installer.exe" : "C:\\Program Files (x86)\\Riot Vanguard\\installer.exe";
                if (!Helper::cliConfig.headless) { MessageBoxA(NULL, "When prompted to uninstall, press YES", "Uninstall Vanguard", MB_ICONINFORMATION | MB_TOPMOST); }
                _spawnl(_P_WAIT, vgPath.c_str(), vgPath.c_str(), NULL);
                Helper::printError("- Riot Vanguard needs to be uninstalled"); Helper::recordResult("Riot Vanguard", "FAIL", "Installed - prompted uninstall"); return;
            }
            Helper::printError("- Failed to uninstall Riot Vanguard (Error 8)"); Helper::recordResult("Riot Vanguard", "FAIL", "Manual uninstall needed"); return;
        }
        RegCloseKey(hSubkey); subkeyNameSize = sizeof(subkeyName); ++idx;
    }
    RegCloseKey(hKey);
    Helper::printSuccess("- Riot Vanguard is not installed", false); Helper::recordResult("Riot Vanguard", "OK", "Not installed");
}

void Checks::installVCRedist() {
    SetConsoleTitleA("Downloading VCRedist"); Helper::vcComplete = false;
    bool alreadyInstalled = (std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") && std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll")) || (std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll") && std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"));
    if (alreadyInstalled) { Helper::printSuccess("- VCRedist is already installed", false); Helper::recordResult("VC Redist", "OK", "Already installed"); Helper::vcComplete = true; return; }
    HRESULT dx64 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x64.exe", "C:\\Windows\\VC_redist.x64.exe", 0, NULL);
    HRESULT dx86 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x86.exe", "C:\\Windows\\VC_redist.x86.exe", 0, NULL);
    if (dx64 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x64.exe")) { Helper::printError("- Failed to download VCRedist x64 (Error 9)"); Helper::recordResult("VC Redist", "FAIL", "Download x64 failed"); Sleep(1000); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe"); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe"); Helper::vcComplete = true; return; }
    if (dx86 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x86.exe")) { Helper::printError("- Failed to download VCRedist x86 (Error 10)"); Helper::recordResult("VC Redist", "FAIL", "Download x86 failed"); Sleep(1000); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe"); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe"); Helper::vcComplete = true; return; }
    SetConsoleTitleA("Installing VCRedist");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x64.exe /install /q /norestart");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x86.exe /install /q /norestart");
    if ((!std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") || !std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll")) && (!std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll") || !std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"))) { Helper::printError("- VCRedist didn't install correctly (Error 11)"); Helper::recordResult("VC Redist", "FAIL", "Verify failed"); Sleep(1000); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe"); Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe"); }
    else { Helper::printSuccess("- VCRedist is installed", false); Helper::recordResult("VC Redist", "OK", "Installed successfully"); }
    Helper::vcComplete = true;
}

void Checks::checkSecureBoot() {
    SetConsoleTitleA("Checking Secure-Boot"); DWORD sb;
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", &sb);
    if (sb == 0) { Helper::printSuccess("- Secure-Boot is disabled", false); Helper::recordResult("Secure Boot", "OK", "Disabled"); }
    else if (sb == 1) { Helper::printError("- Secure-Boot is enabled"); Helper::recordResult("Secure Boot", "FAIL", "Enabled"); }
    else { Helper::printError("- Unable to check Secure-Boot (Error 12)"); Helper::recordResult("Secure Boot", "FAIL", "Unknown state"); }
}

void Checks::isChromeInstalled() {
    SetConsoleTitleA("Checking for Google Chrome");
    std::vector<std::wstring> paths = { L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe" };
    wchar_t lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH) > 0) paths.push_back(std::wstring(lad) + L"\\Google\\Chrome\\Application\\chrome.exe");
    for (auto& p : paths) if (std::filesystem::exists(p)) { Helper::printSuccess("- Google Chrome is installed", false); Helper::recordResult("Google Chrome", "OK", "Installed"); return; }
    Helper::printError("- Google Chrome is not installed"); Helper::recordResult("Google Chrome", "FAIL", "Not installed"); Sleep(1000); Helper::runSystemCommand("start https://www.google.com/chrome/");
}

void Checks::syncWindowsTime() {
    SetConsoleTitleA("Syncing Windows Time");
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) { Helper::printError("- Failed to sync Windows Time (Error 14)"); Helper::recordResult("Time Sync", "FAIL", "SCM open failed"); return; }
    Helper::runSystemCommand("w32tm /register");
    if (Helper::getServiceStatus("W32Time") == STATUS_SERVICE_STOPPED) {
        SC_HANDLE svc = OpenService(scm, "W32Time", SERVICE_ALL_ACCESS);
        if (svc == NULL) { Helper::printError("- Failed to sync Windows Time (Error 15)"); Helper::recordResult("Time Sync", "FAIL", "OpenService failed"); CloseServiceHandle(scm); return; }
        DWORD needed = 0; QueryServiceConfig(svc, NULL, 0, &needed);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) { Helper::printError("- Failed to sync Windows Time (Error 16)"); CloseServiceHandle(svc); CloseServiceHandle(scm); return; }
        std::vector<BYTE> buf(needed); LPQUERY_SERVICE_CONFIG cfg = (LPQUERY_SERVICE_CONFIG)buf.data();
        if (!QueryServiceConfig(svc, cfg, needed, &needed)) { Helper::printError("- Failed to sync Windows Time (Error 16)"); CloseServiceHandle(svc); CloseServiceHandle(scm); return; }
        if (cfg->dwStartType == SERVICE_DISABLED) { Helper::printError("- Failed to sync Windows Time (Error 17)"); CloseServiceHandle(svc); CloseServiceHandle(scm); return; }
        if (!ChangeServiceConfig(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) { Helper::printError("- Failed to sync Windows Time (Error 18)"); CloseServiceHandle(svc); CloseServiceHandle(scm); return; }
        if (StartService(svc, 0, NULL) == FALSE) { Helper::printError("- Failed to sync Windows Time (Error 19)"); CloseServiceHandle(svc); CloseServiceHandle(scm); return; }
        CloseServiceHandle(svc); Sleep(250);
    }
    CloseServiceHandle(scm);
    Helper::runSystemCommand("net stop w32time");
    Helper::runSystemCommand("w32tm /unregister");
    Helper::runSystemCommand("w32tm /register");
    Helper::runSystemCommand("net start w32time");
    Helper::runSystemCommand("w32tm /resync");
    Helper::printSuccess("- Successfully synced Windows time", false); Helper::recordResult("Time Sync", "OK", "Synced");
}

void Checks::disableChromeProtection() {
    SetConsoleTitleA("Disabling Google Chrome Protection");
    std::vector<std::wstring> chromePaths = { L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe" };
    wchar_t lad[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH) > 0) chromePaths.push_back(std::wstring(lad) + L"\\Google\\Chrome\\Application\\chrome.exe");
    bool chromeFound = false;
    for (auto& p : chromePaths) { if (std::filesystem::exists(p)) { chromeFound = true; break; } }
    if (!chromeFound) { Helper::recordResult("Chrome Protection", "SKIPPED", "Chrome not installed"); return; }
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome", "SafeBrowsingProtectionLevel", &status)) {
        if (status == 1) { Helper::printSuccess("- Chrome protection is disabled", false); Helper::recordResult("Chrome Protection", "OK", "Already disabled"); return; }
    }
    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "SafeBrowsingProtectionLevel", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) { Helper::printSuccess("- Disabled Chrome protection", true); Helper::recordResult("Chrome Protection", "OK", "Disabled (changed)"); }
    else { Helper::printError("- Failed to disable Chrome protection (Error 20/21)"); Helper::recordResult("Chrome Protection", "FAIL", "Registry write failed"); }
}

void Checks::checkWinver() {
    SetConsoleTitleA("Checking Winver"); HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS) { Helper::printError("- Failed to check Winver (Error 22)"); Helper::recordResult("Winver", "FAIL", "Read failed"); return; }
    char bs[32]; DWORD bsSize = sizeof(bs);
    if (RegQueryValueEx(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)bs, &bsSize) != ERROR_SUCCESS) { Helper::printError("- Failed to check Winver (Error 22)"); Helper::recordResult("Winver", "FAIL", "Read failed"); RegCloseKey(hKey); return; }
    RegCloseKey(hKey);
    int build = std::stoi(bs);
    char pn[128] = ""; DWORD pnSize = sizeof(pn);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)pn, &pnSize); RegCloseKey(hKey); }
    std::string winver(pn);
    std::map<int, std::string> bm = {{10240,"Win10 1507"},{10586,"Win10 1511"},{14393,"Win10 1607"},{15063,"Win10 1703"},{16299,"Win10 1709"},{17134,"Win10 1803"},{17763,"Win10 1809"},{18362,"Win10 1903"},{19041,"Win10 2004"},{19042,"Win10 20H2"},{19043,"Win10 21H1"},{19044,"Win10 21H2"},{19045,"Win10 22H2"},{22000,"Win11 21H2"},{22621,"Win11 22H2"},{22631,"Win11 23H2"},{26100,"Win11 24H2"}};
    int minb = 19041;
    std::vector<int> tr = {19045, 22621};
    auto it = bm.find(build); if (it != bm.end()) winver = it->second;
    if (build < minb) { Helper::printError("- Winver \"" + winver + "\" unsupported"); Helper::recordResult("Winver", "FAIL", "Unsupported: " + winver); }
    else if (std::find(tr.begin(), tr.end(), build) != tr.end()) { Helper::printConcern("- Winver \"" + winver + "\" is 50/50"); Helper::recordResult("Winver", "WARN", "Troublesome: " + winver); }
    else { Helper::printSuccess("- Winver is supported (" + winver + ")", false); Helper::recordResult("Winver", "OK", winver); }
}

void Checks::deleteSymbols() {
    SetConsoleTitleA("Deleting C:\\Symbols"); std::string path = "C:\\Symbols";
    if (std::filesystem::exists(path)) {
        std::error_code ec;
        if (!std::filesystem::remove_all(path, ec)) { Helper::printError("- Unable to delete " + path + " (Error 24, " + ec.message() + ")"); Helper::recordResult("Symbols", "FAIL", "Delete failed"); return; }
        Helper::printSuccess("- Successfully deleted " + path, true); Helper::recordResult("Symbols", "OK", "Deleted (changed)");
    } else { Helper::recordResult("Symbols", "OK", "Not present"); }
}

void Checks::checkFastBoot() {
    SetConsoleTitleA("Checking Fast-Boot"); DWORD s;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", "HiberbootEnabled", &s)) { if (s == 0) { Helper::printSuccess("- Fast-Boot is disabled", false); Helper::recordResult("Fast Boot", "OK", "Already disabled"); return; } }
    HKEY hKey; DWORD disp; DWORD v = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "HiberbootEnabled", NULL, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) { Helper::printSuccess("- Disabled Fast-Boot", true); Helper::restartRequired = true; Helper::recordResult("Fast Boot", "OK", "Disabled (changed)"); }
    else { Helper::printError("- Failed to disable Fast-Boot (Error 31/32)"); Helper::recordResult("Fast Boot", "FAIL", "Registry failed"); }
}

void Checks::checkExploitProtection() {
    SetConsoleTitleA("Checking Exploit-Protection"); DWORD s;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection", "DisallowExploitProtectionOverride", &s)) { if (s == 1) { Helper::printSuccess("- Exploit-Protection is disabled", false); Helper::recordResult("Exploit Protection", "OK", "Already disabled"); return; } }
    HKEY hKey; DWORD disp; DWORD v = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "DisallowExploitProtectionOverride", NULL, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) { Helper::printSuccess("- Disabled Exploit-Protection", true); Helper::restartRequired = true; Helper::recordResult("Exploit Protection", "OK", "Disabled (changed)"); }
    else { Helper::printError("- Failed to disable Exploit-Protection (Error 25/26)"); Helper::recordResult("Exploit Protection", "FAIL", "Registry failed"); }
}

void Checks::checkSmartScreen() {
    SetConsoleTitleA("Checking SmartScreen"); DWORD s;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableSmartScreen", &s)) { if (s == 0) { Helper::printSuccess("- SmartScreen is disabled", false); Helper::recordResult("SmartScreen", "OK", "Already disabled"); return; } }
    HKEY hKey; DWORD disp; DWORD v = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "EnableSmartScreen", NULL, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) { Helper::printSuccess("- Disabled SmartScreen", true); Helper::restartRequired = true; Helper::recordResult("SmartScreen", "OK", "Disabled (changed)"); }
    else { Helper::printError("- Failed to disable SmartScreen (Error 27/28)"); Helper::recordResult("SmartScreen", "FAIL", "Registry failed"); }
}

void Checks::checkGameBar() {
    SetConsoleTitleA("Checking Xbox Gamebar"); DWORD s;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", "AppCaptureEnabled", &s)) { if (s == 1) { Helper::printSuccess("- Gamebar is enabled", false); Helper::recordResult("Game Bar", "OK", "Already enabled"); return; } }
    HKEY hKey; DWORD disp; DWORD v = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "AppCaptureEnabled", NULL, REG_DWORD, (const BYTE*)&v, sizeof(v)); RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) { Helper::printSuccess("- Enabled Gamebar", true); Helper::restartRequired = true; Helper::recordResult("Game Bar", "OK", "Enabled (changed)"); }
    else { Helper::printError("- Failed to enable Gamebar (Error 29/30)"); Helper::recordResult("Game Bar", "FAIL", "Registry failed"); }
}

void Checks::checkModifiedOS() {
    SetConsoleTitleA("Checking if OS is modified"); bool mod = false; std::string reasons;
    auto cs = [&](const char* n, const char* l) { SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT); if (scm) { SC_HANDLE svc = OpenService(scm, n, SERVICE_QUERY_STATUS); if (svc == NULL) { mod = true; reasons += std::string(" ") + l + " missing;"; } else CloseServiceHandle(svc); CloseServiceHandle(scm); } };
    cs("WinDefend", "WinDefend"); cs("wscsvc", "SecurityCenter"); cs("wuauserv", "WindowsUpdate"); cs("BFE", "BFE"); cs("mpssvc", "Firewall");
    HKEY hKey;
    auto cr = [&](const char* k, const char* l) { if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &hKey) == ERROR_SUCCESS) { mod = true; reasons += std::string(" ") + l + ";"; RegCloseKey(hKey); } };
    cr("SOFTWARE\\AtlasOS", "AtlasOS"); cr("SOFTWARE\\ReviOS", "ReviOS"); cr("SOFTWARE\\GhostSpectre", "GhostSpectre"); cr("SOFTWARE\\GGOS", "GGOS");
    cr("SOFTWARE\\PhoenixOS", "PhoenixOS"); cr("SOFTWARE\\KernelOS", "KernelOS"); cr("SOFTWARE\\XOS", "XOS"); cr("SOFTWARE\\OptimusOS", "OptimusOS");
    char pn[256] = ""; DWORD sz = sizeof(pn);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)pn, &sz); RegCloseKey(hKey); }
    std::string ps(pn);
    std::vector<std::string> mk = {"Atlas","Revi","Ghost","Spectre","Tiny","Lite","X-Lite","Micro","XOS","Optimus","SuperLite","Compact","Minimal","Gaming"};
    for (auto& m : mk) if (ps.find(m) != std::string::npos) { mod = true; reasons += " ProductName(" + m + ");"; break; }
    char ed[256] = ""; DWORD edSz = sizeof(ed);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { if (RegQueryValueEx(hKey, "EditionID", NULL, NULL, (LPBYTE)ed, &edSz) == ERROR_SUCCESS) { std::string es(ed); if (es.find("Custom") != std::string::npos || es.find("Gaming") != std::string::npos) { mod = true; reasons += " Edition(" + es + ");"; } } RegCloseKey(hKey); }
    if (mod) { Helper::printConcern("- OS appears MODIFIED:" + reasons); Helper::recordResult("Modified OS", "WARN", "Modified:" + reasons); }
    else { Helper::printSuccess("- OS appears unmodified", false); Helper::recordResult("Modified OS", "OK", "Clean"); }
}

void Checks::checkTPMStatus() {
    SetConsoleTitleA("Checking TPM"); DWORD a = 0; bool f = false; HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\TPM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) { DWORD sz = sizeof(DWORD); if (RegQueryValueEx(hKey, "TPMActive", NULL, NULL, (LPBYTE)&a, &sz) == ERROR_SUCCESS) f = true; RegCloseKey(hKey); }
    if (!f || a == 0) { Helper::printSuccess("- TPM is disabled / not found", false); Helper::recordResult("TPM", "OK", "Disabled/not found"); }
    else { Helper::printError("- TPM is enabled"); Helper::recordResult("TPM", "FAIL", "Enabled"); }
}

void Checks::checkCoreIsolation() {
    SetConsoleTitleA("Checking Core-Isolation (HVCI)"); DWORD hv = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", &hv)) { if (hv == 0) { Helper::printSuccess("- HVCI is disabled", false); Helper::recordResult("Core Isolation", "OK", "Disabled"); return; } }
    DWORD vb = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", &vb)) { if (vb == 1) { Helper::printError("- VBS/Core-Isolation is enabled"); Helper::recordResult("Core Isolation", "FAIL", "VBS enabled"); return; } }
    if (hv == 1) { Helper::printError("- HVCI is enabled"); Helper::recordResult("Core Isolation", "FAIL", "HVCI enabled"); }
    else { Helper::printSuccess("- HVCI is disabled", false); Helper::recordResult("Core Isolation", "OK", "Disabled"); }
}

void Checks::checkDMAProtection() {
    SetConsoleTitleA("Checking Kernel DMA Protection"); DWORD d = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\KernelDmaProtection", "Enabled", &d)) { if (d == 0) { Helper::printSuccess("- Kernel DMA Protection is disabled", false); Helper::recordResult("DMA Protection", "OK", "Disabled"); return; } else { Helper::printError("- Kernel DMA Protection is enabled"); Helper::recordResult("DMA Protection", "FAIL", "Enabled"); return; } }
    DWORD ds = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "KernelDmaProtectionState", &ds)) { if (ds == 0) { Helper::printSuccess("- Kernel DMA Protection is disabled", false); Helper::recordResult("DMA Protection", "OK", "Disabled"); } else { Helper::printError("- Kernel DMA Protection state=" + std::to_string(ds)); Helper::recordResult("DMA Protection", "FAIL", "State=" + std::to_string(ds)); } return; }
    Helper::printSuccess("- Kernel DMA Protection not active", false); Helper::recordResult("DMA Protection", "OK", "Not supported");
}

void Helper::setupConsole() { HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE); DWORD mode = 0; GetConsoleMode(hStdin, &mode); SetConsoleMode(hStdin, mode & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS)); SetConsoleTitleA("Initializing"); }

void Helper::printSuccess(const std::string& message, bool changed) {
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Green); std::cout << "[+] "; Color::setForegroundColor(Color::White); std::cout << message;
    if (changed) { Color::setForegroundColor(Color::Yellow); std::cout << " (CHANGED)"; }
    std::cout << std::endl;
    logWrite("[+] " + message + (changed ? " (CHANGED)" : ""));
}

void Helper::printConcern(const std::string& message) {
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Yellow); std::cout << "[-] "; Color::setForegroundColor(Color::White); std::cout << message << std::endl;
    logWrite("[-] " + message);
}

void Helper::printError(const std::string& message) {
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Red); std::cout << "[X] "; Color::setForegroundColor(Color::White); std::cout << message << std::endl;
    logWrite("[X] " + message);
}

void Helper::runSystemCommand(const char* command) { std::string mc = command; mc += " 2>nul"; FILE* s = _popen(mc.c_str(), "r"); if (s) { char b[1024]; while (fgets(b, sizeof(b), s)) {} _pclose(s); } }

void Helper::titleLoop() {
    int ld = 150, sd = 40;
    std::string msgs[] = {"FBI Setup","FBI Setup.","FBI Setup..","FBI Setup...","FBI Setup..","FBI Setup.","FBI Setup","FBI Setup.","FBI Setup..","FBI Setup...","FBI Setup..","FBI Setup.","FBI Setup","FBI Setup.","FBI Setup..","FBI Setup...","FBI Setup..","FBI Setup.","FBI Setup","FBI Setup.","FBI Setup..","FBI Setup...","FBI Setup..","FBI Setup.","FBI Setup","FBI Setu","FBI Set","FBI Se","FBI S","FBI ","FBI","FB","F","","M","Ma","Mad","Made","Made ","Made B","Made By","Made By ","Made By F","Made By FB","Made By FBI","Made By FBI ","Made By FBI S","Made By FBI Se","Made By FBI Set","Made By FBI Setu","Made By FBI Setup","Made By FBI Setup.","Made By FBI Setup..","Made By FBI Setup...","Made By FBI Setup..","Made By FBI Setup.","Made By FBI Setup","Made By FBI Setup.","Made By FBI Setup..","Made By FBI Setup...","Made By FBI Setup..","Made By FBI Setup.","Made By FBI Setup","Made By FBI Setup.","Made By FBI Setup..","Made By FBI Setup...","Made By FBI Setup..","Made By FBI Setup.","Made By FBI Setup","Made By FBI Setup.","Made By FBI Setup..","Made By FBI Setup...","Made By FBI Setup..","Made By FBI Setup.","Made By FBI Setup","Made By FBI Setu","Made By FBI Set","Made By FBI Se","Made By FBI S","Made By FBI ","Made By FBI","Made By FB","Made By F","Made By ","Made By","Made B","Made ","Made","Mad","Ma","M","","F","FB","FBI","FBI ","FBI S","FBI Se","FBI Set","FBI Setu","FBI Setup"};
    int dlys[] = {ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,ld,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd,sd};
    int idx = 0;
    while (titleLoopBool) { for (auto& m : msgs) { idx++; SetConsoleTitleA((std::string(35,' ')+"|"+std::string(35-m.length()+10,' ')+m).c_str()); std::this_thread::sleep_for(std::chrono::milliseconds(dlys[idx])); if (!titleLoopBool) goto end_loop; } idx = 0; }
end_loop: {}
}

bool Helper::readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData) {
    HKEY hKey; LONG ret = RegOpenKeyEx(hKeyParent, subkey, 0, KEY_READ, &hKey);
    if (ret == ERROR_SUCCESS) { DWORD data; DWORD len = sizeof(DWORD); ret = RegQueryValueEx(hKey, valueName, NULL, NULL, reinterpret_cast<LPBYTE>(&data), &len); RegCloseKey(hKey); if (ret == ERROR_SUCCESS) { (*readData) = data; return true; } return false; }
    return false;
}

ServiceStatus Helper::getServiceStatus(LPCSTR serviceName) {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS); if (!scm) return STATUS_SERVICE_STOPPED;
    SC_HANDLE svc = OpenService(scm, serviceName, SERVICE_QUERY_STATUS); if (!svc) { CloseServiceHandle(scm); return STATUS_SERVICE_STOPPED; }
    SERVICE_STATUS_PROCESS ssp; DWORD needed;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &needed)) { CloseServiceHandle(svc); CloseServiceHandle(scm); return STATUS_SERVICE_STOPPED; }
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return (ServiceStatus)ssp.dwCurrentState;
}

void Color::setBackgroundColor(const RGBColor& color) { HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode); dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode); printf(("\x1b[48;2;"+std::to_string(color.r)+";"+std::to_string(color.g)+";"+std::to_string(color.b)+"m").c_str()); }
void Color::setForegroundColor(const RGBColor& color) { HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode); dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode); printf(("\x1b[38;2;"+std::to_string(color.r)+";"+std::to_string(color.g)+";"+std::to_string(color.b)+"m").c_str()); }
