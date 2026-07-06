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

    int choice = MessageBoxA(GetConsoleWindow(),
        "The checker found changes that may be needed.\n\nApply these changes now?",
        "Apply setup changes?",
        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (choice == IDYES) return true;

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
    HINTERNET hSession = WinHttpOpen(L"FBI-Setup/2.0",
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
    std::cout << "  --skip N[,M,...]    Skip specific checks by number\n";
    std::cout << "  --only N[,M,...]    Run only specific checks by number\n\n";
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
        else if (arg == "--headless") { config.headless = true; }
        else if (arg == "--quiet") { config.quiet = true; }
        else if (arg == "--apply") { config.autoApply = true; }
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
    std::wstring dir = std::wstring(tempPath) + L"FBI-Setup";
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

bool Helper::readDwordValueRegistry(HKEY hKeyParent, LPCSTR subkey, LPCSTR valueName, DWORD* readData)
{
    HKEY hKey;
    if (RegOpenKeyEx(hKeyParent, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD dataSize = sizeof(DWORD);
    DWORD type;
    LONG result = RegQueryValueEx(hKey, valueName, NULL, &type, (LPBYTE)readData, &dataSize);
    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS && type == REG_DWORD);
}

ServiceStatus Helper::getServiceStatus(LPCSTR serviceName)
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) return STATUS_SERVICE_STOPPED;

    SC_HANDLE service = OpenService(scm, serviceName, SERVICE_QUERY_STATUS);
    if (service == NULL) {
        CloseServiceHandle(scm);
        return STATUS_SERVICE_STOPPED;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    ServiceStatus result = STATUS_SERVICE_STOPPED;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        switch (status.dwCurrentState) {
        case SERVICE_STOPPED: result = STATUS_SERVICE_STOPPED; break;
        case SERVICE_START_PENDING: result = STATUS_SERVICE_START_PENDING; break;
        case SERVICE_STOP_PENDING: result = STATUS_SERVICE_STOP_PENDING; break;
        case SERVICE_RUNNING: result = STATUS_SERVICE_RUNNING; break;
        case SERVICE_CONTINUE_PENDING: result = STATUS_SERVICE_CONTINUE_PENDING; break;
        case SERVICE_PAUSE_PENDING: result = STATUS_SERVICE_PAUSE_PENDING; break;
        case SERVICE_PAUSED: result = STATUS_SERVICE_PAUSED; break;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return result;
}

void Helper::setupConsole()
{
    SetConsoleTitleA("FBI-Setup");
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hConsole, &mode)) {
        SetConsoleMode(hConsole, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

void Helper::runSystemCommand(const char* command)
{
    logWrite("  CMD: " + std::string(command));
    std::thread([command]() {
        std::string cmd = command ? command : "";
        cmd += " >nul 2>&1";
        std::system(cmd.c_str());
    }).detach();
}

static HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

void Helper::printSuccess(const std::string& message, bool changed)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    if (changed) Color::setForegroundColor(Color::Green);
    else Color::setForegroundColor(Color::Green);
    std::cout << "[+] " << message << "\n";
    Color::setForegroundColor(Color::LightGray);
}

void Helper::printConcern(const std::string& message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Yellow);
    std::cout << "[!] " << message << "\n";
    Color::setForegroundColor(Color::LightGray);
}

void Helper::printError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    Color::setForegroundColor(Color::Red);
    std::cout << "[-] " << message << "\n";
    Color::setForegroundColor(Color::LightGray);
}

static void SetConsoleColor(const RGBColor& color, bool isBackground);

void Color::setBackgroundColor(const RGBColor& aColor)
{
    SetConsoleColor(aColor, true);
}

void Color::setForegroundColor(const RGBColor& aColor)
{
    SetConsoleColor(aColor, false);
}

static void SetConsoleColor(const RGBColor& color, bool isBackground)
{
    HANDLE h = GetStdHandle(isBackground ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, 7);
    SetConsoleTextAttribute(h, 0);
    std::string seq = "\x1b[" + std::to_string(isBackground ? 48 : 38) + ";2;" +
        std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b) + "m";
    DWORD written;
    WriteConsoleA(h, seq.c_str(), seq.size(), &written, NULL);
}

