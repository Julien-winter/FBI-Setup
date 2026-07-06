#include "Functions.h"

namespace Helper {
    bool restartRequired = false;
    std::atomic<bool> vcComplete{false};
    int vcCheckSleepTimes = 0;
    std::mutex consoleMutex;
    std::mutex logMutex;
    std::ofstream logFile;
    bool logEnabled = false;
    std::string g_logPath;
    CLIConfig cliConfig;
    std::vector<CheckResult> g_results;
    std::mutex g_resultsMutex;
    std::vector<std::string> g_pendingChanges;
    std::mutex g_pendingChangesMutex;
    bool applyChanges = false;
    std::string g_repoUrl = "Julien-winter/FBI-Setup";
    std::string g_appName = "FBI-Setup";
    std::string g_appVersion = "1.2.0";
    int g_exitCode = 0;
}

void Helper::recordResult(const std::string& check, const std::string& status, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.push_back({check, status, message});
    logWrite("  [" + status + "] " + check + " - " + message);
}

void Helper::clearResults()
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.clear();
}

bool Helper::requestChange(const std::string& change)
{
    if (applyChanges) return true;

    {
        std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
        if (std::find(g_pendingChanges.begin(), g_pendingChanges.end(), change) == g_pendingChanges.end()) {
            g_pendingChanges.push_back(change);
        }
    }

    printConcern("- Change needed: " + change);
    return false;
}

void Helper::clearPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    g_pendingChanges.clear();
}

bool Helper::hasPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    return !g_pendingChanges.empty();
}

void Helper::printPendingChanges()
{
    std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
    if (g_pendingChanges.empty()) return;

    Color::setForegroundColor(Color::Cyan);
    std::cout << "\nPending changes:\n";
    Color::setForegroundColor(Color::LightGray);
    for (const auto& change : g_pendingChanges) {
        std::cout << " - " << change << "\n";
    }
}

bool Helper::promptApplyChanges()
{
    if (!hasPendingChanges()) return false;
    printPendingChanges();

    if (cliConfig.headless) {
        printConcern("- Headless mode: pending changes were not applied");
        return false;
    }

    int choice = MessageBoxA(NULL,
        "The checker found changes that may be needed.\n\nApply these changes now?",
        "Apply setup changes?",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (choice == IDYES) return true;
    if (choice == IDNO) return false;

    std::string answer;
    std::cout << "\nApply pending changes? Type yes or no: ";
    std::getline(std::cin, answer);
    std::transform(answer.begin(), answer.end(), answer.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return answer == "yes" || answer == "y";
}

std::string Helper::escapeJSON(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c;
        }
    }
    return out;
}

std::string Helper::getTimestampISO()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_s(&tm, &time);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void Helper::exportResultsJSON()
{
    if (cliConfig.exportPath.empty()) return;

    std::ofstream f(cliConfig.exportPath);
    if (!f.is_open()) {
        printError("- Failed to export results to " + cliConfig.exportPath);
        return;
    }

    int passed = 0, failed = 0, warnings = 0, skipped = 0;
    for (auto& r : g_results) {
        if (r.status == "OK") passed++;
        else if (r.status == "FAIL") failed++;
        else if (r.status == "WARN") warnings++;
        else if (r.status == "SKIPPED") skipped++;
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

std::string Helper::fetchURL(const std::wstring& url)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"CL-Setup/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {0}, path[1024] = {0};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead)) {
        if (bytesRead == 0) break;
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

void Helper::showHelp()
{
    std::cout << g_appName << " - System Pre-Flight Check Tool v" << g_appVersion << "\n\n";
    std::cout << "Usage: " << g_appName << ".exe [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --help              Show this help message\n";
    std::cout << "  --headless          Run without user interaction\n";
    std::cout << "  --quiet             Only show errors and warnings\n";
    std::cout << "  --apply             Apply needed changes without prompting\n";
    std::cout << "  --export FILE       Export results as JSON to FILE\n";
    std::cout << "  --skip N[,M,...]    Skip specific checks by number (1-26)\n";
    std::cout << "  --only N[,M,...]    Run only specific checks by number (1-26)\n\n";
    std::cout << "Check Numbers:\n";
    std::cout << "  1=WindowsDefender 2=3rdPartyAV 3=SecureBoot 4=CPU-V 5=RiotVanguard\n";
    std::cout << "  6=VCRedist 7=Chrome 8=ChromeProtection 9=TimeSync 10=Winver\n";
    std::cout << "  11=Symbols 12=FastBoot 13=ExploitProtection 14=SmartScreen\n";
    std::cout << "  15=GameBar 16=TPM 17=CoreIsolation 18=DMAProtection 19=ModifiedOS\n";
    std::cout << "  20=Internet 21=NetworkAdapters 22=VM-Detect 23=Auto-Update\n";
    std::cout << "  24=SecurityMitigations 25=UAC 26=WindowsUpdate\n";
}

CLIConfig Helper::parseCLI(int argc, char* argv[])
{
    CLIConfig config;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--help") {
            config.showHelp = true;
            return config;
        }
        else if (arg == "--headless") {
            config.headless = true;
        }
        else if (arg == "--quiet") {
            config.quiet = true;
        }
        else if (arg == "--apply") {
            config.autoApply = true;
        }
        else if (arg == "--skip" && i + 1 < argc) {
            std::string val(argv[++i]);
            std::stringstream ss(val);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try { config.skipChecks.push_back(std::stoi(token)); }
                catch (...) {}
            }
        }
        else if (arg == "--only" && i + 1 < argc) {
            std::string val(argv[++i]);
            std::stringstream ss(val);
            std::string token;
            while (std::getline(ss, token, ',')) {
                try { config.onlyChecks.push_back(std::stoi(token)); }
                catch (...) {}
            }
        }
        else if (arg == "--export" && i + 1 < argc) {
            config.exportPath = argv[++i];
        }
    }

    return config;
}

bool Helper::isCheckSkipped(int checkId)
{
    if (!cliConfig.onlyChecks.empty()) {
        return std::find(cliConfig.onlyChecks.begin(), cliConfig.onlyChecks.end(), checkId) == cliConfig.onlyChecks.end();
    }
    if (!cliConfig.skipChecks.empty()) {
        return std::find(cliConfig.skipChecks.begin(), cliConfig.skipChecks.end(), checkId) != cliConfig.skipChecks.end();
    }
    return false;
}

bool Helper::isAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size)) {
            isElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

void Helper::initLogging()
{
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CL-Setup";
    CreateDirectoryW(dir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char filename[64];
    snprintf(filename, sizeof(filename), "setup-%04d-%02d-%02d-%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);

    std::wstring logPath = dir + L"\\" + std::wstring(filename, filename + strlen(filename));
    int len = WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, NULL, 0, NULL, NULL);
    g_logPath.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, &g_logPath[0], len, NULL, NULL);

    logFile.open(logPath, std::ios::out | std::ios::app);
    if (logFile.is_open()) {
        logEnabled = true;
        logWrite("=== " + g_appName + " v" + g_appVersion + " started at " + getTimestampISO() + " ===");
    }
}

void Helper::closeLogging()
{
    if (logEnabled && logFile.is_open()) {
        logWrite("=== " + g_appName + " finished ===");
        logFile.close();
        logEnabled = false;
    }
}

void Helper::logWrite(const std::string& message)
{
    if (!logEnabled || !logFile.is_open()) return;
    std::lock_guard<std::mutex> lock(logMutex);
    logFile << message << std::endl;
    logFile.flush();
}

std::string Helper::runSystemCommandWithOutput(const char* command, DWORD timeoutMs)
{
    std::string commandText = command ? command : "";
    auto future = std::async(std::launch::async, [commandText]() -> std::string {
        std::FILE* pipe = _popen(commandText.c_str(), "r");
        if (!pipe) return "";
        char buffer[256];
        std::string result;
        while (std::fgets(buffer, sizeof(buffer), pipe) != NULL) {
            result += buffer;
        }
        _pclose(pipe);
        return result;
    });

    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
        return "TIMEOUT";
    }
    return future.get();
}