void Checks::createRestorePoint()
{
    HMODULE hSr = LoadLibraryA("srclient.dll");
    if (!hSr) {
        Helper::printConcern("- Could not load srclient.dll for restore point");
        Helper::recordResult("Restore Point", "WARN", "srclient.dll not found");
        return;
    }

    typedef BOOL(WINAPI* SRSetRestorePointWPtr)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
    SRSetRestorePointWPtr pSR = (SRSetRestorePointWPtr)GetProcAddress(hSr, "SRSetRestorePointW");
    if (!pSR) {
        FreeLibrary(hSr);
        Helper::printConcern("- Could not find SRSetRestorePointW export");
        Helper::recordResult("Restore Point", "WARN", "Export not found");
        return;
    }

    RESTOREPOINTINFOW rpi;
    rpi.dwEventType = BEGIN_SYSTEM_CHANGE;
    rpi.dwRestorePtType = MODIFY_SETTINGS;
    rpi.llSequenceNumber = 0;
    std::wstring desc = L"FBI-Setup v" + std::wstring(Helper::g_appVersion.begin(), Helper::g_appVersion.end());
    wcsncpy_s(rpi.szDescription, MAX_PATH, desc.c_str(), _TRUNCATE);
    STATEMGRSTATUS sms;
    if (pSR(&rpi, &sms)) {
        Helper::printSuccess("- System Restore Point created", true);
        Helper::recordResult("Restore Point", "OK", "Created");
    } else {
        Helper::printConcern("- Failed to create System Restore Point (nStatus=" + std::to_string(sms.nStatus) + ")");
        Helper::recordResult("Restore Point", "WARN", "Failed: " + std::to_string(sms.nStatus));
    }
    FreeLibrary(hSr);
}