void Helper::backupRegistryKey(HKEY hKeyParent, const std::string& subkey)
{
    HKEY hKey;
    if (RegOpenKeyEx(hKeyParent, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dir = std::wstring(tempPath) + L"CL-Setup\\Backup";
    CreateDirectoryW(dir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d-%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string keyName = subkey;
    std::replace(keyName.begin(), keyName.end(), '\\', '_');
    std::string exportPath = "C:\\Windows\\Temp\\" + keyName + "_" + timestamp + ".reg";
    std::string cmd = "reg export \"" + subkey + "\" \"" + exportPath + "\" /y 2>nul";
    runSystemCommand(cmd.c_str());
    RegCloseKey(hKey);
}

ServiceStatus Helper::getServiceStatus(LPCSTR serviceName)
{
    SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scmHandle == NULL) return STATUS_SERVICE_STOPPED;
    SC_HANDLE serviceHandle = OpenService(scmHandle, serviceName, SERVICE_QUERY_STATUS);
    if (serviceHandle == NULL) { CloseServiceHandle(scmHandle); return STATUS_SERVICE_STOPPED; }
    SERVICE_STATUS_PROCESS serviceStatus;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(serviceHandle, SC_STATUS_PROCESS_INFO, (LPBYTE)&serviceStatus, sizeof(serviceStatus), &bytesNeeded)) {
        CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle); return STATUS_SERVICE_STOPPED;
    }
    CloseServiceHandle(serviceHandle);
    CloseServiceHandle(scmHandle);
    return (ServiceStatus)serviceStatus.dwCurrentState;
}

void Helper::printInfo(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::LightGray);
    std::cout << "  " << message << std::endl;
}

void Checks::checkInternet()
{
    SetConsoleTitleA("Checking Internet Connection");

    auto testPing = []() -> bool {
        std::string out = Helper::runSystemCommandWithOutput("ping -n 1 -w 3000 8.8.8.8", 5000);
        return (out.find("TTL=") != std::string::npos || out.find("ttl=") != std::string::npos);
    };

    if (testPing()) {
        Helper::recordResult("Internet", "OK", "Connected");
        return;
    }

    if (!Helper::requestChange("Repair internet connection with DNS flush, Winsock reset, and IP renew")) {
        Helper::recordResult("Internet", "WARN", "No connectivity - repair pending approval");
        return;
    }

    Helper::printConcern("- No internet detected, attempting fixes...");
    Helper::runSystemCommand("ipconfig /flushdns");
    Helper::runSystemCommand("netsh winsock reset");
    Sleep(1000);

    if (testPing()) {
        Helper::printSuccess("- Internet restored after DNS/Winsock reset", true);
        Helper::recordResult("Internet", "OK", "Fixed via DNS/Winsock reset");
        return;
    }

    Helper::runSystemCommand("ipconfig /release");
    Sleep(500);
    Helper::runSystemCommand("ipconfig /renew");
    Sleep(2000);

    if (testPing()) {
        Helper::printSuccess("- Internet restored after IP renew", true);
        Helper::recordResult("Internet", "OK", "Fixed via IP renew");
        return;
    }

    Helper::printError("- No internet connection - could not fix automatically");
    Helper::recordResult("Internet", "FAIL", "No connectivity - fix attempts failed");
}

void Checks::checkNetworkAdapters()
{
    SetConsoleTitleA("Checking Network Adapters");

    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_UNSPEC;
    ULONG bufferSize = 15 * 1024;
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG result = GetAdaptersAddresses(family, flags, NULL, adapters, &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(family, flags, NULL, adapters, &bufferSize);
    }

    if (result != NO_ERROR) {
        Helper::printError("- Failed to list network adapters (GLE=" + std::to_string(result) + ")");
        Helper::recordResult("Network Adapters", "FAIL", "GetAdaptersAddresses failed: " + std::to_string(result));
        return;
    }

    auto wideToUtf8 = [](const wchar_t* value) -> std::string {
        if (value == NULL || value[0] == L'\0') return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
        if (size <= 1) return "";
        std::string text(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, NULL, NULL);
        return text;
    };

    auto statusToString = [](IF_OPER_STATUS status) -> std::string {
        switch (status) {
        case IfOperStatusUp: return "Up";
        case IfOperStatusDown: return "Down";
        case IfOperStatusTesting: return "Testing";
        case IfOperStatusUnknown: return "Unknown";
        case IfOperStatusDormant: return "Dormant";
        case IfOperStatusNotPresent: return "Not present";
        case IfOperStatusLowerLayerDown: return "Lower layer down";
        default: return "Other";
        }
    };

    int total = 0;
    int used = 0;
    std::string summary;

    for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
        total++;

        std::string name = wideToUtf8(adapter->FriendlyName);
        if (name.empty() && adapter->AdapterName != NULL) {
            name = adapter->AdapterName;
        }
        if (name.empty()) {
            name = "Unknown adapter";
        }

        bool hasAddress = adapter->FirstUnicastAddress != NULL;
        bool hasGateway = adapter->FirstGatewayAddress != NULL;
        bool isLoopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        bool isUsed = adapter->OperStatus == IfOperStatusUp && !isLoopback && (hasAddress || hasGateway);
        if (isUsed) used++;

        std::string line = name + " - " + (isUsed ? "USED" : "NOT USED") +
            " (" + statusToString(adapter->OperStatus) + ")";
        if (isLoopback) line += " [loopback]";

        if (isUsed) {
            Helper::printSuccess("- Adapter: " + line, false);
        }
        else {
            Helper::printInfo("Adapter: " + line);
        }

        if (!summary.empty()) summary += "; ";
        summary += line;
    }

    if (total == 0) {
        Helper::printConcern("- No network adapters found");
        Helper::recordResult("Network Adapters", "WARN", "No adapters found");
        return;
    }

    Helper::recordResult("Network Adapters", "OK",
        std::to_string(used) + "/" + std::to_string(total) + " used: " + summary);
}

void Checks::checkSecurityMitigations()
{
    SetConsoleTitleA("Checking Security Mitigations");

    bool changed = false;
    bool failed = false;
    bool warning = false;
    std::string summary;

    auto appendSummary = [&](const std::string& item) {
        if (!summary.empty()) summary += "; ";
        summary += item;
    };

    auto readDword = [](const char* subkey, const char* valueName, DWORD& value) -> bool {
        return Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, subkey, valueName, &value);
    };

    auto setDword = [&](const char* label, const char* subkey, const char* valueName, DWORD desiredValue) -> bool {
        DWORD currentValue = 0;
        bool hasValue = readDword(subkey, valueName, currentValue);
        if (hasValue && currentValue == desiredValue) {
            Helper::printSuccess("- " + std::string(label) + " already set to " + std::to_string(desiredValue), false);
            appendSummary(std::string(label) + "=" + std::to_string(desiredValue));
            return true;
        }

        if (!Helper::requestChange("Set " + std::string(label) + " to " + std::to_string(desiredValue))) {
            appendSummary(std::string(label) + "=" + (hasValue ? std::to_string(currentValue) : "missing") +
                "->" + std::to_string(desiredValue) + " pending");
            warning = true;
            return false;
        }

        HKEY hKey = NULL;
        LONG createResult = RegCreateKeyEx(HKEY_LOCAL_MACHINE, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
        if (createResult != ERROR_SUCCESS) {
            Helper::printError("- Failed to open " + std::string(label) + " registry key");
            appendSummary(std::string(label) + "=write failed");
            failed = true;
            return false;
        }

        LONG writeResult = RegSetValueEx(hKey, valueName, NULL, REG_DWORD,
            reinterpret_cast<const BYTE*>(&desiredValue), sizeof(desiredValue));
        RegCloseKey(hKey);

        if (writeResult != ERROR_SUCCESS) {
            Helper::printError("- Failed to set " + std::string(label));
            appendSummary(std::string(label) + "=write failed");
            failed = true;
            return false;
        }

        std::string previous = hasValue ? std::to_string(currentValue) : "missing";
        Helper::printSuccess("- Set " + std::string(label) + " from " + previous + " to " + std::to_string(desiredValue), true);
        appendSummary(std::string(label) + "=" + previous + "->" + std::to_string(desiredValue));
        changed = true;
        return true;
    };

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management");

    setDword("EnableVirtualizationBasedSecurity",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", 0);
    setDword("RequirePlatformSecurityFeatures",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "RequirePlatformSecurityFeatures", 0);
    setDword("DeviceGuard Locked",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "Locked", 1);
    setDword("HVCI Enabled",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", 0);
    setDword("HVCI Locked",
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Locked", 1);
    setDword("KVA FeatureSettingsOverride",
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", "FeatureSettingsOverride", 3);
    setDword("KVA FeatureSettingsOverrideMask",
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", "FeatureSettingsOverrideMask", 3);

    auto lower = [](std::string value) -> std::string {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };

    // Create restore point before making security changes
    if (Helper::applyChanges) {
        HINSTANCE hSrDll = LoadLibraryA("srclient.dll");
        if (hSrDll) {
            typedef BOOL(WINAPI* SRSetRestorePointFn)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
            SRSetRestorePointFn SRSetRestorePoint = (SRSetRestorePointFn)GetProcAddress(hSrDll, "SRSetRestorePointW");
            if (SRSetRestorePoint) {
                RESTOREPOINTINFOW rp;
                rp.dwEventType = BEGIN_SYSTEM_CHANGE;
                rp.dwRestorePtType = MODIFY_SETTINGS;
                rp.llSequenceNumber = 0;
                std::wstring desc = L"CL-Setup v" + std::wstring(Helper::g_appVersion.begin(), Helper::g_appVersion.end()) + L" - Security mitigation changes";
                wcsncpy_s(rp.szDescription, desc.c_str(), _TRUNCATE);
                STATEMGRSTATUS sms;
                if (SRSetRestorePoint(&rp, &sms)) {
                    rp.dwEventType = END_SYSTEM_CHANGE;
                    SRSetRestorePoint(&rp, &sms);
                    Helper::printSuccess("- System Restore Point created", true);
                }
            }
            FreeLibrary(hSrDll);
        }
    }

    std::string bcdOutput = lower(Helper::runSystemCommandWithOutput("bcdedit /enum {current}", 5000));
    if (bcdOutput == "timeout" || bcdOutput.empty()) {
        Helper::printConcern("- Could not read hypervisor launch status");
        appendSummary("hypervisorlaunchtype=unknown");
        warning = true;
    }
    else if (bcdOutput.find("hypervisorlaunchtype") != std::string::npos &&
        bcdOutput.find("off") != std::string::npos) {
        Helper::printSuccess("- Hypervisor launch is already off", false);
        appendSummary("hypervisorlaunchtype=off");
    }
    else {
        if (!Helper::requestChange("Set hypervisorlaunchtype to off")) {
            appendSummary("hypervisorlaunchtype=change pending");
            warning = true;
            return;
        }
        Helper::runSystemCommand("bcdedit /set hypervisorlaunchtype off");
        Helper::printSuccess("- Set hypervisor launch type to off", true);
        appendSummary("hypervisorlaunchtype=changed to off");
        changed = true;
    }

    auto checkAndDisableFeature = [&](const std::string& featureName) {
        std::string queryCommand = "dism /online /get-featureinfo /featurename:" + featureName + " 2>&1";
        std::string output = lower(Helper::runSystemCommandWithOutput(queryCommand.c_str(), 15000));
        if (output == "timeout" || output.empty()) {
            Helper::printConcern("- Could not read Windows feature status: " + featureName);
            appendSummary(featureName + "=unknown");
            warning = true;
            return;
        }

        if (output.find("error") != std::string::npos ||
            output.find("fehler") != std::string::npos ||
            output.find("not found") != std::string::npos ||
            output.find("nicht") != std::string::npos ||
            output.find("0x800f") != std::string::npos) {
            Helper::printSuccess("- Windows feature is not available: " + featureName, false);
            appendSummary(featureName + "=disabled (not available)");
            return;
        }

        if (output.find("state : disabled") != std::string::npos ||
            output.find("state: disabled") != std::string::npos ||
            output.find("deaktiviert") != std::string::npos) {
            Helper::printSuccess("- Windows feature is already disabled: " + featureName, false);
            appendSummary(featureName + "=disabled");
            return;
        }

        if (output.find("state : enabled") == std::string::npos &&
            output.find("state: enabled") == std::string::npos &&
            output.find("aktiviert") == std::string::npos) {
            Helper::printInfo("Windows feature status is unclear (treated as disabled): " + featureName);
            appendSummary(featureName + "=likely disabled");
            return;
        }

        if (!Helper::requestChange("Disable Windows feature " + featureName)) {
            appendSummary(featureName + "=disable pending");
            warning = true;
            return;
        }

        std::string disableCommand = "dism /online /disable-feature /featurename:" + featureName + " /norestart";
        Helper::runSystemCommand(disableCommand.c_str());
        Helper::printSuccess("- Disabled Windows feature: " + featureName, true);
        appendSummary(featureName + "=changed to disabled");
        changed = true;
    };

    checkAndDisableFeature("Microsoft-Hyper-V-Hypervisor");
    checkAndDisableFeature("Microsoft-Hyper-V-All");

    if (changed) {
        Helper::restartRequired = true;
        Helper::recordResult("Security Mitigations", (failed || warning) ? "WARN" : "OK", "Changed: " + summary);
    }
    else if (failed) {
        Helper::recordResult("Security Mitigations", "FAIL", summary);
    }
    else if (warning) {
        Helper::recordResult("Security Mitigations", "WARN", summary);
    }
    else {
        Helper::recordResult("Security Mitigations", "OK", "Already configured: " + summary);
    }
}

void Checks::checkVM()
{
    SetConsoleTitleA("Checking for Virtual Machine");

    bool isVM = false;
    std::string vmType;

    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VBoxGuest", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = "VirtualBox"; RegCloseKey(hKey);
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\VMware, Inc.\\VMware Tools", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = vmType.empty() ? "VMware" : vmType + "/VMware"; RegCloseKey(hKey);
    }
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\ACPI\\DSDT\\VBOX__", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = vmType.empty() ? "VirtualBox (ACPI)" : vmType + "/VBox-ACPI"; RegCloseKey(hKey);
    }

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        if (OpenService(scm, "VBoxService", SERVICE_QUERY_STATUS)) {
            isVM = true; vmType = vmType.empty() ? "VirtualBox" : vmType;
        }
        CloseServiceHandle(scm);
    }

    if (isVM) {
        Helper::printError("- Running inside a Virtual Machine (" + vmType + ")");
        Helper::recordResult("VM Detection", "FAIL", "Detected: " + vmType);
    }
    else {
        Helper::recordResult("VM Detection", "OK", "Bare metal");
    }
}

void Checks::checkForUpdate()
{
    SetConsoleTitleA("Checking for Updates");

    // Cache path with repo-validated cache
    auto getUpdateCachePath = [&]() -> std::string {
        char basePath[MAX_PATH];
        GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH);
        std::string dir = std::string(basePath) + "\\CL-Setup";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\update-check.txt";
    };

    auto isCacheFresh = [](const std::string& path) -> bool {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
            return false;
        }
        FILETIME nowFileTime;
        GetSystemTimeAsFileTime(&nowFileTime);
        ULARGE_INTEGER now;
        now.LowPart = nowFileTime.dwLowDateTime;
        now.HighPart = nowFileTime.dwHighDateTime;
        ULARGE_INTEGER lastWrite;
        lastWrite.LowPart = data.ftLastWriteTime.dwLowDateTime;
        lastWrite.HighPart = data.ftLastWriteTime.dwHighDateTime;
        const unsigned long long oneWeek = 7ULL * 24ULL * 60ULL * 60ULL * 10000000ULL;
        return now.QuadPart >= lastWrite.QuadPart && (now.QuadPart - lastWrite.QuadPart) < oneWeek;
    };

    auto readCachedTag = [](const std::string& path, const std::string& expectedRepo) -> std::string {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string storedRepo;
            std::getline(file, storedRepo);
            std::string tag;
            if (std::getline(file, tag)) {
                if (storedRepo == expectedRepo && !tag.empty()) {
                    return tag;
                }
            }
        }
        return "";
    };

    auto writeCachedTag = [](const std::string& path, const std::string& tag, const std::string& repo) {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << repo << "\n";
            file << tag << "\n";
        }
    };

    std::string cachePath = getUpdateCachePath();
    std::string tag = readCachedTag(cachePath, Helper::g_repoUrl);
    bool usedCache = isCacheFresh(cachePath) && !tag.empty();

    if (!usedCache) {
        std::string json = Helper::fetchURL(L"https://api.github.com/repos/" +
            std::wstring(Helper::g_repoUrl.begin(), Helper::g_repoUrl.end()) + L"/releases/latest");

        if (json.empty()) {
            if (!tag.empty()) {
                Helper::printConcern("- Could not refresh update info, using cached latest version: " + tag);
            }
            else {
                Helper::printConcern("- Could not check for updates (no response from GitHub)");
                Helper::recordResult("Auto-Update", "WARN", "Could not reach GitHub API");
                return;
            }
        }
        else {
            tag.clear();
            auto tagPos = json.find("\"tag_name\":\"");
            if (tagPos != std::string::npos) {
                tagPos += 12;
                auto endPos = json.find("\"", tagPos);
                if (endPos != std::string::npos) {
                    tag = json.substr(tagPos, endPos - tagPos);
                }
            }

            if (tag.empty()) {
                Helper::printConcern("- Could not parse update info");
                Helper::recordResult("Auto-Update", "WARN", "Could not parse release info");
                return;
            }
            writeCachedTag(cachePath, tag, Helper::g_repoUrl);
        }
    }

    if (tag != "v" + Helper::g_appVersion && tag != Helper::g_appVersion) {
        Helper::printConcern("- New version available: " + tag + " (current: v" + Helper::g_appVersion + ")");
        if (!Helper::cliConfig.headless) {
            MessageBoxA(GetConsoleWindow(),
                ("A newer Setup Checker version is available: " + tag +
                    "\n\nCurrent version: v" + Helper::g_appVersion +
                    "\n\nDownload it from the GitHub releases page.").c_str(),
                "Update available",
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }
        Helper::recordResult("Auto-Update", "WARN", "New version: " + tag + (usedCache ? " (cached weekly check)" : ""));
    }
    else {
        Helper::printSuccess("- No new updates (v" + Helper::g_appVersion + ")", false);
        Helper::recordResult("Auto-Update", "OK",
            "Up to date (v" + Helper::g_appVersion + ")" + (usedCache ? " - checked this week" : ""));
    }
}