void Checks::backupRegistryKey(const std::string& key, const std::string& label)
{
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring backupDir = std::wstring(tempPath) + L"FBI-Setup\\backups\\";
    CreateDirectoryW(backupDir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d_%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    std::string filename = backupDir.begin() != backupDir.end()
        ? std::string(backupDir.begin(), backupDir.end()) + label + "_" + timestamp + ".reg"
        : std::string("C:\\Windows\\Temp\\FBI-Setup\\backups\\") + label + "_" + timestamp + ".reg";

    // Convert to ANSI path for reg.exe
    std::string cmd = "reg export \"" + key + "\" \"" + filename + "\" /y >nul 2>&1";
    std::system(cmd.c_str());

    if (std::filesystem::exists(filename)) {
        Helper::printSuccess("- Registry backup saved: " + label, true);
        Helper::recordResult("Registry Backup", "OK", label + " backed up");
    } else {
        Helper::printConcern("- Registry backup may have failed: " + label);
        Helper::recordResult("Registry Backup", "WARN", label + " backup attempted");
    }
}

void Checks::checkWindowsDefender()
{
    SetConsoleTitleA("Checking Windows Defender");

    if (checkDefenderTamperProtection()) {
        Helper::printError("- Windows Defender cannot be disabled (Tamper Protection prevents changes)");
        Helper::recordResult("Windows Defender", "FAIL", "Tamper protection enabled");
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

void Checks::check3rdPartyAntiVirus()
{
    SetConsoleTitleA("Checking for 3rd Party Anti-Viruses");

    auto runWMIC = [](const std::string& cmd) -> std::string {
        std::FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) {
            if (!Helper::requestChange("Enable WMIC optional feature for antivirus detection")) {
                return std::string("ERROR");
            }
            Helper::runSystemCommand("dism /online /enable-feature /featurename:WMIC /quiet /norestart 2>nul");
            Sleep(500);
            pipe = _popen(cmd.c_str(), "r");
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
                        size_t start = 0;
                        while (start != std::string::npos) {
                            size_t comma = av.find(",", start);
                            std::string single = av.substr(start, comma == std::string::npos ? comma : comma - start);
                            if (!single.empty() && single[0] == ' ') single = single.substr(1);
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
                            start = comma == std::string::npos ? comma : comma + 1;
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
        std::FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) {
            if (!Helper::requestChange("Enable WMIC optional feature for CPU virtualization detection")) {
                return std::string("ERROR");
            }
            Helper::runSystemCommand("dism /online /enable-feature /featurename:WMIC /quiet /norestart 2>nul");
            Sleep(500);
            pipe = _popen(cmd.c_str(), "r");
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

    if (result.find("True") != std::string::npos) {
        Helper::printError("- CPU-V is enabled in BIOS, please disable in BIOS");
        Helper::recordResult("CPU-V", "FAIL", "Enabled in BIOS");
        return;
    }

    Helper::recordResult("CPU-V", "OK", "Disabled");
}

void Checks::checkRiotVanguard()
{
    SetConsoleTitleA("Checking for Riot Vanguard");

    HKEY hKey;
    LONG result = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        Helper::printError("- Failed to check if Riot Vanguard is installed (Error 6, GLE=" + std::to_string(GetLastError()) + ")");
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
            if (!Helper::requestChange("Uninstall Riot Vanguard")) {
                Helper::recordResult("Riot Vanguard", "WARN", "Installed - uninstall pending approval");
                return;
            }
            uninstallRiotVanguard();
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

void Checks::uninstallRiotVanguard()
{
    if (std::filesystem::exists("C:\\Program Files\\Riot Vanguard\\installer.exe") ||
        std::filesystem::exists("C:\\Program Files (x86)\\Riot Vanguard\\installer.exe")) {
        if (!Helper::cliConfig.headless) {
            MessageBoxA(GetConsoleWindow(), "When prompted to uninstall Vanguard, press YES", "Uninstall Vanguard", MB_ICONINFORMATION | MB_TOPMOST);
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
}

void Checks::checkVCRedist()
{
    SetConsoleTitleA("Checking VCRedist");

    bool alreadyInstalled = (std::filesystem::exists("C:\\Windows\\System32\\vcruntime140.dll") ||
        std::filesystem::exists("C:\\Windows\\SysWOW64\\vcruntime140.dll")) &&
        (std::filesystem::exists("C:\\Windows\\System32\\msvcp140.dll") ||
            std::filesystem::exists("C:\\Windows\\SysWOW64\\msvcp140.dll"));
    if (alreadyInstalled) {
        Helper::printSuccess("- VCRedist is already installed", false);
        Helper::recordResult("VC Redist", "OK", "Already installed");
        return;
    }

    if (!Helper::requestChange("Download and install Microsoft Visual C++ Redistributable")) {
        Helper::printConcern("- VCRedist is missing");
        Helper::recordResult("VC Redist", "WARN", "Missing - install pending approval");
        return;
    }

    installVCRedist();
}

void Checks::installVCRedist()
{
    SetConsoleTitleA("Downloading VCRedist");

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
        return;
    }
    if (downloadX86 != S_OK || !std::filesystem::exists("C:\\Windows\\VC_redist.x86.exe")) {
        Helper::printError("- Failed to download VCRedist x86 (Error 10, HR=" + std::to_string(downloadX86) + ")");
        Helper::recordResult("VC Redist", "FAIL", "Download x86 failed");
        Sleep(1000);
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x64.exe");
        Helper::runSystemCommand("start https://aka.ms/vs/17/release/vc_redist.x86.exe");
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
        return;
    }

    Helper::printSuccess("- VCRedist is installed", false);
    Helper::recordResult("VC Redist", "OK", "Installed successfully");
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
    Helper::printConcern("- Google Chrome is not installed (needed to disable Chrome protection)");
    Helper::recordResult("Google Chrome", "WARN", "Not installed");
    Sleep(1000);
    Helper::runSystemCommand("start https://www.google.com/chrome/");
}

void Checks::syncWindowsTime()
{
    SetConsoleTitleA("Syncing Windows Time");

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        Helper::printError("- Failed to check Time Sync (Error 14)");
        Helper::recordResult("Time Sync", "FAIL", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "W32Time", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            Helper::printConcern("- Windows Time service not found");
            Helper::recordResult("Time Sync", "FAIL", "Service not found");
        } else {
            Helper::printError("- Failed to check Time Sync (Error 15)");
            Helper::recordResult("Time Sync", "FAIL", "OpenService failed");
        }
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printError("- Failed to check Time Sync (Error 16)");
        Helper::recordResult("Time Sync", "FAIL", "QueryService failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (status.dwCurrentState == SERVICE_RUNNING) {
        Helper::printSuccess("- Windows Time is running", false);
        Helper::recordResult("Time Sync", "OK", "Running");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    if (!Helper::requestChange("Repair and start Windows Time service")) {
        Helper::recordResult("Time Sync", "WARN", "Disabled - repair pending approval");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    CloseServiceHandle(service);
    service = OpenService(scm, "W32Time", SERVICE_ALL_ACCESS);
    if (service != NULL) {
        ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);

    Helper::runSystemCommand("net start W32Time");
    Helper::runSystemCommand("w32tm /config /syncfromflags:manual /manualpeerlist:pool.ntp.org");
    Helper::runSystemCommand("w32tm /resync");

    Helper::printSuccess("- Windows Time service configured and started", true);
    Helper::recordResult("Time Sync", "OK", "Repaired and started");
}

void Checks::disableChromeProtection()
{
    SetConsoleTitleA("Disabling Chrome Protection");

    DWORD currentValue;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Google\\Chrome", "SafeBrowsingProtectionLevel", &currentValue)) {
        if (currentValue == 0x00000001) {
            Helper::printSuccess("- Chrome protection is already disabled", false);
            Helper::recordResult("Chrome Protection", "OK", "Already disabled");
            return;
        }
    }

    if (!Helper::requestChange("Disable Chrome protection (Safe Browsing policy)")) {
        Helper::recordResult("Chrome Protection", "WARN", "Needs disable - pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0x00000001;
    LONG createKey = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    if (createKey == ERROR_SUCCESS) {
        LONG cd = RegSetValueEx(hKey, "SafeBrowsingProtectionLevel", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        if (cd == ERROR_SUCCESS) {
            Helper::printSuccess("- Disabled Chrome protection", true);
            Helper::recordResult("Chrome Protection", "OK", "Disabled (changed)");
            return;
        }
    }
    Helper::printError("- Failed to disable Chrome protection (Error 20/21)");
    Helper::recordResult("Chrome Protection", "FAIL", "Registry write failed");
}

void Checks::checkWinver()
{
    SetConsoleTitleA("Checking Winver");
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Read failed");
        return;
    }

    char value[64]; DWORD size = sizeof(value);
    LONG result = RegQueryValueEx(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)value, &size);
    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 23)");
        Helper::recordResult("Winver", "FAIL", "Read CurrentBuild failed");
        return;
    }

    int build = 0;
    try { build = std::stoi(std::string(value, size - 1)); }
    catch (...) {
        Helper::printError("- Failed to check Winver (Error 24)");
        Helper::recordResult("Winver", "FAIL", "Parse failed");
        return;
    }

    if (build < 22000) {
        Helper::printError("- Winver (build " + std::to_string(build) + ") is not supported. Minimum: 22000 (Win11)");
        Helper::recordResult("Winver", "FAIL", "Build " + std::to_string(build) + " unsupported");
    } else {
        Helper::printSuccess("- Winver (build " + std::to_string(build) + ") is supported", false);
        Helper::recordResult("Winver", "OK", "Build " + std::to_string(build));
    }
}

void Checks::deleteSymbols()
{
    SetConsoleTitleA("Checking Symbols");

    if (!std::filesystem::exists("C:\\Symbols")) {
        Helper::printSuccess("- C:\\Symbols does not exist", false);
        Helper::recordResult("Symbols", "OK", "Not present");
        return;
    }

    if (!Helper::requestChange("Delete C:\\Symbols directory")) {
        Helper::recordResult("Symbols", "WARN", "Exists - delete pending approval");
        return;
    }

    try {
        std::filesystem::remove_all("C:\\Symbols");
        if (!std::filesystem::exists("C:\\Symbols")) {
            Helper::printSuccess("- Deleted C:\\Symbols", true);
            Helper::recordResult("Symbols", "OK", "Deleted");
        } else {
            Helper::printError("- Failed to delete C:\\Symbols (Error 25)");
            Helper::recordResult("Symbols", "FAIL", "Delete failed");
        }
    } catch (...) {
        Helper::printError("- Failed to delete C:\\Symbols (Error 26)");
        Helper::recordResult("Symbols", "FAIL", "Exception during delete");
    }
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
    if (!Helper::requestChange("Disable Fast-Boot")) {
        Helper::recordResult("Fast Boot", "WARN", "Needs disable - pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    if (ck == ERROR_SUCCESS) {
        LONG cd = RegSetValueEx(hKey, "HiberbootEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        if (cd == ERROR_SUCCESS) {
            Helper::printSuccess("- Disabled Fast-Boot", true);
            Helper::restartRequired = true;
            Helper::recordResult("Fast Boot", "OK", "Disabled (changed)");
            return;
        }
    }
    Helper::printError("- Failed to disable Fast-Boot (Error 31/32)");
    Helper::recordResult("Fast Boot", "FAIL", "Registry write failed");
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
    if (!Helper::requestChange("Disable Exploit-Protection")) {
        Helper::recordResult("Exploit Protection", "WARN", "Needs disable - pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    if (ck == ERROR_SUCCESS) {
        LONG cd = RegSetValueEx(hKey, "DisallowExploitProtectionOverride", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        if (cd == ERROR_SUCCESS) {
            Helper::printSuccess("- Disabled Exploit-Protection", true);
            Helper::restartRequired = true;
            Helper::recordResult("Exploit Protection", "OK", "Disabled (changed)");
            return;
        }
    }
    Helper::printError("- Failed to disable Exploit-Protection (Error 25/26)");
    Helper::recordResult("Exploit Protection", "FAIL", "Registry write failed");
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
        Helper::recordResult("SmartScreen", "WARN", "Needs disable - pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    if (ck == ERROR_SUCCESS) {
        LONG cd = RegSetValueEx(hKey, "EnableSmartScreen", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        if (cd == ERROR_SUCCESS) {
            Helper::printSuccess("- Disabled SmartScreen", true);
            Helper::restartRequired = true;
            Helper::recordResult("SmartScreen", "OK", "Disabled (changed)");
            return;
        }
    }
    Helper::printError("- Failed to disable SmartScreen (Error 27/28)");
    Helper::recordResult("SmartScreen", "FAIL", "Registry write failed");
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
    if (!Helper::requestChange("Enable Gamebar")) {
        Helper::recordResult("Game Bar", "WARN", "Needs enable - pending approval");
        return;
    }

    HKEY hKey; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    if (ck == ERROR_SUCCESS) {
        LONG cd = RegSetValueEx(hKey, "AppCaptureEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        if (cd == ERROR_SUCCESS) {
            Helper::printSuccess("- Enabled Gamebar", true);
            Helper::restartRequired = true;
            Helper::recordResult("Game Bar", "OK", "Enabled (changed)");
            return;
        }
    }
    Helper::printError("- Failed to enable Gamebar (Error 29/30)");
    Helper::recordResult("Game Bar", "FAIL", "Registry write failed");
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
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char val[64] = ""; DWORD sz = sizeof(val);
        if (RegQueryValueEx(hKey, "ConsentPromptBehaviorAdmin", NULL, NULL, (LPBYTE)val, &sz) == ERROR_SUCCESS) {
            if (val[0] == 0) { modified = true; reasons += " UAC disabled;"; }
        }
        RegCloseKey(hKey);
    }

    if (modified) {
        Helper::printConcern("- OS appears to be modified:" + reasons);
        Helper::recordResult("Modified OS", "WARN", reasons);
    } else {
        Helper::printSuccess("- OS appears unmodified", false);
        Helper::recordResult("Modified OS", "OK", "Clean");
    }
}

void Checks::checkTPMStatus()
{
    SetConsoleTitleA("Checking TPM Status");
    std::string wmicResult = Helper::runSystemCommandWithOutput(
        "wmic /namespace:\\\\root\\cimv2\\security\\microsofttpm path win32_tpm get IsActivated_InitialValue /format:value",
        10000);
    if (wmicResult.find("IsActivated_InitialValue=1") != std::string::npos) {
        Helper::printConcern("- TPM is activated");
        Helper::recordResult("TPM", "WARN", "Activated");
    } else if (wmicResult.find("IsActivated_InitialValue=0") != std::string::npos) {
        Helper::printSuccess("- TPM is not activated", false);
        Helper::recordResult("TPM", "OK", "Not activated");
    } else {
        Helper::printSuccess("- TPM not found", false);
        Helper::recordResult("TPM", "OK", "Not present");
    }
}

void Checks::checkCoreIsolation()
{
    SetConsoleTitleA("Checking Core Isolation");
    DWORD hypervisorEnabled = 0;
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard", "EnableVirtualizationBasedSecurity", &hypervisorEnabled);

    if (hypervisorEnabled == 0) {
        Helper::printSuccess("- Core Isolation (VBS) is disabled", false);
        Helper::recordResult("Core Isolation", "OK", "Disabled");
        return;
    }

    if (!Helper::requestChange("Disable Core Isolation (VBS)")) {
        Helper::recordResult("Core Isolation", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey;
    DWORD val = 0;
    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "EnableVirtualizationBasedSecurity", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "Enabled", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);

    Helper::restartRequired = true;
    Helper::printSuccess("- Core Isolation disabled (restart required)", true);
    Helper::recordResult("Core Isolation", "OK", "Disabled (changed)");
}

void Checks::checkDMAProtection()
{
    SetConsoleTitleA("Checking DMA Protection");
    DWORD dmaStatus = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Kernel DMA Protection", "Device Protection Supported", &dmaStatus)) {
        if (dmaStatus > 0) {
            Helper::printConcern("- Kernel DMA Protection is active");
            Helper::recordResult("DMA Protection", "WARN", "Active");
        } else {
            Helper::printSuccess("- Kernel DMA Protection is not active", false);
            Helper::recordResult("DMA Protection", "OK", "Not active");
        }
    } else {
        Helper::printSuccess("- Kernel DMA Protection not found", false);
        Helper::recordResult("DMA Protection", "OK", "Not present");
    }
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
            Helper::printConcern("- Adapter: " + line);
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
        } else {
            Helper::runSystemCommand("bcdedit /set hypervisorlaunchtype off");
            Helper::printSuccess("- Set hypervisor launch type to off", true);
            appendSummary("hypervisorlaunchtype=changed to off");
            changed = true;
        }
    }

    auto checkAndDisableFeature = [&](const std::string& featureName) {
        std::string queryCommand = "dism /online /get-featureinfo /featurename:" + featureName;
        std::string output = lower(Helper::runSystemCommandWithOutput(queryCommand.c_str(), 15000));
        if (output == "timeout" || output.empty()) {
            Helper::printConcern("- Could not read Windows feature status: " + featureName);
            appendSummary(featureName + "=unknown");
            warning = true;
            return;
        }

        if (output.find("state : disabled") != std::string::npos ||
            output.find("state: disabled") != std::string::npos) {
            Helper::printSuccess("- Windows feature is already disabled: " + featureName, false);
            appendSummary(featureName + "=disabled");
            return;
        }

        if (output.find("state : enabled") == std::string::npos &&
            output.find("state: enabled") == std::string::npos) {
            Helper::printConcern("- Windows feature status is unclear: " + featureName);
            appendSummary(featureName + "=unclear");
            warning = true;
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

    auto getUpdateCachePath = []() -> std::string {
        char localAppData[MAX_PATH] = "";
        std::string basePath;
        if (GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
            basePath = localAppData;
        }
        else {
            char tempPath[MAX_PATH] = "";
            GetTempPathA(MAX_PATH, tempPath);
            basePath = tempPath;
        }

        std::string dir = basePath + "\\FBI-Setup";
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

    auto readCachedTag = [](const std::string& path) -> std::string {
        std::ifstream file(path);
        std::string tag;
        if (file.is_open()) {
            std::getline(file, tag);
        }
        return tag;
    };

    auto writeCachedTag = [](const std::string& path, const std::string& tag) {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (file.is_open()) {
            file << tag << "\n";
        }
    };

    std::string cachePath = getUpdateCachePath();
    std::string tag = readCachedTag(cachePath);
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
            writeCachedTag(cachePath, tag);
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
        Helper::recordResult("Auto-Update", "OK",
            "Up to date (v" + Helper::g_appVersion + ")" + (usedCache ? " - checked this week" : ""));
    }
}

void Checks::checkUAC()
{
    SetConsoleTitleA("Checking UAC");
    DWORD enableLUA = 1, consentPrompt = 2;

    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", "EnableLUA", &enableLUA);
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", "ConsentPromptBehaviorAdmin", &consentPrompt);

    if (enableLUA == 0) {
        Helper::printSuccess("- UAC (EnableLUA) is already disabled", false);
        Helper::recordResult("UAC", "OK", "Disabled");
        return;
    }

    if (enableLUA == 1 && consentPrompt == 0) {
        Helper::printSuccess("- UAC notifications are disabled", false);
        Helper::recordResult("UAC", "OK", "Notifications disabled");
        return;
    }

    if (!Helper::requestChange("Disable UAC (EnableLUA=0, ConsentPromptBehaviorAdmin=0)")) {
        Helper::recordResult("UAC", "WARN", "Enabled - disable pending approval");
        return;
    }

    HKEY hKey;
    DWORD val0 = 0, val1 = 1;
    RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    RegSetValueEx(hKey, "ConsentPromptBehaviorAdmin", 0, REG_DWORD, (BYTE*)&val0, sizeof(val0));
    RegSetValueEx(hKey, "PromptOnSecureDesktop", 0, REG_DWORD, (BYTE*)&val0, sizeof(val0));
    RegSetValueEx(hKey, "EnableLUA", 0, REG_DWORD, (BYTE*)&val0, sizeof(val0));
    RegCloseKey(hKey);

    Helper::restartRequired = true;
    Helper::printSuccess("- UAC disabled (restart required)", true);
    Helper::recordResult("UAC", "OK", "Disabled (changed)");
}

void Checks::checkWindowsUpdate()
{
    SetConsoleTitleA("Checking Windows Update");

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        Helper::recordResult("Windows Update", "FAIL", "SCM open failed");
        return;
    }

    SC_HANDLE service = OpenService(scm, "wuauserv", SERVICE_QUERY_STATUS);
    if (service == NULL) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            Helper::printSuccess("- Windows Update service not found (already removed)", false);
            Helper::recordResult("Windows Update", "OK", "Service removed");
        } else {
            Helper::printError("- Failed to check Windows Update (Error 36)");
            Helper::recordResult("Windows Update", "FAIL", "OpenService failed");
        }
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS_PROCESS status;
    DWORD bytesNeeded;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
        Helper::printError("- Failed to check Windows Update (Error 36)");
        Helper::recordResult("Windows Update", "FAIL", "QueryService failed");
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return;
    }

    DWORD noAutoUpdate = 0;
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU", "NoAutoUpdate", &noAutoUpdate);

    bool serviceOk = (status.dwCurrentState != SERVICE_RUNNING && status.dwCurrentState != SERVICE_START_PENDING);
    bool policyOk = (noAutoUpdate == 1);

    if (serviceOk && policyOk) {
        Helper::printSuccess("- Windows Update is disabled", false);
        Helper::recordResult("Windows Update", "OK", "Disabled (service + policy)");
    }
    else if (!serviceOk && !policyOk) {
        if (!Helper::requestChange("Disable Windows Update (service + policy)")) {
            Helper::recordResult("Windows Update", "WARN", "Enabled - disable pending approval");
        } else {
            // Stop + disable service
            SERVICE_STATUS ss;
            ControlService(service, SERVICE_CONTROL_STOP, &ss);
            ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
            HKEY hKey;
            DWORD val = 1;
            RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU",
                0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
            RegSetValueEx(hKey, "NoAutoUpdate", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
            RegCloseKey(hKey);
            Helper::restartRequired = true;
            Helper::printSuccess("- Windows Update disabled (restart may be needed)", true);
            Helper::recordResult("Windows Update", "OK", "Disabled (changed)");
        }
    }
    else {
        // Mixed state
        if (!Helper::requestChange("Disable Windows Update (mixed state)")) {
            Helper::recordResult("Windows Update", "WARN", "Mixed state - disable pending approval");
        } else {
            SERVICE_STATUS ss;
            ControlService(service, SERVICE_CONTROL_STOP, &ss);
            ChangeServiceConfig(service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
            HKEY hKey;
            DWORD val = 1;
            RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU",
                0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
            RegSetValueEx(hKey, "NoAutoUpdate", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
            RegCloseKey(hKey);
            Helper::restartRequired = true;
            Helper::printSuccess("- Windows Update fully disabled (restart may be needed)", true);
            Helper::recordResult("Windows Update", "OK", "Disabled (changed)");
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
}

void Checks::checkTimeSync()
{
    syncWindowsTime();
}

void Checks::checkSymbols()
{
    deleteSymbols();
}