bool Checks::checkDefenderTamperProtection()
{
    DWORD tamperProtection = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows Defender\\Features", "TamperProtection", &tamperProtection)) {
        if (tamperProtection == 5) {
            Helper::printError("- Windows Defender Tamper Protection is ENABLED");
            Helper::printConcern("  Disable it manually: Windows Security > Virus & threat protection > Manage protection > Tamper Protection");
            Helper::recordResult("Defender Tamper Protection", "FAIL", "Enabled (value=5)");
            return true;
        }
    }
    Helper::recordResult("Defender Tamper Protection", "OK", "Disabled");
    return false;
}

bool Checks::checkAVTamperProtection(const std::string& avName)
{
    const std::vector<std::pair<std::string, std::string>> tamperServices = {
        {"Norton", "SAVService"},       {"Norton", "ccEvtMgr"},
        {"McAfee", "McShield"},         {"McAfee", "mfetd"},
        {"Kaspersky", "AVP"},           {"Kaspersky", "klnagent"},
        {"Bitdefender", "BDAuxSrv"},    {"Bitdefender", "EPSecurityService"},
        {"ESET", "ESET Service"},       {"ESET", "ekrn"},
        {"AVG", "avgsvc"},              {"Avast", "avast! Service"},
    };

    std::string lowerAV = avName;
    std::transform(lowerAV.begin(), lowerAV.end(), lowerAV.begin(), ::tolower);

    for (const auto& [name, service] : tamperServices) {
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerAV.find(lowerName) != std::string::npos) {
            ServiceStatus status = Helper::getServiceStatus(service.c_str());
            if (status == STATUS_SERVICE_RUNNING) {
                Helper::printError("- " + name + " Tamper Protection active (service: " + service + ")");
                Helper::printConcern("  Disable it manually in " + name + " settings before uninstalling");
                Helper::recordResult("AV Tamper Protection", "FAIL", name + " active");
                return true;
            }
        }
    }
    return false;
}

void Checks::checkWindowsDefender()
{
    SetConsoleTitleA("Checking Windows Defender");

    if (checkDefenderTamperProtection()) {
        return;
    }

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

    if (!Helper::requestChange("Disable Windows Defender service and policy settings")) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        Helper::recordResult("Windows Defender", "WARN", "Enabled - disable pending approval");
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

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall");

    auto runWMIC = [](const std::string& cmd) -> std::string {
        std::string fullCmd = cmd + " 2>nul";
        std::FILE* pipe = _popen(fullCmd.c_str(), "r");
        if (!pipe) {
            if (!Helper::requestChange("Enable WMIC optional feature for antivirus detection")) {
                return std::string("ERROR");
            }
            Helper::runSystemCommand("dism /online /enable-feature /featurename:WMIC /quiet /norestart 2>nul");
            Sleep(500);
            pipe = _popen(fullCmd.c_str(), "r");
        }
        if (!pipe) return "ERROR";
        char buffer[256]; std::string result;
        while (std::fgets(buffer, 256, pipe)) result += buffer;
        _pclose(pipe);
        return result;
    };

    std::string wmiOutput = runWMIC("WMIC /Node:localhost /Namespace:\\\\root\\SecurityCenter2 Path AntiVirusProduct Get displayName /Format:List");
    if (wmiOutput == "ERROR") {
        Helper::printConcern("- WMIC not available - possible modified OS");
        Helper::recordResult("3rd Party AV", "WARN", "WMIC not available");
        return;
    }

    std::string antivirusList;
    while (wmiOutput.find("\n") != std::string::npos) {
        std::string av = wmiOutput.substr(0, wmiOutput.find("\n"));
        if (av.find("Windows Defender") == std::string::npos && av.size() > 12) {
            av = av.substr(12);
            av.erase(std::remove(av.begin(), av.end(), '\n'), av.end());
            av.erase(std::remove(av.begin(), av.end(), '\r'), av.end());
            av.erase(std::remove(av.begin(), av.end(), '\b'), av.end());
            if (!av.empty()) {
                if (!antivirusList.empty()) antivirusList += ", ";
                antivirusList += av;
            }
        }
        size_t pos = wmiOutput.find("\n");
        if (pos + 1 < wmiOutput.size()) wmiOutput = wmiOutput.substr(pos + 1);
        else break;
    }

    if (!antivirusList.empty()) {
        // Check tamper protection before attempting uninstall
        std::string lowerList = antivirusList;
        std::transform(lowerList.begin(), lowerList.end(), lowerList.begin(), ::tolower);
        bool tamperActive = false;

        const std::vector<std::string> avNames = {
            "norton", "mcafee", "kaspersky", "bitdefender", "eset", "avg", "avast"
        };
        for (const auto& av : avNames) {
            if (lowerList.find(av) != std::string::npos) {
                if (checkAVTamperProtection(av)) tamperActive = true;
            }
        }

        if (tamperActive) {
            Helper::printConcern("- Cannot auto-uninstall while tamper protection is active");
            Helper::recordResult("3rd Party AV", "WARN", "Tamper protection active: " + antivirusList);
            return;
        }

        if (!Helper::requestChange("Attempt to uninstall detected third-party antivirus: " + antivirusList)) {
            Helper::recordResult("3rd Party AV", "WARN", "Found, uninstall pending approval: " + antivirusList);
            return;
        }

        // Try to auto-uninstall
        bool anyRemoved = false;
        HKEY hKey;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD idx = 0; char subkeyName[256]; DWORD subkeyNameSize = sizeof(subkeyName);
            while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                HKEY hSubkey;
                if (RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey) == ERROR_SUCCESS) {
                    char dn[256] = ""; DWORD dnSize = sizeof(dn);
                    RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize);
                    std::string displayName(dn);
                    for (auto& av : {antivirusList}) {
                        size_t comma = 0;
                        while (comma != std::string::npos) {
                            std::string single = av.substr(comma == 0 ? 0 : comma + 2);
                            comma = av.find(",", comma == 0 ? 0 : comma + 2);
                            if (displayName.find(single) != std::string::npos) {
                                char uninstall[512] = ""; DWORD usSize = sizeof(uninstall);
                                if (RegQueryValueEx(hSubkey, "UninstallString", NULL, NULL, (LPBYTE)uninstall, &usSize) == ERROR_SUCCESS) {
                                    std::string cmd(uninstall);
                                    cmd += " /S /quiet /norestart";
                                    Helper::runSystemCommand(cmd.c_str());
                                    anyRemoved = true;
                                }
                                break;
                            }
                        }
                    }
                    RegCloseKey(hSubkey);
                }
                subkeyNameSize = sizeof(subkeyName); ++idx;
            }
            RegCloseKey(hKey);
        }

        if (anyRemoved) {
            Helper::printSuccess("- Attempted to remove 3rd party AV (" + antivirusList + ")", true);
            Helper::recordResult("3rd Party AV", "OK", "Uninstall attempted: " + antivirusList);
        } else {
            Helper::printError("- 3rd party Anti-Virus detected: " + antivirusList);
            Helper::recordResult("3rd Party AV", "FAIL", "Found: " + antivirusList);
        }
        return;
    }

    Helper::recordResult("3rd Party AV", "OK", "None detected");
}

void Checks::checkCPUV()
{
    SetConsoleTitleA("Checking CPUV-V");

    auto runWMIC = [](const std::string& cmd) -> std::string {
        std::string fullCmd = cmd + " 2>nul";
        std::FILE* pipe = _popen(fullCmd.c_str(), "r");
        if (!pipe) {
            if (!Helper::requestChange("Enable WMIC optional feature for CPU virtualization detection")) {
                return std::string("ERROR");
            }
            Helper::runSystemCommand("dism /online /enable-feature /featurename:WMIC /quiet /norestart 2>nul");
            Sleep(500);
            pipe = _popen(fullCmd.c_str(), "r");
        }
        if (!pipe) return "ERROR";
        char buffer[256]; std::string result;
        while (std::fgets(buffer, 256, pipe)) result += buffer;
        _pclose(pipe);
        return result;
    };

    std::string result = runWMIC("WMIC CPU Get VirtualizationFirmwareEnabled");
    if (result == "ERROR") {
        Helper::printConcern("- WMIC not available - possible modified OS");
        Helper::recordResult("CPU-V", "WARN", "WMIC not available");
        return;
    }

    std::string lowerResult = result;
    std::transform(lowerResult.begin(), lowerResult.end(), lowerResult.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lowerResult.find("true") != std::string::npos) {
        Helper::printConcern("- CPU-V is enabled in BIOS. Consider disabling in BIOS if not needed");
        Helper::recordResult("CPU-V", "WARN", "Enabled in BIOS");
        return;
    }

    Helper::recordResult("CPU-V", "OK", "Disabled");
}

void Checks::uninstallRiotVanguard()
{
    SetConsoleTitleA("Checking for Riot Vanguard");

    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to check if Riot Vanguard is installed (Error 6, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Riot Vanguard", "FAIL", "RegOpen failed");
        return;
    }

    DWORD subkeyIndex = 0;
    char subkeyName[256];
    DWORD subkeyNameSize = sizeof(subkeyName);
    while (RegEnumKeyEx(hKey, subkeyIndex, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hSubkey;
        result = RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey);
        if (result != ERROR_SUCCESS) { RegCloseKey(hKey); Helper::recordResult("Riot Vanguard", "FAIL", "RegEnum failed"); return; }

        char displayName[256];
        DWORD displayNameSize = sizeof(displayName);
        result = RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)displayName, &displayNameSize);
        if (result == ERROR_SUCCESS && strcmp(displayName, "Riot Vanguard") == 0) {
            RegCloseKey(hSubkey);
            RegCloseKey(hKey);

            if (std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe") ||
                std::filesystem::exists("C:\\Program Files (x86)\\Riot Vanguard\\installer.exe")) {
                if (!Helper::requestChange("Uninstall Riot Vanguard")) {
                    Helper::printError("- Riot Vanguard is installed");
                    Helper::recordResult("Riot Vanguard", "WARN", "Installed - uninstall pending approval");
                    return;
                }

                if (!Helper::cliConfig.headless) {
                    MessageBoxA(NULL, "When prompted to uninstall Vanguard, press YES", "Uninstall Vanguard", MB_ICONINFORMATION | MB_TOPMOST);
                }

                const char* vgPath = std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe")
                    ? "C:\\Program Files\\Riot Vanguard\\installer.exe"
                    : "C:\\Program Files (x86)\\Riot Vanguard\\installer.exe";
                _spawnl(_P_WAIT, vgPath, "installer.exe", NULL);
                Helper::printError("- Riot Vanguard needs to be uninstalled");
                Helper::recordResult("Riot Vanguard", "FAIL", "Installed - prompted uninstall");
                return;
            }
            Helper::printError("- Failed to uninstall Riot Vanguard, manually uninstall (Error 8)");
            Helper::recordResult("Riot Vanguard", "FAIL", "Installed - manual uninstall needed");
            return;
        }
        RegCloseKey(hSubkey);
        subkeyNameSize = sizeof(subkeyName);
        ++subkeyIndex;
    }
    RegCloseKey(hKey);
    Helper::printSuccess("- Riot Vanguard is not installed", false);
    Helper::recordResult("Riot Vanguard", "OK", "Not installed");
}

void Checks::installVCRedist()
{
    SetConsoleTitleA("Downloading VCRedist");
    Helper::vcComplete = false;

    bool alreadyInstalled = (std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") ||
                             std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll")) &&
                            (std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll") ||
                             std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"));
    if (alreadyInstalled) {
        Helper::printSuccess("- VCRedist is already installed", false);
        Helper::recordResult("VC Redist", "OK", "Already installed");
        Helper::vcComplete = true;
        return;
    }

    if (!Helper::requestChange("Download and install Microsoft Visual C++ Redistributable")) {
        Helper::printConcern("- VCRedist is missing");
        Helper::recordResult("VC Redist", "WARN", "Missing - install pending approval");
        Helper::vcComplete = true;
        return;
    }

    HRESULT downloadX64 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x64.exe",
        "C:\\Windows\\VC_redist.x64.exe", 0, NULL);
    HRESULT downloadX86 = URLDownloadToFileA(NULL, "https://aka.ms/vs/17/release/vc_redist.x86.exe",
        "C:\\Windows\\VC_redist.x86.exe", 0, NULL);

    if (downloadX64 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x64.exe")) {
        Helper::printError("- Failed to download VCRedist x64 (Error 9, HR=" + std::to_string(downloadX64) + ")");
        Helper::recordResult("VC Redist", "FAIL", "Download x64 failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }
    if (downloadX86 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x86.exe")) {
        Helper::printError("- Failed to download VCRedist x86 (Error 10, HR=" + std::to_string(downloadX86) + ")");
        Helper::recordResult("VC Redist", "FAIL", "Download x86 failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }

    SetConsoleTitleA("Installing VCRedist");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x64.exe /install /q /norestart");
    Helper::runSystemCommand("C:\\Windows\\VC_redist.x86.exe /install /q /norestart");

    if (!(std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") ||
          std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll")) ||
        !(std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll") ||
          std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"))) {
        Helper::printError("- VCRedist didn't install correctly (Error 11)");
        Helper::recordResult("VC Redist", "FAIL", "Install verify failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
        Helper::vcComplete = true;
        return;
    }

    Helper::printSuccess("- VCRedist is installed", false);
    Helper::recordResult("VC Redist", "OK", "Installed successfully");
    Helper::vcComplete = true;
}

void Checks::checkSecureBoot()
{
    SetConsoleTitleA("Checking Secure-Boot");
    DWORD secbootStatus;
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", &secbootStatus);

    if (secbootStatus == 0x00000000) {
        Helper::printSuccess("- Secure-Boot is disabled", false);
        Helper::recordResult("Secure Boot", "OK", "Disabled");
    }
    else if (secbootStatus == 0x00000001) {
        Helper::printError("- Secure-Boot is enabled, please disable Secure-Boot in your BIOS");
        Helper::recordResult("Secure Boot", "FAIL", "Enabled");
    }
    else {
        Helper::printError("- Unable to check Secure-Boot Status (Error 12)");
        Helper::recordResult("Secure Boot", "FAIL", "Unknown state");
    }
}

void Checks::isChromeInstalled()
{
    SetConsoleTitleA("Checking for Google Chrome");

    std::vector<std::wstring> paths = {
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    };
    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        paths.push_back(std::wstring(localAppData) + L"\\Google\\Chrome\\Application\\chrome.exe");
    }
    for (auto& p : paths) {
        if (std::filesystem::exists(p)) {
            Helper::printSuccess("- Google Chrome is installed", false);
            Helper::recordResult("Google Chrome", "OK", "Installed");
            return;
        }
    }
    Helper::printError("- Google Chrome is not installed");
    if (!Helper::requestChange("Open Google Chrome download page")) {
        Helper::recordResult("Google Chrome", "WARN", "Not installed - download pending approval");
        return;
    }
    Helper::recordResult("Google Chrome", "FAIL", "Not installed");
    Sleep(1000);
    Helper::runSystemCommand("start https://www.google.com/chrome/");
}

void Checks::syncWindowsTime()
{
    SetConsoleTitleA("Syncing Windows Time");

    // First check if time service is already running
    ServiceStatus currentStatus = Helper::getServiceStatus("W32Time");
    if (currentStatus == STATUS_SERVICE_RUNNING) {
        // Already running - just resync
        Helper::runSystemCommand("w32tm /resync");
        Helper::printSuccess("- Windows Time is running", false);
        Helper::recordResult("Time Sync", "OK", "Running, resynced");
        return;
    }

    SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scmHandle == NULL) {
        DWORD err = GetLastError();
        Helper::printError("- Failed to sync Windows Time (Error 14, GLE=" + std::to_string(err) + ")");
        Helper::recordResult("Time Sync", "FAIL", "SCM open failed");
        return;
    }
    if (!Helper::requestChange("Repair and start Windows Time service")) {
        CloseServiceHandle(scmHandle);
        Helper::recordResult("Time Sync", "WARN", "Disabled - repair pending approval");
        return;
    }

    Helper::runSystemCommand("w32tm /register");

    if (Helper::getServiceStatus("W32Time") == STATUS_SERVICE_STOPPED) {
        SC_HANDLE serviceHandle = OpenService(scmHandle, "W32Time", SERVICE_ALL_ACCESS);
        if (serviceHandle == NULL) {
            Helper::printError("- Failed to sync Windows Time (Error 15)");
            Helper::recordResult("Time Sync", "FAIL", "OpenService failed");
            CloseServiceHandle(scmHandle);
            return;
        }
        DWORD bytesNeeded = 0;
        QueryServiceConfig(serviceHandle, NULL, 0, &bytesNeeded);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytesNeeded == 0) {
            Helper::printError("- Failed to sync Windows Time (Error 16)");
            Helper::recordResult("Time Sync", "FAIL", "Config query failed");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        std::vector<BYTE> buffer(bytesNeeded);
        LPQUERY_SERVICE_CONFIG serviceConfig = (LPQUERY_SERVICE_CONFIG)buffer.data();
        if (!QueryServiceConfig(serviceHandle, serviceConfig, bytesNeeded, &bytesNeeded)) {
            Helper::printError("- Failed to sync Windows Time (Error 16)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        if (serviceConfig->dwStartType == SERVICE_DISABLED) {
            Helper::printError("- Failed to sync Windows Time (Error 17)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        if (!ChangeServiceConfig(serviceHandle, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            Helper::printError("- Failed to sync Windows Time (Error 18)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        if (StartService(serviceHandle, 0, NULL) == FALSE) {
            Helper::printError("- Failed to sync Windows Time (Error 19)");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        Sleep(250);
    }
    else { CloseServiceHandle(scmHandle); }

    Helper::runSystemCommand("w32tm /resync");
    Helper::printSuccess("- Successfully synced Windows time", true);
    Helper::recordResult("Time Sync", "OK", "Synced (was disabled)");
}

void Checks::disableChromeProtection()
{
    SetConsoleTitleA("Disabling Google Chrome Protection");

    bool chromeInstalled = std::filesystem::exists(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe") ||
                           std::filesystem::exists(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    if (!chromeInstalled) {
        Helper::recordResult("Chrome Protection", "SKIPPED", "Chrome not installed");
        return;
    }

    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        "SafeBrowsingProtectionLevel", &status)) {
        if (status == 0x00000001) {
            Helper::printSuccess("- Chrome protection is already disabled", false);
            Helper::recordResult("Chrome Protection", "OK", "Already disabled");
            return;
        }
    }

    if (!Helper::requestChange("Disable Chrome Safe Browsing protection policy")) {
        Helper::recordResult("Chrome Protection", "WARN", "Enabled - policy change pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0x00000001;
    LONG createKey = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG createDWORD = RegSetValueEx(hKey, "SafeBrowsingProtectionLevel", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);

    if (createKey == ERROR_SUCCESS && createDWORD == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Chrome protection", true);
        Helper::recordResult("Chrome Protection", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Chrome protection (Error 20/21)");
        Helper::recordResult("Chrome Protection", "FAIL", "Registry write failed");
    }
}

void Checks::checkWinver()
{
    SetConsoleTitleA("Checking Winver");
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Registry open failed");
        return;
    }
    char buildStr[32]; DWORD buildSize = sizeof(buildStr);
    LONG ret = RegQueryValueEx(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)buildStr, &buildSize);
    RegCloseKey(hKey);
    if (ret != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Read failed");
        return;
    }
    int build = std::stoi(buildStr);

    char productName[128] = ""; DWORD productSize = sizeof(productName);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)productName, &productSize);
        RegCloseKey(hKey);
    }
    std::string winver(productName);

    std::map<int, std::string> build_map = {
        {10240, "Win10 1507"}, {10586, "Win10 1511"}, {14393, "Win10 1607"}, {15063, "Win10 1703"},
        {16299, "Win10 1709"}, {17134, "Win10 1803"}, {17763, "Win10 1809"}, {18362, "Win10 1903"},
        {19041, "Win10 2004"}, {19042, "Win10 20H2"}, {19043, "Win10 21H1"}, {19044, "Win10 21H2"},
        {19045, "Win10 22H2"}, {22000, "Win11 21H2"}, {22621, "Win11 22H2"}, {22631, "Win11 23H2"},
        {26100, "Win11 24H2"},
    };
    int min_build = 19041;
    std::vector<int> trouble = {19045, 22621};

    auto it = build_map.find(build);
    if (it != build_map.end()) { winver = it->second; }

    if (build < min_build) {
        Helper::printError("- Winver \"" + winver + "\" is unsupported, please upgrade");
        Helper::recordResult("Winver", "FAIL", "Unsupported: " + winver + " (Build " + std::to_string(build) + ")");
    }
    else if (std::find(trouble.begin(), trouble.end(), build) != trouble.end()) {
        Helper::printConcern("- Winver \"" + winver + "\" is a 50/50, if issues occur contact support");
        Helper::recordResult("Winver", "WARN", "Troublesome: " + winver);
    }
    else {
        Helper::printSuccess("- Winver is supported (" + winver + ")", false);
        Helper::recordResult("Winver", "OK", winver);
    }
}

void Checks::deleteSymbols()
{
    SetConsoleTitleA("Deleting C:\\Symbols");
    std::string path = "C:\\Symbols";
    if (std::filesystem::exists(path)) {
        if (!Helper::requestChange("Delete " + path)) {
            Helper::recordResult("Symbols", "WARN", "Present - delete pending approval");
            return;
        }

        std::error_code ec;
        if (!std::filesystem::remove_all(path, ec)) {
            Helper::printError("- Unable to delete " + path + " (Error 24, " + ec.message() + ")");
            Helper::recordResult("Symbols", "FAIL", "Delete failed: " + ec.message());
            return;
        }
        Helper::printSuccess("- Successfully deleted " + path, true);
        Helper::recordResult("Symbols", "OK", "Deleted (changed)");
        return;
    }
    Helper::printSuccess("- C:\\Symbols does not exist", false);
    Helper::recordResult("Symbols", "OK", "Not present");
}

void Checks::checkFastBoot()
{
    SetConsoleTitleA("Checking Fast-Boot");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", "HiberbootEnabled", &status)) {
        if (status == 0) {
            Helper::printSuccess("- Fast-Boot is disabled", false);
            Helper::recordResult("Fast Boot", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable Fast Boot")) {
        Helper::recordResult("Fast Boot", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "HiberbootEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Fast-Boot", true);
        Helper::restartRequired = true;
        Helper::recordResult("Fast Boot", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Fast-Boot (Error 31/32)");
        Helper::recordResult("Fast Boot", "FAIL", "Registry write failed");
    }
}

void Checks::checkExploitProtection()
{
    SetConsoleTitleA("Checking Exploit-Protection");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        "DisallowExploitProtectionOverride", &status)) {
        if (status == 1) {
            Helper::printSuccess("- Exploit-Protection is disabled", false);
            Helper::recordResult("Exploit Protection", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable Exploit Protection override")) {
        Helper::recordResult("Exploit Protection", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "DisallowExploitProtectionOverride", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled Exploit-Protection", true);
        Helper::restartRequired = true;
        Helper::recordResult("Exploit Protection", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable Exploit-Protection (Error 25/26)");
        Helper::recordResult("Exploit Protection", "FAIL", "Registry write failed");
    }
}

void Checks::checkSmartScreen()
{
    SetConsoleTitleA("Checking SmartScreen");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows\\System", "EnableSmartScreen", &status)) {
        if (status == 0) {
            Helper::printSuccess("- SmartScreen is disabled", false);
            Helper::recordResult("SmartScreen", "OK", "Already disabled");
            return;
        }
    }
    if (!Helper::requestChange("Disable SmartScreen")) {
        Helper::recordResult("SmartScreen", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "EnableSmartScreen", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully disabled SmartScreen", true);
        Helper::restartRequired = true;
        Helper::recordResult("SmartScreen", "OK", "Disabled (changed)");
    }
    else {
        Helper::printError("- Failed to disable SmartScreen (Error 27/28)");
        Helper::recordResult("SmartScreen", "FAIL", "Registry write failed");
    }
}

void Checks::checkGameBar()
{
    SetConsoleTitleA("Checking Xbox Gamebar");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", "AppCaptureEnabled", &status)) {
        if (status == 1) {
            Helper::printSuccess("- Gamebar is enabled", false);
            Helper::recordResult("Game Bar", "OK", "Already enabled");
            return;
        }
    }
    if (!Helper::requestChange("Enable Xbox Game Bar capture setting")) {
        Helper::recordResult("Game Bar", "WARN", "Disabled - enable pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = RegSetValueEx(hKey, "AppCaptureEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Successfully enabled Gamebar", true);
        Helper::restartRequired = true;
        Helper::recordResult("Game Bar", "OK", "Enabled (changed)");
    }
    else {
        Helper::printError("- Failed to enable Gamebar (Error 29/30)");
        Helper::recordResult("Game Bar", "FAIL", "Registry write failed");
    }
}

void Checks::checkModifiedOS()
{
    SetConsoleTitleA("Checking if OS is modified");
    bool modified = false;
    std::string reasons;

    auto checkService = [&](const char* name, const char* label) {
        SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenService(scm, name, SERVICE_QUERY_STATUS);
            if (svc == NULL) { modified = true; reasons += std::string(" ") + label + " missing;"; }
            else CloseServiceHandle(svc);
            CloseServiceHandle(scm);
        }
    };
    checkService("WinDefend", "WinDefend");
    checkService("wscsvc", "SecurityCenter");
    checkService("wuauserv", "WindowsUpdate");
    checkService("BFE", "BFE");
    checkService("mpssvc", "Firewall");

    HKEY hKey;
    auto checkReg = [&](const char* key, const char* label) {
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            modified = true; reasons += std::string(" ") + label + ";"; RegCloseKey(hKey);
        }
    };
    checkReg("SOFTWARE\\AtlasOS", "AtlasOS");
    checkReg("SOFTWARE\\ReviOS", "ReviOS");
    checkReg("SOFTWARE\\GhostSpectre", "GhostSpectre");
    checkReg("SOFTWARE\\GGOS", "GGOS");
    checkReg("SOFTWARE\\PhoenixOS", "PhoenixOS");
    checkReg("SOFTWARE\\KernelOS", "KernelOS");
    checkReg("SOFTWARE\\XOS", "XOS");
    checkReg("SOFTWARE\\OptimusOS", "OptimusOS");

    char productName[256] = ""; DWORD size = sizeof(productName);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, "ProductName", NULL, NULL, (LPBYTE)productName, &size);
        RegCloseKey(hKey);
    }
    std::string pn(productName);
    std::vector<std::string> markers = {"Atlas","Revi","Ghost","Spectre","Tiny","Lite","X-Lite","Micro","XOS","Optimus","SuperLite","Compact","Minimal","Gaming"};
    for (auto& m : markers) if (pn.find(m) != std::string::npos) { modified = true; reasons += " ProductName(" + m + ");"; break; }

    char editionID[256] = ""; DWORD edSize = sizeof(editionID);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, "EditionID", NULL, NULL, (LPBYTE)editionID, &edSize) == ERROR_SUCCESS) {
            std::string ed(editionID);
            if (ed.find("Custom") != std::string::npos || ed.find("Gaming") != std::string::npos) {
                modified = true; reasons += " Edition(" + ed + ");";
            }
        }
        RegCloseKey(hKey);
    }

    if (modified) {
        Helper::printConcern("- OS appears MODIFIED:" + reasons);
        Helper::recordResult("Modified OS", "WARN", "Modified:" + reasons);
    }
    else {
        Helper::printSuccess("- The OS appears unmodified", false);
        Helper::recordResult("Modified OS", "OK", "Clean");
    }
}

void Checks::checkTPMStatus()
{
    SetConsoleTitleA("Checking TPM");
    DWORD tpmActive = 0; bool found = false;
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\TPM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        if (RegQueryValueEx(hKey, "TPMActive", NULL, NULL, (LPBYTE)&tpmActive, &size) == ERROR_SUCCESS) found = true;
        RegCloseKey(hKey);
    }
    if (!found || tpmActive == 0) {
        Helper::printSuccess("- TPM is disabled / not found", false);
        Helper::recordResult("TPM", "OK", "Disabled/not found");
    }
    else {
        Helper::printError("- TPM is enabled, please disable TPM in BIOS");
        Helper::recordResult("TPM", "FAIL", "Enabled");
    }
}

void Checks::checkCoreIsolation()
{
    SetConsoleTitleA("Checking Core Isolation (VBS/HVCI)");

    DWORD hvci = 0;
    bool hvciRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", "Enabled", &hvci);

    DWORD vbs = 0;
    bool vbsRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", &vbs);

    if (hvciRead && hvci == 0 && vbsRead && vbs == 0) {
        Helper::printSuccess("- Core Isolation (VBS/HVCI) is disabled", false);
        Helper::recordResult("Core Isolation", "OK", "Disabled");
        return;
    }

    bool changed = false;
    HKEY hKey;

    if (hvci == 1) {
        if (!Helper::requestChange("Disable Core Isolation HVCI")) {
            Helper::recordResult("Core Isolation", "WARN", "HVCI enabled - disable pending approval");
            return;
        }

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "Enabled", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                Helper::printSuccess("- Disabled HVCI", true);
                Helper::recordResult("Core Isolation", "OK", "HVCI disabled (changed)");
                changed = true;
            }
            else {
                Helper::printError("- Failed to disable HVCI (access denied?)");
                Helper::printConcern("  Disable manually: Windows Security > Device Security > Core Isolation");
                Helper::recordResult("Core Isolation", "FAIL", "HVCI write failed");
            }
            RegCloseKey(hKey);
        }
    }

    if (vbs == 1) {
        if (!Helper::requestChange("Disable Virtualization Based Security")) {
            Helper::recordResult("Core Isolation", "WARN", "VBS enabled - disable pending approval");
            return;
        }

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "EnableVirtualizationBasedSecurity", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                if (!changed) Helper::printSuccess("- Disabled VBS", true);
                Helper::recordResult("Core Isolation", "OK", "VBS disabled (changed)");
                changed = true;
            }
            RegCloseKey(hKey);
        }
    }

    if (changed) {
        Helper::restartRequired = true;
    }
    else if (hvci == 1 || vbs == 1) {
        Helper::printConcern("- Could not disable Core Isolation, disable manually in Windows Security");
        Helper::recordResult("Core Isolation", "FAIL", "Manual disable needed");
    }
    else {
        Helper::printSuccess("- Core Isolation (VBS/HVCI) is disabled", false);
        Helper::recordResult("Core Isolation", "OK", "Disabled");
    }
}

void Checks::checkDMAProtection()
{
    SetConsoleTitleA("Checking Kernel DMA Protection");
    DWORD dma = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\KernelDmaProtection", "Enabled", &dma)) {
        if (dma == 0) {
            Helper::printSuccess("- Kernel DMA Protection is disabled", false);
            Helper::recordResult("DMA Protection", "OK", "Disabled");
            return;
        }
        else {
            Helper::printError("- Kernel DMA Protection is enabled");
            Helper::recordResult("DMA Protection", "FAIL", "Enabled");
            return;
        }
    }
    DWORD dmaState = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "KernelDmaProtectionState", &dmaState)) {
        if (dmaState == 0) {
            Helper::printSuccess("- Kernel DMA Protection is disabled", false);
            Helper::recordResult("DMA Protection", "OK", "Disabled");
        }
        else {
            Helper::printError("- Kernel DMA Protection is enabled (" + std::to_string(dmaState) + ")");
            Helper::recordResult("DMA Protection", "FAIL", "State=" + std::to_string(dmaState));
        }
        return;
    }
    Helper::printSuccess("- Kernel DMA Protection is not active", false);
    Helper::recordResult("DMA Protection", "OK", "Not supported");
}

void Checks::checkUAC()
{
    SetConsoleTitleA("Checking UAC");
    DWORD uacEnabled = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        "EnableLUA", &uacEnabled)) {
        if (uacEnabled == 0) {
            Helper::printSuccess("- UAC notifications are disabled", false);
            Helper::recordResult("UAC", "OK", "Disabled");
        }
        else {
            Helper::printConcern("- UAC is enabled, consider disabling UAC");
            Helper::recordResult("UAC", "WARN", "Enabled - consider disabling");
        }
    }
    else {
        Helper::printConcern("- Could not read UAC status");
        Helper::recordResult("UAC", "WARN", "Could not read registry");
    }
}

void Checks::checkWindowsUpdate()
{
    SetConsoleTitleA("Checking Windows Update Service");

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        Helper::printConcern("- Could not check Windows Update service status");
        Helper::recordResult("Windows Update", "WARN", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "wuauserv", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        Helper::printConcern("- Windows Update service not found");
        Helper::recordResult("Windows Update", "WARN", "Service not found");
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printConcern("- Could not query Windows Update service status");
        Helper::recordResult("Windows Update", "WARN", "Query failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (status.dwCurrentState == SERVICE_RUNNING) {
        Helper::printConcern("- Windows Update service is running (recommend disabling after installing updates)");
        Helper::recordResult("Windows Update", "WARN", "Running - recommend disabling");
    }
    else {
        Helper::printSuccess("- Windows Update is disabled", false);
        Helper::recordResult("Windows Update", "OK", "Disabled/stopped");
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

void Helper::setupConsole()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS));
    SetConsoleTitleA("Initializing");
}

void Helper::printSuccess(const std::string& message, bool changed)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Green);
    std::cout << "[+] ";
    Color::setForegroundColor(Color::White);
    std::cout << message;
    if (changed) { Color::setForegroundColor(Color::Yellow); std::cout << " (CHANGED)"; }
    std::cout << std::endl;
}

void Helper::printConcern(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Yellow);
    std::cout << "[-] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

void Helper::printError(const std::string& message)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Red);
    std::cout << "[X] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

void Helper::runSystemCommand(const char* command)
{
    std::string modifiedCommand = command;
    if (modifiedCommand.find("2>nul") == std::string::npos &&
        modifiedCommand.find("2>&1") == std::string::npos) {
        modifiedCommand += " 2>nul";
    }
    FILE* stream = _popen(modifiedCommand.c_str(), "r");
    if (stream == NULL) return;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), stream) != NULL) {}
    _pclose(stream);
}

bool Helper::readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData) {
    HKEY hKey;
    LONG ret = RegOpenKeyEx(hKeyParent, subkey, 0, KEY_READ, &hKey);
    if (ret == ERROR_SUCCESS) {
        DWORD data;
        DWORD len = sizeof(DWORD);
        ret = RegQueryValueEx(hKey, valueName, NULL, NULL, reinterpret_cast<LPBYTE>(&data), &len);
        RegCloseKey(hKey);
        if (ret == ERROR_SUCCESS) {
            (*readData) = data;
            return true;
        }
        return false;
    }
    return false;
}

void Color::setBackgroundColor(const RGBColor& color) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode);
    printf(("\x1b[48;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b) + "m").c_str());
}

void Color::setForegroundColor(const RGBColor& color) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, dwMode);
    printf(("\x1b[38;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b) + "m").c_str());
}

