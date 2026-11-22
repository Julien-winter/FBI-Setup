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
    // v1.2.2: every console line is printed at most once per run - the apply
    // phase re-runs checks and must not repeat unchanged status lines
    std::unordered_set<std::string> g_printedLines;
    std::string g_repoUrl = "Julien-winter/FBI-Setup";
    std::string g_appName = "FBI-Setup";
    std::string g_appVersion = "1.2.1";
    int g_exitCode = 0;
}

namespace {
    std::string toLowerCopy(const std::string& value)
    {
        std::string result = value;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return result;
    }

    std::string trimCopy(const std::string& value)
    {
        const char* whitespace = " \t\r\n";
        size_t start = value.find_first_not_of(whitespace);
        if (start == std::string::npos) return "";
        size_t end = value.find_last_not_of(whitespace);
        return value.substr(start, end - start + 1);
    }

    // "5,6,7,8" style CSV without matching 3 inside 13/23/30
    bool csvContains(const std::string& csv, int value)
    {
        std::string token;
        for (size_t i = 0; i <= csv.size(); i++) {
            if (i == csv.size() || csv[i] == ',') {
                std::string trimmed = trimCopy(token);
                if (!trimmed.empty() && atoi(trimmed.c_str()) == value) return true;
                token.clear();
            }
            else {
                token += csv[i];
            }
        }
        return false;
    }

    std::string readRegistryStringValue(HKEY root, const char* subkey, const char* valueName)
    {
        HKEY hKey;
        if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
        char buffer[512] = { 0 };
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        std::string result;
        if (RegQueryValueExA(hKey, valueName, NULL, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            result.assign(buffer);
        }
        RegCloseKey(hKey);
        return result;
    }

    bool startsWithLower(const std::string& lowerValue, const char* lowerPrefix)
    {
        std::string prefix = lowerPrefix;
        return lowerValue.size() >= prefix.size() &&
            lowerValue.compare(0, prefix.size(), prefix) == 0;
    }

    // v1.2.2: a device instance on one of these buses is real hardware and is
    // NEVER a removal candidate (ROOT\, SWD\, VMBUS\, TAP\, BTH\... are not listed)
    bool isHardwareBusInstance(const std::string& instanceId)
    {
        if (instanceId.empty()) return false;
        static const char* hardwarePrefixes[] = {
            "pci\\", "usb\\", "pcmcia\\", "1394\\", "scsi\\", "sas\\", "nvme\\",
            "ide\\", "fibre\\", "mmc\\", "ufs\\", "gpe\\", "acpi\\"
        };
        std::string lower = toLowerCopy(instanceId);
        for (const char* prefix : hardwarePrefixes) {
            if (startsWithLower(lower, prefix)) return true;
        }
        return false;
    }

    // Only loopback / real virtual devices are ever candidates for removal - never a physical NIC.
    // v1.2.2 rework: classify by (a) friendly-name markers, (b) the PnP device-instance bus.
    // Unknown instance -> treated as hardware (conservative: never remove what we cannot prove).
    bool isVirtualAdapter(const std::string& friendlyName, const IP_ADAPTER_ADDRESSES* adapter,
                          const std::string& instanceId, std::string* reason)
    {
        if (adapter == NULL) return false;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) return false;

        std::string name = toLowerCopy(friendlyName);

        // (a) name markers win over the bus check - VMware/VBox sit on PCI\ but are virtual
        static const char* virtualMarkers[] = {
            "vethernet", "hyper-v", "vmware", "virtualbox", "vbox", "bluetooth", "wan miniport",
            "km-test", "microsoft wi-fi direct", "microsoft loopback", "isatap", "teredo",
            "6to4", "vpn", "tap", "tun", "wireguard", "nordlynx", "proton", "windscribe", "mullvad",
            "expressvpn", "openvpn", "softether", "anyconnect", "hamachi", "zerotier", "npcap",
            "wsl", "docker", "network bridge", "layered", "pan ", "virtual",
            // v1.2.2: leftovers that stayed behind in v1.2.1
            "lan-verbindung*", "tailscale", "wintun"
        };
        for (const char* marker : virtualMarkers) {
            if (name.find(marker) != std::string::npos) {
                if (reason != NULL) *reason = std::string("name marker '") + marker + "'";
                return true;
            }
        }

        if (adapter->IfType == IF_TYPE_PPP || adapter->IfType == IF_TYPE_SLIP ||
            adapter->IfType == IF_TYPE_TUNNEL) {
            if (reason != NULL) *reason = "software link (PPP/SLIP/tunnel)";
            return true;
        }

        // (b) bus classification via the PnP device instance
        if (instanceId.empty()) {
            return false; // unknown device -> conservative, keep it
        }
        if (isHardwareBusInstance(instanceId)) {
            return false;
        }
        if (reason != NULL) {
            *reason = "virtual bus device (" + instanceId + ")";
        }
        return true;
    }

    // v1.2.2: mainboard vendor from the firmware registry block (SMBIOS mirror)
    std::string mainboardVendor()
    {
        std::string vendor = readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardManufacturer");
        if (vendor.empty()) {
            vendor = readRegistryStringValue(HKEY_LOCAL_MACHINE,
                "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemManufacturer");
        }
        std::string v = toLowerCopy(trimCopy(vendor));
        if (v.empty()) return "";
        if (v.find("asus") != std::string::npos) return "ASUS";
        if (v.find("micro-star") != std::string::npos || v == "msi") return "MSI";
        if (v.find("gigabyte") != std::string::npos) return "Gigabyte";
        if (v.find("asrock") != std::string::npos) return "ASRock";
        if (v.find("alienware") != std::string::npos || v.find("dell") != std::string::npos) return "Dell";
        if (v.find("hewlett") != std::string::npos || v == "hp" || v.find("compaq") != std::string::npos) return "HP";
        if (v.find("lenovo") != std::string::npos || v.find("ibm") != std::string::npos) return "Lenovo";
        if (v.find("acer") != std::string::npos) return "Acer";
        if (v.find("medion") != std::string::npos) return "Medion";
        if (v.find("toshiba") != std::string::npos) return "Toshiba";
        if (v.find("supermicro") != std::string::npos) return "Supermicro";
        if (v.find("evga") != std::string::npos) return "EVGA";
        return "";
    }

    // v1.2.2: SHORT BIOS path for the detected board, appended in the SAME line
    // topic: "tpm" | "secureboot" | "cpuv"
    // amdSvm: on AMD boards the CPU-V toggle is called "SVM Mode" (v1.2.3)
    std::string biosHint(const char* topic, bool amdSvm = false)
    {
        const std::string t = topic;
        const std::string vendor = mainboardVendor();
        std::string path;

        if (t == "tpm") {
            if (vendor == "ASUS") path = "Advanced > fTPM Config > Off";
            else if (vendor == "MSI") path = "Security > fTPM/PTT > Off";
            else if (vendor == "Gigabyte") path = "Settings > fTPM > Off";
            else if (vendor == "ASRock") path = "Advanced > CPU Config > fTPM";
            else if (vendor == "Dell" || vendor == "HP" || vendor == "Lenovo")
                path = "Security > TPM > Off";
            else path = "BIOS > Security > TPM > Off";
        }
        else if (t == "secureboot") {
            if (vendor == "ASUS" || vendor == "Gigabyte") path = "Boot > Secure Boot > Disabled";
            else if (vendor == "MSI") path = "Security > Secure Boot > Disabled";
            else if (vendor == "ASRock") path = "Security > Secure Boot > Disabled";
            else if (vendor == "Dell" || vendor == "HP" || vendor == "Lenovo")
                path = "Security > Secure Boot > Disabled";
            else path = "BIOS > Secure Boot > Disabled";
        }
        else { // "cpuv"
            // v1.2.3: AMD-first hint - the old table always said VT-x, so AMD
            // users (e.g. MSI B650, CPU-V=SVM) got a menu entry that does not
            // exist on their board. Intel machines keep the exact old strings.
            if (amdSvm) {
                if (vendor == "ASUS") path = "Advanced > CPU Config > SVM Mode";
                else if (vendor == "MSI") path = "OC > CPU Features > SVM Mode";
                else if (vendor == "Gigabyte") path = "Tweaker > SVM Mode";
                else if (vendor == "ASRock") path = "Advanced > CPU Config > SVM Mode";
                else if (vendor == "Dell") path = "CPU > SVM Mode";
                else if (vendor == "HP") path = "Security > SVM Mode";
                else if (vendor == "Lenovo") path = "Config > CPU > SVM Mode";
                else path = "BIOS > CPU Config > SVM Mode";
            }
            else if (vendor == "ASUS") path = "Advanced > CPU Config > VT-x";
            else if (vendor == "MSI") path = "OC > CPU Features > VT-x";
            else if (vendor == "Gigabyte") path = "Tweaker > VT-x / SVM Mode";
            else if (vendor == "ASRock") path = "Advanced > CPU Config > VT-x/SVM";
            else if (vendor == "Dell") path = "CPU > VT-x";
            else if (vendor == "HP") path = "Security > VT-x";
            else if (vendor == "Lenovo") path = "Config > CPU > VT-x";
            else path = "BIOS > CPU Config > VT-x";
        }

        return "[" + (vendor.empty() ? std::string("BIOS") : vendor) + ": " + path + "]";
    }

    // Maps adapter GUID (from GetAdaptersAddresses) to the PnP device instance id used by pnputil
    void collectNetAdapterInstances(std::map<std::string, std::string>& out)
    {
        static const GUID kGuidDevClassNet =
            { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

        HDEVINFO deviceInfo = SetupDiGetClassDevsA(&kGuidDevClassNet, NULL, NULL, DIGCF_PRESENT);
        if (deviceInfo == INVALID_HANDLE_VALUE) return;

        SP_DEVINFO_DATA data;
        ZeroMemory(&data, sizeof(data));
        data.cbSize = sizeof(data);

        for (DWORD index = 0; SetupDiEnumDeviceInfo(deviceInfo, index, &data); index++) {
            char instanceBuffer[512] = { 0 };
            DWORD needed = 0;
            std::string instanceId;
            if (SetupDiGetDeviceInstanceIdA(deviceInfo, &data, instanceBuffer, sizeof(instanceBuffer), &needed)) {
                instanceId = instanceBuffer;
            }

            std::string adapterGuid;
            // Scope must be DICS_FLAG_GLOBAL/DICS_FLAG_CONFIGSPECIFIC and KeyType
            // DIREG_DEV/DIREG_DRV - "DIREG_READ" does not exist in the Windows SDK.
            // DIREG_DRV opens the driver (software) key that holds NetCfgInstanceId.
            HKEY driverKey = SetupDiOpenDevRegKey(deviceInfo, &data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            if (driverKey != NULL && driverKey != INVALID_HANDLE_VALUE) {
                char guidBuffer[128] = { 0 };
                DWORD size = sizeof(guidBuffer);
                DWORD type = 0;
                if (RegQueryValueExA(driverKey, "NetCfgInstanceId", NULL, &type,
                    reinterpret_cast<LPBYTE>(guidBuffer), &size) == ERROR_SUCCESS &&
                    (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    adapterGuid = guidBuffer;
                }
                RegCloseKey(driverKey);
            }

            if (!adapterGuid.empty() && !instanceId.empty()) {
                out[toLowerCopy(adapterGuid)] = instanceId;
            }
        }
        SetupDiDestroyDeviceInfoList(deviceInfo);
    }

    // The undo script (Undo-FBI-Setup.ps1) uses this list to re-detect removed adapters
    void appendRemovedAdapterLog(const std::string& instanceId, const std::string& name)
    {
        char basePath[MAX_PATH] = { 0 };
        if (GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH) == 0) return;
        std::string directory = std::string(basePath) + "\\FBI-Setup";
        CreateDirectoryA(directory.c_str(), NULL);
        std::ofstream log(directory + "\\removed-adapters.log", std::ios::app);
        if (log.is_open()) {
            log << instanceId << "|" << name << "\n";
        }
    }

    struct SmbiosIdentity {
        bool valid = false;
        int smbiosMajor = 0;
        int smbiosMinor = 0;
        std::string biosVendor;      // Type 0 (v1.2.2)
        std::string biosVersion;     // Type 0 (v1.2.2)
        std::string systemManufacturer;
        std::string systemProduct;
        std::string systemVersion;
        std::string systemSerial;
        std::string boardManufacturer;
        std::string boardProduct;
        std::string boardVersion;
        std::string boardSerial;
        std::string uuidText;
        bool uuidNil = false;
        bool uuidSameByte = false;   // e.g. 11111111-1111-... (v1.2.2)
    };

    std::string formatSmbiosUuid(const BYTE* bytes)
    {
        char text[64] = { 0 };
        snprintf(text, sizeof(text),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        return text;
    }

    // Raw SMBIOS (GetSystemFirmwareTable) - the layer perm HWID spoofers patch
    SmbiosIdentity readSmbiosIdentity()
    {
        SmbiosIdentity identity;

        UINT bufferSize = GetSystemFirmwareTable(0x52534D42 /* 'RSMB' */, 0, NULL, 0);
        if (bufferSize == 0 || bufferSize > 8 * 1024 * 1024) return identity;

        std::vector<BYTE> buffer(bufferSize);
        UINT bytesRead = GetSystemFirmwareTable(0x52534D42, 0, buffer.data(), bufferSize);
        if (bytesRead == 0) return identity;

#pragma pack(push, 1)
        struct RawSmbiosData {
            BYTE Used20CallingMethod;
            BYTE SMBIOSMajorVersion;
            BYTE SMBIOSMinorVersion;
            BYTE DmiRevision;
            DWORD Length;
            BYTE SMBIOSTableData[1];
        };
#pragma pack(pop)

        const size_t headerSize = offsetof(RawSmbiosData, SMBIOSTableData);
        if (bytesRead < headerSize) return identity;

        const RawSmbiosData* raw = reinterpret_cast<const RawSmbiosData*>(buffer.data());
        identity.smbiosMajor = raw->SMBIOSMajorVersion;
        identity.smbiosMinor = raw->SMBIOSMinorVersion;
        DWORD tableLength = raw->Length;
        DWORD available = bytesRead - static_cast<DWORD>(headerSize);
        if (tableLength > available) tableLength = available;

        const BYTE* table = raw->SMBIOSTableData;
        DWORD offset = 0;
        while (offset + 4 <= tableLength) {
            const BYTE* header = table + offset;
            BYTE type = header[0];
            BYTE formattedLength = header[1];
            if (formattedLength < 4 || offset + formattedLength > tableLength) break;

            std::vector<std::string> strings;
            DWORD position = offset + formattedLength;
            while (position < tableLength && table[position] != 0) {
                DWORD start = position;
                while (position < tableLength && table[position] != 0) position++;
                strings.emplace_back(reinterpret_cast<const char*>(table + start), position - start);
                position++;
            }
            position++; // string-set terminator

            auto getString = [&](int index) -> std::string {
                if (index < 0 || index >= static_cast<int>(strings.size())) return "";
                return strings[index];
            };

            if (type == 0 && formattedLength >= 0x0A) {
                // Type 0: BIOS vendor (string 0) + BIOS version (string 1)
                identity.biosVendor = getString(0);
                identity.biosVersion = getString(1);
            }
            else if (type == 1 && formattedLength >= 0x18) {
                identity.valid = true;
                identity.systemManufacturer = getString(0);
                identity.systemProduct = getString(1);
                identity.systemVersion = getString(2);
                identity.systemSerial = getString(3);

                const BYTE* uuid = header + 8;
                bool allZero = true;
                bool allFF = true;
                bool sameByte = true;
                for (int i = 0; i < 16; i++) {
                    if (uuid[i] != 0x00) allZero = false;
                    if (uuid[i] != 0xFF) allFF = false;
                    if (uuid[i] != uuid[0]) sameByte = false;
                }
                identity.uuidNil = allZero || allFF;
                // e.g. 11111111-1111-1111-1111-111111111111 - synthetic, not OEM
                identity.uuidSameByte = sameByte && !allZero && !allFF;
                identity.uuidText = formatSmbiosUuid(uuid);
            }
            else if (type == 2 && formattedLength >= 8) {
                identity.boardManufacturer = getString(0);
                identity.boardProduct = getString(1);
                identity.boardVersion = getString(2);
                identity.boardSerial = getString(3);
            }
            else if (type == 127) {
                break;
            }

            offset = position;
        }
        return identity;
    }

    bool isPlaceholderIdentity(const std::string& value)
    {
        std::string lowered = toLowerCopy(trimCopy(value));
        if (lowered.empty()) return true;
        static const char* placeholders[] = {
            "to be filled by o.e.m.", "to be filled by o.e.m", "default string",
            "system product name", "system manufacturer", "system version",
            "not specified", "not applicable", "none", "n/a", "oem", "unknown",
            "base board version", "chassis version", "invalid", "sernum", "xxxx"
        };
        for (const char* placeholder : placeholders) {
            if (lowered == placeholder) return true;
        }
        return false;
    }

    bool isAllHexValue(const std::string& value)
    {
        if (value.size() < 8) return false;
        for (char c : value) {
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    // "BCF1D5C8102B6C3", "AT5LMDB1M": no spaces, letters + digits, high entropy
    bool isRandomToken(const std::string& value)
    {
        if (value.size() < 8 || value.find(' ') != std::string::npos) return false;
        bool hasAlpha = false;
        bool hasDigit = false;
        bool seen[256] = { false };
        int unique = 0;
        for (unsigned char c : value) {
            if (!seen[c]) { seen[c] = true; unique++; }
            if (c >= '0' && c <= '9') hasDigit = true;
            else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) hasAlpha = true;
        }
        if (!hasAlpha || !hasDigit) return false;
        return static_cast<double>(unique) / static_cast<double>(value.size()) >= 0.6;
    }

    // "6429-BM-11135", "10116-BV-11344": digits + dashes + short code
    bool isSyntheticPattern(const std::string& value)
    {
        if (value.size() < 10) return false;
        int dashes = 0;
        int digitRun = 0;
        bool longDigitRun = false;
        for (char c : value) {
            if (c == '-') { dashes++; digitRun = 0; }
            else if (c >= '0' && c <= '9') { digitRun++; if (digitRun >= 4) longDigitRun = true; }
            else if ((c >= 'A' && c <= 'Z') || c == ' ') { digitRun = 0; }
            else { return false; }
        }
        return dashes >= 2 && longDigitRun;
    }

    // v1.2.1: 0 = clean, 1 = weak anomaly, 2 = strong (hex/synthetic) pattern;
    // strong is counted separately because two independent strong hits confirm spoofing
    int identityScore(const std::string& value, bool* wasStrong)
    {
        if (wasStrong) *wasStrong = false;
        std::string cleaned = trimCopy(value);
        if (cleaned.empty()) return 0;
        if (isAllHexValue(cleaned) || isSyntheticPattern(cleaned)) {
            if (wasStrong) *wasStrong = true;
            return 2;
        }
        if (isRandomToken(cleaned)) return 1;
        if (isPlaceholderIdentity(cleaned)) return 1;
        return 0;
    }

    bool identityDiffers(const std::string& smbios, const std::string& registryValue)
    {
        if (smbios.empty() || registryValue.empty()) return false;
        return toLowerCopy(trimCopy(smbios)) != toLowerCopy(trimCopy(registryValue));
    }

    // v1.2.1: "v1.2.3" / "1.2.3-beta" -> {1,2,3}; false if not version-like
    bool parseVersionParts(const std::string& input, std::vector<int>& out)
    {
        std::string s = trimCopy(input);
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
        size_t cut = s.find_first_of("-+");
        if (cut != std::string::npos) s = s.substr(0, cut);
        if (s.empty()) return false;

        out.clear();
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, '.')) {
            if (part.empty()) return false;
            for (char c : part) {
                if (c < '0' || c > '9') return false;
            }
            out.push_back(atoi(part.c_str()));
        }
        return out.size() >= 2;
    }

    bool looksLikeVersion(const std::string& input)
    {
        std::vector<int> parts;
        return parseVersionParts(input, parts);
    }

    // true only when the remote tag is actually NEWER than the local version
    bool isNewerVersion(const std::string& remote, const std::string& local)
    {
        std::vector<int> r;
        std::vector<int> l;
        if (!parseVersionParts(remote, r) || !parseVersionParts(local, l)) return false;
        size_t n = (std::max)(r.size(), l.size());
        r.resize(n, 0);
        l.resize(n, 0);
        return r > l;
    }

    // v1.2.2: bounded command runner (bugfix: the old std::async/_popen "timeout"
    // never fired - the future's destructor waited for the child, and
    // runSystemCommand had no timeout at all, so a hung child froze the tool
    // before the screenshot hint was even printed).
    // - runs "cmd.exe /c <command>" (same command processor _popen used)
    // - stdout captured, stderr inherited from the console (same as _popen)
    // - breaks on PROCESS exit + drained pipe, not on pipe EOF, so a target that
    //   inherits the pipe (e.g. `start https://...` launching a browser) cannot
    //   stall the caller
    // - on deadline the whole job tree is killed (cmd, powershell, dism, ...)
    // Returns: 0 = finished, 1 = timed out (tree killed), -1 = launch failed.
    int runBoundedCommand(const char* command, DWORD timeoutMs,
        std::string* output, DWORD* exitCode)
    {
        if (command == NULL || command[0] == '\0') return -1;

        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        HANDLE outRead = NULL, outWrite = NULL;
        if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return -1;
        // only the write end may be inherited by the child
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);  // same as _popen: console
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        std::string cmdLine = std::string("cmd.exe /c ") + command;
        std::vector<char> clBuf(cmdLine.begin(), cmdLine.end());
        clBuf.push_back('\0');

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));
        BOOL created = CreateProcessA(NULL, clBuf.data(), NULL, NULL, TRUE,
            0 /* keep parent console, like _popen - no extra window */, NULL, NULL, &si, &pi);
        CloseHandle(outWrite); // parent copy - EOF comes when the child tree releases it
        if (!created) { CloseHandle(outRead); return -1; }

        // job object so a timeout kills the WHOLE tree (cmd -> powershell/dism/...)
        // NOTE: no JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE - a normal close must leave
        // surviving children (e.g. a browser launched via `start https://...`) alive
        HANDLE job = CreateJobObjectA(NULL, NULL);
        if (job != NULL) {
            if (!AssignProcessToJobObject(job, pi.hProcess)) {
                CloseHandle(job);
                job = NULL;
            }
        }

        std::string collected;
        char tmp[4096];
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        bool childExited = false;
        bool timedOut = false;

        for (;;) {
            // drain everything already in the pipe (never blocks)
            DWORD avail = 0;
            while (PeekNamedPipe(outRead, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                DWORD want = avail < sizeof(tmp) ? avail : sizeof(tmp);
                DWORD got = 0;
                if (!ReadFile(outRead, tmp, want, &got, NULL) || got == 0) break;
                if (output != NULL) collected.append(tmp, got);
            }

            if (!childExited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                childExited = true;
            }
            if (childExited) {
                // drain what was written before exit; PeekNamedPipe returns FALSE
                // with ERROR_BROKEN_PIPE once all writers closed AND the buffer is
                // empty - that means done (must NOT wait for the deadline here)
                DWORD rest = 0;
                if (!PeekNamedPipe(outRead, NULL, 0, NULL, &rest, NULL) || rest == 0) break;
                if (GetTickCount64() > deadline) break;
                continue;
            }

            if (GetTickCount64() >= deadline) {
                timedOut = true;
                if (job != NULL) TerminateJobObject(job, 1);
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                break;
            }
            Sleep(20);
        }

        if (!timedOut && exitCode != NULL) {
            DWORD code = 0;
            if (GetExitCodeProcess(pi.hProcess, &code)) *exitCode = code;
        }
        if (output != NULL) output->swap(collected);

        CloseHandle(outRead);
        if (job != NULL) CloseHandle(job); // on timeout TerminateJobObject already killed the tree
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return timedOut ? 1 : 0;
    }
}

void Helper::recordResult(const std::string& check, const std::string& status, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    // v1.2.2: upsert instead of append. A check re-run in the apply phase replaces its
    // initial entry (fresh status), so the final summary counts every check exactly once
    // instead of only the partial apply re-run ("17 passed" bug).
    for (auto& result : g_results) {
        if (result.check == check) {
            bool unchanged = (result.status == status && result.message == message);
            result.status = status;
            result.message = message;
            // v1.2.2: an unchanged re-run in the apply phase would only duplicate the
            // log line - log again only when the result actually changed
            if (!unchanged) {
                logWrite("  [" + status + "] " + check + " - " + message + " (updated)");
            }
            return;
        }
    }
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

// v1.2.1 (FBI): the shop promo box is completely removed - no MessageBox,
// no settings.ini opt-out (nothing to opt out of anymore)

bool Helper::promptApplyChanges()
{
    if (!hasPendingChanges()) {
        return false;
    }
    // v1.2.2: no console copy of the pending list - every item already printed
    // its "- Change needed:" discovery line, and the dialog repeats them anyway
    // (three copies used to stack up mid-screenshot)

    if (cliConfig.headless) {
        printConcern("- Headless mode: pending changes were not applied");
        return false;
    }

    std::string msg = "The checker found changes that may be needed:\n\n";
    {
        std::lock_guard<std::mutex> lock(g_pendingChangesMutex);
        for (const auto& change : g_pendingChanges) {
            msg += "  \x95 " + change + "\n";
        }
    }
    msg += "\nApply these changes now?";

    int choice = MessageBoxA(NULL, msg.c_str(),
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
        default:
            // v1.2.2: remaining C0 control chars must be \u-escaped or the
            // whole export becomes invalid JSON (0x1B from stripped ANSI etc.)
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else out += c;
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

    // v1.2.2: snapshot under the results mutex (threaded checks may still record)
    decltype(g_results) results;
    {
        std::lock_guard<std::mutex> lock(g_resultsMutex);
        results = g_results;
    }

    int passed = 0, failed = 0, warnings = 0, skipped = 0, info = 0;
    for (auto& r : results) {
        if (r.status == "OK") passed++;
        else if (r.status == "FAIL") failed++;
        else if (r.status == "WARN") warnings++;
        else if (r.status == "SKIPPED") skipped++;
        // v1.2.2: INFO results (UAC, Windows Update) were not counted at all,
        // so passed+failed+warnings+skipped never matched results.length
        else if (r.status == "INFO") info++;
    }

    f << "{\n";
    f << "  \"timestamp\": \"" << escapeJSON(getTimestampISO()) << "\",\n";
    f << "  \"program\": \"" << escapeJSON(g_appName) << "\",\n";
    f << "  \"version\": \"" << escapeJSON(g_appVersion) << "\",\n";
    f << "  \"results\": [\n";
    for (size_t i = 0; i < results.size(); i++) {
        f << "    {\"check\": \"" << escapeJSON(results[i].check) << "\", "
          << "\"status\": \"" << escapeJSON(results[i].status) << "\", "
          << "\"message\": \"" << escapeJSON(results[i].message) << "\"}";
        if (i < results.size() - 1) f << ",";
        f << "\n";
    }
    f << "  ],\n";
    f << "  \"summary\": {\n";
    f << "    \"passed\": " << passed << ",\n";
    f << "    \"failed\": " << failed << ",\n";
    f << "    \"warnings\": " << warnings << ",\n";
    f << "    \"skipped\": " << skipped << ",\n";
    f << "    \"info\": " << info << ",\n";
    f << "    \"restart_required\": " << (restartRequired ? "true" : "false") << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();

    printSuccess("- Results exported to " + cliConfig.exportPath, false);
}

std::string Helper::fetchURL(const std::wstring& url)
{
    std::string result;
    // v1.2.2: version-correct User-Agent (was hardcoded "FBI-Setup/2.0")
    std::wstring agent = L"FBI-Setup/" + std::wstring(g_appVersion.begin(), g_appVersion.end());
    HINTERNET hSession = WinHttpOpen(agent.c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    // v1.2.2: short timeouts so a dead network cannot stall a check for minutes
    // (used by the update check and the new internet HTTP fallback)
    WinHttpSetTimeouts(hSession, 5000, 5000, 8000, 8000);

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
    std::cout << "  --skip N[,M,...]    Skip specific checks by number (1-28)\n";
    std::cout << "  --only N[,M,...]    Run only specific checks by number (1-28)\n\n";
    std::cout << "Check Numbers:\n";
    std::cout << "  1=WindowsDefender 2=3rdPartyAV 3=SecureBoot 4=RiotVanguard\n";
    std::cout << "  5=VCRedist 6=Chrome 7=ChromeProtection 8=TimeSync 9=Winver\n";
    std::cout << "  10=Symbols 11=FastBoot 12=ExploitProtection 13=SmartScreen\n";
    std::cout << "  14=GameBar 15=TPM 16=CoreIsolation 17=DMAProtection 18=ModifiedOS\n";
    std::cout << "  19=Internet 20=NetworkAdapters 21=VM-Detect 22=Auto-Update\n";
    std::cout << "  23=SecurityMitigations 24=UAC 25=WindowsUpdate\n";
    std::cout << "  26=CPU-V 27=HWID-Spoofer 28=Activation\n";
}

CLIConfig Helper::parseCLI(int argc, char* argv[])
{
    CLIConfig config;
    // v1.2.2: only 1-28 are valid check ids; everything else is reported
    auto parseIds = [](const std::string& val, std::vector<int>& dest, const char* flag) {
        std::stringstream ss(val);
        std::string token;
        while (std::getline(ss, token, ',')) {
            int id = -1;
            try { id = std::stoi(token); } catch (...) { id = -1; }
            if (id < 1 || id > 28) {
                std::cout << "[!] Ignored invalid check number for " << flag
                    << ": \"" << token << "\" (valid: 1-28)\n";
                continue;
            }
            dest.push_back(id);
        }
    };

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
        else if (arg == "--skip") {
            if (i + 1 < argc) parseIds(argv[++i], config.skipChecks, "--skip");
            else std::cout << "[!] --skip needs a check number (1-28)\n";
        }
        else if (arg == "--only") {
            if (i + 1 < argc) parseIds(argv[++i], config.onlyChecks, "--only");
            else std::cout << "[!] --only needs a check number (1-28)\n";
        }
        else if (arg == "--export") {
            if (i + 1 < argc) config.exportPath = argv[++i];
            else std::cout << "[!] --export needs a file path\n";
        }
        else {
            std::cout << "[!] Ignored unknown argument: " << arg << "\n";
        }
    }

    // v1.2.2: --only wins over --skip - say so instead of silently dropping
    if (!config.onlyChecks.empty() && !config.skipChecks.empty()) {
        std::cout << "[!] Both --only and --skip given: --only takes precedence, --skip is ignored\n";
        config.skipChecks.clear();
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
    // v1.2.2: init + check - an uninitialized buffer would be used as path (UB)
    wchar_t tempPath[MAX_PATH] = {};
    DWORD tempLen = GetTempPathW(MAX_PATH, tempPath);
    if (tempLen == 0 || tempLen >= MAX_PATH) {
        wcscpy_s(tempPath, L"C:\\Windows\\Temp\\");
    }
    std::wstring dir = std::wstring(tempPath) + L"FBI-Setup";
    CreateDirectoryW(dir.c_str(), NULL);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    char filename[64];
    // v1.2.2: seconds in the name - runs within the same minute used to append to
    // the same file, so several runs (even different versions) ended up in one log
    snprintf(filename, sizeof(filename), "setup-%04d-%02d-%02d-%02d%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

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
    // v1.2.2: hold the log mutex while closing - a detached VCRedist thread
    // could otherwise be inside logWrite() while the stream is closed
    // (the final line is written directly: logWrite would re-lock the mutex)
    std::lock_guard<std::mutex> lock(logMutex);
    if (logEnabled && logFile.is_open()) {
        logFile << "=== " + g_appName + " finished ===" << std::endl;
        logFile.close();
        logEnabled = false;
    }
}

void Helper::logWrite(const std::string& message)
{
    // v1.2.2: check inside the lock (the old check ran before it, so the file
    // could be closed between check and write)
    std::lock_guard<std::mutex> lock(logMutex);
    if (!logEnabled || !logFile.is_open()) return;
    logFile << message << std::endl;
    logFile.flush();
}

std::string Helper::runSystemCommandWithOutput(const char* command, DWORD timeoutMs)
{
    if (command == NULL) return "";
    // v1.2.2 bugfix: real deadline (the std::async version's future destructor
    // blocked until the child exited - "TIMEOUT" could never appear early)
    std::string output;
    int rc = runBoundedCommand(command, timeoutMs, &output, NULL);
    if (rc == 1) return "TIMEOUT";   // killed after deadline
    if (rc < 0) return "";           // launch failed
    return output;
}

void Helper::backupRegistryKey(HKEY hKeyParent, const std::string& subkey)
{
    // Export every key only once per run: the first call always happens before the
    // key is modified, later calls would already capture changed data.
    // Undo-FBI-Setup.ps1 imports the recorded backup file (or, as a fallback, the
    // OLDEST matching .reg file in C:\Windows\Temp - the one closest to the
    // pre-change state) to revert the change.
    static std::mutex backupMutex;
    static std::vector<std::string> backedUpKeys;
    {
        std::lock_guard<std::mutex> lock(backupMutex);
        if (std::find(backedUpKeys.begin(), backedUpKeys.end(), subkey) != backedUpKeys.end()) return;
    }

    HKEY hKey;
    if (RegOpenKeyEx(hKeyParent, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

    // v1.2.2: init + check (uninitialized path = UB, backups would land in CWD)
    wchar_t tempPath[MAX_PATH] = {};
    DWORD tempLen = GetTempPathW(MAX_PATH, tempPath);
    if (tempLen == 0 || tempLen >= MAX_PATH) {
        wcscpy_s(tempPath, L"C:\\Windows\\Temp\\");
    }
    std::wstring dir = std::wstring(tempPath) + L"FBI-Setup\\Backup";
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
    // reg export needs the full hive path (HKLM\...), RegOpenKeyEx above does not
    std::string hivePrefix = (hKeyParent == HKEY_LOCAL_MACHINE) ? "HKLM\\" :
        (hKeyParent == HKEY_CURRENT_USER) ? "HKCU\\" :
        (hKeyParent == HKEY_CLASSES_ROOT) ? "HKCR\\" :
        (hKeyParent == HKEY_USERS) ? "HKU\\" : "";
    std::string cmd = "reg export \"" + hivePrefix + subkey + "\" \"" + exportPath + "\" /y 2>nul";
    runSystemCommand(cmd.c_str());
    RegCloseKey(hKey);

    // Undo-FBI-Setup.ps1 restores the backup that was taken before the first
    // reg: marker for this key (last backup: entry preceding it in the log)
    if (std::filesystem::exists(exportPath)) {
        logAppliedChange("backup:" + subkey + "|" + exportPath);
    }

    std::lock_guard<std::mutex> lock(backupMutex);
    backedUpKeys.push_back(subkey);
}

void Helper::logAppliedChange(const std::string& change)
{
    // Undo-FBI-Setup.ps1 reverts only the entries recorded here, so every
    // non-reversible-looking action (bcdedit, DISM, services, adapters) logs itself
    char basePath[MAX_PATH] = { 0 };
    if (GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH) == 0) return;
    std::string directory = std::string(basePath) + "\\FBI-Setup";
    CreateDirectoryA(directory.c_str(), NULL);
    std::ofstream log(directory + "\\applied-changes.log", std::ios::app);
    if (log.is_open()) {
        log << change << "\n";
    }
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
    if (!g_printedLines.insert(std::string("[i]") + message).second) return;
    Color::setForegroundColor(Color::LightGray);
    std::cout << "  " << message << std::endl;
}

void Checks::checkInternet()
{
    SetConsoleTitleA("Checking Internet Connection");

    // v1.2.2: use the ping EXIT CODE (language-independent, "TTL=" parsing broke
    // on localized output) and fall back to an HTTP probe when ICMP is blocked
    // by a firewall - blocked ping does NOT mean no internet
    auto testPing = []() -> bool {
        int rc = Helper::runSystemCommandExitCode("ping -n 1 -w 3000 8.8.8.8");
        return rc == 0;
    };
    auto testHttp = []() -> bool {
        std::string body = Helper::fetchURL(L"http://www.msftconnecttest.com/connecttest.txt");
        if (body.empty()) body = Helper::fetchURL(L"https://www.msftconnecttest.com/connecttest.txt");
        // v1.2.2 bugfix: `|| !body.empty()` subsumed the find - ANY answer
        // (captive portal, error page) counted as online. Only the expected
        // body of the Microsoft connect test proves connectivity.
        return body.find("Microsoft Connect Test") != std::string::npos;
    };
    auto online = [&]() -> bool { return testPing() || testHttp(); };

    if (online()) {
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
    Helper::logAppliedChange("info:network was repaired (DNS flush + Winsock reset) - a reboot may be required; the Winsock catalog was reset to Windows defaults");
    Sleep(1000);

    if (online()) {
        Helper::printSuccess("- Internet restored after DNS/Winsock reset", true);
        Helper::recordResult("Internet", "OK", "Fixed via DNS/Winsock reset");
        return;
    }

    Helper::runSystemCommand("ipconfig /release");
    Sleep(500);
    Helper::runSystemCommand("ipconfig /renew");
    Sleep(2000);

    if (online()) {
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
    // v1.2.2: classify with the device instance already in hand (collected BEFORE the
    // approval prompt) so each candidate carries proof of being virtual
    std::map<std::string, std::string> deviceInstances;
    collectNetAdapterInstances(deviceInstances);

    struct UnusedCandidate {
        std::string name;
        std::string instanceId;
        std::string reason;
    };
    std::vector<UnusedCandidate> unusedVirtual;

    for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != NULL; adapter = adapter->Next) {
        // v1.2.4: Windows' own loopback (127.0.0.1) is hidden - it exists on
        // every system, cannot be removed and never carries external traffic,
        // so it was pure screenshot noise ("why is this kept?"). Skipping it
        // here also keeps it out of the used/total count and the summary.
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        total++;

        std::string name = wideToUtf8(adapter->FriendlyName);
        if (name.empty() && adapter->AdapterName != NULL) {
            name = adapter->AdapterName;
        }
        if (name.empty()) {
            name = "Unknown adapter";
        }

        std::string instanceId;
        if (adapter->AdapterName != NULL) {
            auto device = deviceInstances.find(toLowerCopy(adapter->AdapterName));
            if (device != deviceInstances.end()) {
                instanceId = device->second;
            }
        }

        bool hasAddress = adapter->FirstUnicastAddress != NULL;
        bool hasGateway = adapter->FirstGatewayAddress != NULL;
        bool isLoopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        bool isUsed = adapter->OperStatus == IfOperStatusUp && !isLoopback && (hasAddress || hasGateway);
        std::string reason;
        bool virtualCandidate = !isLoopback && isVirtualAdapter(name, adapter, instanceId, &reason);
        // v1.2.2: only propose removal when we actually have a device instance -
        // pnputil cannot remove anything without one (seen with "eth0" in a VM)
        if (isUsed) used++;
        else if (virtualCandidate && !instanceId.empty()) {
            unusedVirtual.push_back({ name, instanceId, reason });
        }

        std::string line = name + " - " + (isUsed ? "USED" : "NOT USED") +
            " (" + statusToString(adapter->OperStatus) + ")";
        if (isLoopback) line += " [loopback]";
        else if (virtualCandidate) line += " [virtual]";
        else if (!isUsed) line += instanceId.empty() ? " [unknown]" : " [hardware]";

        // v1.2.4: flagged adapters (VPNs, virtual adapters) warn in orange -
        // a VPN/virtual NIC is a classic reason for game kicks
        std::string kickWarn;
        if (virtualCandidate) kickWarn = " (can cause game kicks)";

        if (isUsed) {
            Helper::printAdapterLine("- Adapter: " + line, true, kickWarn);
        }
        else {
            Helper::printAdapterLine("Adapter: " + line, false, kickWarn);
        }

        if (!summary.empty()) summary += "; ";
        summary += line + kickWarn;
    }

    if (total == 0) {
        Helper::printConcern("- No network adapters found");
        Helper::recordResult("Network Adapters", "WARN", "No adapters found");
        return;
    }

    std::string usage = std::to_string(used) + "/" + std::to_string(total) + " used: " + summary;

    // v1.2.1: remove unused virtual adapters (Hyper-V/VMware/VPN/Bluetooth leftovers...)
    if (unusedVirtual.empty()) {
        Helper::recordResult("Network Adapters", "OK", usage);
        return;
    }

    auto describe = [](const UnusedCandidate& entry) {
        std::string text = entry.name;
        if (!entry.instanceId.empty()) text += " (" + entry.instanceId + ")";
        if (!entry.reason.empty()) text += " - " + entry.reason;
        return text;
    };

    std::string names;
    for (const auto& entry : unusedVirtual) {
        if (!names.empty()) names += ", ";
        names += describe(entry);
    }

    if (!Helper::requestChange("Uninstall unused virtual network adapter(s): " + names)) {
        Helper::recordResult("Network Adapters", "WARN",
            usage + "; not removed yet: " + names);
        return;
    }

    int removed = 0;
    for (const auto& entry : unusedVirtual) {
        if (entry.instanceId.empty()) {
            Helper::printInfo("No device instance found for adapter: " + entry.name);
            continue;
        }

        std::string command = "pnputil /remove-device \"" + entry.instanceId + "\"";
        int exitCode = Helper::runSystemCommandExitCode(command.c_str());
        if (exitCode == 0) {
            Helper::printSuccess("- Removed unused virtual adapter: " + entry.name, true);
            appendRemovedAdapterLog(entry.instanceId, entry.name);
            removed++;
        }
        else {
            Helper::printError("- Could not remove adapter " + entry.name +
                " (pnputil exit " + std::to_string(exitCode) + ")");
        }
    }

    std::string namesShort;
    for (const auto& entry : unusedVirtual) {
        if (!namesShort.empty()) namesShort += ", ";
        namesShort += entry.name;
    }

    Helper::recordResult("Network Adapters", removed > 0 ? "OK" : "WARN",
        usage + "; removed " + std::to_string(removed) + "/" +
        std::to_string(unusedVirtual.size()) + " unused virtual adapter(s): " + namesShort);
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
        Helper::logAppliedChange("reg:" + std::string(subkey));
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

    // v1.2.3: no system restore point is created anymore (see apply phase) -
    // Undo-FBI-Setup.ps1 reverts the setDword writes above from the
    // %TEMP%\FBI-Setup\Backup exports taken before them + markers

    std::string bcdOutput = lower(Helper::runSystemCommandWithOutput("bcdedit /enum {current}", 5000));
    // v1.2.2: evaluate ONLY the hypervisorlaunchtype line - searching the whole
    // output for "off" false-matched other entries (e.g. "hypervisorlaunchtype"
    // absent but some other line containing "off")
    std::string hvLine;
    {
        size_t pos = bcdOutput.find("hypervisorlaunchtype");
        if (pos != std::string::npos) {
            size_t end = bcdOutput.find('\n', pos);
            hvLine = bcdOutput.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }
    }
    if (bcdOutput == "timeout" || bcdOutput.empty()) {
        Helper::printConcern("- Could not read hypervisor launch status");
        appendSummary("hypervisorlaunchtype=unknown");
        warning = true;
    }
    else if (!hvLine.empty() &&
        (hvLine.find("off") != std::string::npos || hvLine.find("aus") != std::string::npos)) {
        Helper::printSuccess("- Hypervisor launch is already off", false);
        appendSummary("hypervisorlaunchtype=off");
    }
    else {
        if (!Helper::requestChange("Set hypervisorlaunchtype to off")) {
            appendSummary("hypervisorlaunchtype=change pending");
            warning = true;
            // v1.2.2: NO early return here - keep going so the Hyper-V feature
            // pendings below are registered too, and so a result is always recorded
        }
        else {
            // v1.2.2: check the exit code - bcdedit needs admin rights and fails
            // silently otherwise, which used to report a false "changed to off"
            int rc = Helper::runSystemCommandExitCode("bcdedit /set hypervisorlaunchtype off");
            if (rc == 0) {
                Helper::logAppliedChange("bcdedit:hypervisorlaunchtype=off");
                Helper::printSuccess("- Set hypervisor launch type to off", true);
                appendSummary("hypervisorlaunchtype=changed to off");
                changed = true;
            }
            else {
                Helper::printError("- Failed to set hypervisor launch type (bcdedit exit " +
                    std::to_string(rc) + ")");
                appendSummary("hypervisorlaunchtype=failed");
                failed = true;
            }
        }
    }

    auto checkAndDisableFeature = [&](const std::string& featureName) {
        // v1.2.2: ask PowerShell for the enum state first - DISM text output is
        // localized ("State : Deaktiviert", "État : Activé", ...) and the old
        // keyword matching only understood English + German
        bool featureEnabled = false;
        bool stateKnown = false;
        {
            std::string psCommand =
                "powershell -NoProfile -NonInteractive -Command \""
                "(Get-WindowsOptionalFeature -Online -FeatureName '" + featureName + "' -ErrorAction Stop).State\" 2>nul";
            std::string psState = toLowerCopy(trimCopy(
                Helper::runSystemCommandWithOutput(psCommand.c_str(), 20000)));
            // v1.2.2 fix: psState is already lowercased - comparing to "TIMEOUT"
            // could never match (the timeout case fell through to DISM text matching)
            if (psState != "timeout" && psState != "time-out" && !psState.empty()) {
                // "Disabled" / "Enabled" / "Enable Pending" / "Disable Pending" / "Absent"
                if (psState.find("disabled") != std::string::npos ||
                    psState.find("absent") != std::string::npos) {
                    stateKnown = true; featureEnabled = false;
                }
                else if (psState.find("enabled") != std::string::npos) {
                    stateKnown = true; featureEnabled = true;
                }
            }
        }

        if (!stateKnown) {
            // fallback: localized DISM text matching (English + German + generic)
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
                stateKnown = true; featureEnabled = false;
            }
            else if (output.find("state : enabled") != std::string::npos ||
                output.find("state: enabled") != std::string::npos ||
                output.find("aktiviert") != std::string::npos) {
                stateKnown = true; featureEnabled = true;
            }
            else {
                Helper::printInfo("Windows feature status is unclear (treated as disabled): " + featureName);
                appendSummary(featureName + "=likely disabled");
                return;
            }
        }

        if (!featureEnabled) {
            Helper::printSuccess("- Windows feature is already disabled: " + featureName, false);
            appendSummary(featureName + "=disabled");
            return;
        }

        if (!Helper::requestChange("Disable Windows feature " + featureName)) {
            appendSummary(featureName + "=disable pending");
            warning = true;
            return;
        }

        std::string disableCommand = "dism /online /disable-feature /featurename:" + featureName + " /norestart";
        // v1.2.2: check the exit code - DISM can fail (pending actions, servicing
        // stack) and the old code reported success regardless
        int rc = Helper::runSystemCommandExitCode(disableCommand.c_str());
        if (rc == 0 || rc == 3010) { // 3010 = success, reboot required
            Helper::printSuccess("- Disabled Windows feature: " + featureName, true);
            Helper::logAppliedChange("dism:" + featureName);
            appendSummary(featureName + "=changed to disabled");
            changed = true;
        }
        else {
            Helper::printError("- Failed to disable Windows feature " + featureName +
                " (dism exit " + std::to_string(rc) + ")");
            appendSummary(featureName + "=failed");
            failed = true;
        }
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

    // v1.2.2: Hyper-V guest marker (VMGUID value only exists inside guests)
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Virtual Machine\\Guest\\Parameters",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        isVM = true; vmType = vmType.empty() ? "Hyper-V" : vmType + "/Hyper-V"; RegCloseKey(hKey);
    }

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm) {
        // v1.2.2 bugfix: the service handle was never closed (handle leak per run)
        SC_HANDLE vboxSvc = OpenService(scm, "VBoxService", SERVICE_QUERY_STATUS);
        if (vboxSvc != NULL) {
            isVM = true; vmType = vmType.empty() ? "VirtualBox" : vmType;
            CloseServiceHandle(vboxSvc);
        }
        CloseServiceHandle(scm);
    }

    // v1.2.2: firmware identifiers in the BIOS registry block - catches QEMU/KVM,
    // Bochs, Xen, Parallels etc. that leave no service/registry marker behind
    if (!isVM) {
        std::string sysMan = toLowerCopy(trimCopy(readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemManufacturer")));
        std::string sysProd = toLowerCopy(trimCopy(readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName")));
        std::string board = toLowerCopy(trimCopy(readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardManufacturer")));
        std::string blob = sysMan + " " + sysProd + " " + board;
        struct VmHint { const char* needle; const char* label; };
        static const VmHint hints[] = {
            { "qemu", "QEMU/KVM" }, { "bochs", "Bochs" }, { "kvm", "KVM" },
            { "xen", "Xen" }, { "parallels", "Parallels" },
            { "virtual machine", "Hyper-V" }, { "virtualbox", "VirtualBox" },
            { "vmware virtual", "VMware" }, { "innotek", "VirtualBox" },
            { "vbox", "VirtualBox" }, { "bhyve", "bhyve" }, { "amazon ec2", "Nitro" },
        };
        for (const VmHint& hint : hints) {
            if (blob.find(hint.needle) != std::string::npos) {
                isVM = true; vmType = hint.label; break;
            }
        }
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
        // v1.2.2: init + check - on failure the old code used garbage as path
        char basePath[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", basePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return "";
        std::string dir = std::string(basePath) + "\\FBI-Setup";
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
                tag = trimCopy(tag);
                // v1.2.1: ignore poisoned/stale cache entries that are not version-like
                if (storedRepo == expectedRepo && looksLikeVersion(tag)) {
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

            // v1.2.1: only accept real version tags (the API also returns
            // non-version tags, and garbage must never trigger the dialog)
            if (!looksLikeVersion(tag)) {
                Helper::printConcern("- Could not parse update info");
                Helper::recordResult("Auto-Update", "WARN", "Could not parse release info");
                return;
            }
            writeCachedTag(cachePath, tag, Helper::g_repoUrl);
        }
    }

    // v1.2.1: only warn when the remote tag is actually newer - equal or
    // older tags (dev builds, re-releases) must not show the dialog
    if (isNewerVersion(tag, Helper::g_appVersion)) {
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
        // v1.2.2: no console line when there is nothing to do - "No new updates"
        // only showed noise (still recorded for log/JSON)
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

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender");

    if (checkDefenderTamperProtection()) {
        // v1.2.2: record the check itself too - it used to vanish from the
        // summary and the JSON export when Tamper Protection blocked it
        Helper::recordResult("Windows Defender", "SKIPPED", "Blocked by Tamper Protection");
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

    // Always set registry policies (v1.2.2: check every result - the old code
    // reported "disabled (changed)" even when the writes failed, e.g. by policy)
    HKEY hKey = NULL; DWORD val = 1;
    bool policiesOk = true;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (ck != ERROR_SUCCESS) {
        policiesOk = false;
    }
    else {
        if (RegSetValueEx(hKey, "DisableAntiSpyware", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) != ERROR_SUCCESS) {
            policiesOk = false;
        }
        RegCloseKey(hKey);
        hKey = NULL;
    }

    ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (ck != ERROR_SUCCESS) {
        policiesOk = false;
    }
    else {
        if (RegSetValueEx(hKey, "DisableRealtimeMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) != ERROR_SUCCESS ||
            RegSetValueEx(hKey, "DisableBehaviorMonitoring", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) != ERROR_SUCCESS ||
            RegSetValueEx(hKey, "DisableOnAccessProtection", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) != ERROR_SUCCESS ||
            RegSetValueEx(hKey, "DisableScanOnRealtimeEnable", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) != ERROR_SUCCESS) {
            policiesOk = false;
        }
        RegCloseKey(hKey);
        hKey = NULL;
    }

    // Undo-FBI-Setup.ps1 re-enables the service from this marker
    Helper::logAppliedChange("service:WinDefend=disabled");
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows Defender");

    if (!policiesOk) {
        Helper::printError("- Failed to set Windows Defender policy values (Error 25/26)");
        Helper::recordResult("Windows Defender", "FAIL", "Policy write failed");
    }
    else if (serviceStopped) {
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

    // v1.2.1: WMIC is removed by default on newer Windows 11 builds,
    // so query SecurityCenter2 through PowerShell/CIM instead (the same source
    // WMIC used). The marker tells us whether PowerShell actually ran.
    std::string psCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct).displayName; 'CL-AV-DONE'"
        "\" 2>nul";
    std::string avOutput = Helper::runSystemCommandWithOutput(psCommand.c_str(), 30000);
    if (avOutput == "TIMEOUT") avOutput.clear();
    bool psWorked = avOutput.find("CL-AV-DONE") != std::string::npos;

    std::string antivirusList;
    if (psWorked) {
        std::stringstream avStream(avOutput);
        std::string line;
        while (std::getline(avStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string wmicPrefix = "displayName="; // tolerate old WMIC-style output
            if (line.rfind(wmicPrefix, 0) == 0) line = line.substr(wmicPrefix.size());
            line = trimCopy(line);
            if (line.empty() || line == "CL-AV-DONE") continue;

            std::string lowered = line;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // "Windows Defender" and the newer "Microsoft Defender Antivirus" display name
            if (lowered.find("windows defender") != std::string::npos ||
                lowered.find("microsoft defender") != std::string::npos) continue;
            if (antivirusList.find(line) != std::string::npos) continue;

            if (!antivirusList.empty()) antivirusList += ", ";
            antivirusList += line;
        }
    }
    else {
        // Fallback without PowerShell/CIM: known AV products in the uninstall registry
        static const char* knownAv[] = {
            "norton", "mcafee", "kaspersky", "bitdefender", "eset", "avg", "avast",
            "avira", "sophos", "trend micro", "f-secure", "malwarebytes", "g data",
            "zonealarm", "comodo", "bullguard", "secureanywhere", "webroot"
        };
        HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
        const char* uninstallPaths[] = {
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
        };

        for (HKEY root : roots) {
            for (const char* uninstallPath : uninstallPaths) {
                HKEY hKey;
                if (RegOpenKeyEx(root, uninstallPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) continue;

                DWORD idx = 0;
                char subkeyName[256];
                DWORD subkeyNameSize = sizeof(subkeyName);
                while (RegEnumKeyEx(hKey, idx, subkeyName, &subkeyNameSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                    HKEY hSubkey;
                    if (RegOpenKeyEx(hKey, subkeyName, 0, KEY_READ, &hSubkey) == ERROR_SUCCESS) {
                        char dn[256] = "";
                        DWORD dnSize = sizeof(dn);
                        if (RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)dn, &dnSize) == ERROR_SUCCESS) {
                            std::string display = trimCopy(dn);
                            std::string lowered = display;
                            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            for (const char* marker : knownAv) {
                                if (lowered.find(marker) != std::string::npos) {
                                    if (antivirusList.find(display) == std::string::npos) {
                                        if (!antivirusList.empty()) antivirusList += ", ";
                                        antivirusList += display;
                                    }
                                    break;
                                }
                            }
                        }
                        RegCloseKey(hSubkey);
                    }
                    subkeyNameSize = sizeof(subkeyName);
                    ++idx;
                }
                RegCloseKey(hKey);
            }
        }

        if (antivirusList.empty()) {
            Helper::printConcern("- Could not query antivirus products (PowerShell blocked, no known AV in registry)");
            Helper::recordResult("3rd Party AV", "WARN", "AV detection unavailable");
            return;
        }
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
        bool anyFailed = false;
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
                    // v1.2.2 bugfix: the old split kept the WHOLE list as the first
                    // token (comma was only advanced after substr), so with >= 2 AVs
                    // only the last product was ever matched
                    size_t start = 0;
                    while (start != std::string::npos && start < antivirusList.size()) {
                        size_t comma = antivirusList.find(", ", start);
                        std::string single = (comma == std::string::npos)
                            ? antivirusList.substr(start)
                            : antivirusList.substr(start, comma - start);
                        start = (comma == std::string::npos) ? std::string::npos : comma + 2;
                        if (displayName.find(single) != std::string::npos) {
                            char uninstall[512] = ""; DWORD usSize = sizeof(uninstall);
                            if (RegQueryValueEx(hSubkey, "UninstallString", NULL, NULL, (LPBYTE)uninstall, &usSize) == ERROR_SUCCESS) {
                                // v1.2.2: quote the executable (paths with spaces broke
                                // the command) and pick the right silent switch:
                                // MSI wants /qn, InstallShield/NSIS-style wants /S
                                std::string raw(uninstall);
                                std::string lowered = raw;
                                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                bool isMsi = lowered.find("msiexec") != std::string::npos;
                                std::string cmd;
                                if (!raw.empty() && raw[0] != '"') {
                                    // quote up to and including .exe/.msi if unquoted
                                    // (NOMINMAX: avoid std::min, the Windows.h macro wins)
                                    size_t exePos = lowered.find(".exe");
                                    size_t msiPos = lowered.find(".msi");
                                    size_t endPos = std::string::npos;
                                    if (exePos != std::string::npos) endPos = exePos;
                                    if (msiPos != std::string::npos &&
                                        (endPos == std::string::npos || msiPos < endPos)) endPos = msiPos;
                                    if (endPos != std::string::npos) {
                                        endPos += 4; // include the extension
                                        std::string exePart = raw.substr(0, endPos);
                                        std::string rest = raw.substr(endPos);
                                        if (exePart.find(' ') != std::string::npos) {
                                            cmd = "\"" + exePart + "\"" + rest;
                                        }
                                        else cmd = raw;
                                    }
                                    else cmd = raw;
                                }
                                else cmd = raw;
                                cmd += isMsi ? " /qn /norestart" : " /S /quiet /norestart";
                                // v1.2.2: check the exit code - "attempted" must not
                                // count as success when the uninstaller failed
                                int rc = Helper::runSystemCommandExitCode(cmd.c_str());
                                if (rc == 0 || rc == 3010) { // 3010 = success, reboot pending
                                    anyRemoved = true;
                                }
                                else {
                                    anyFailed = true;
                                    Helper::printConcern("- Uninstall of " + single +
                                        " returned exit code " + std::to_string(rc));
                                }
                            }
                            break;
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
            Helper::logAppliedChange("info:3rd party AV uninstall was attempted (" + antivirusList +
                ") - reinstall it manually if it is still needed");
            Helper::recordResult("3rd Party AV", "OK", "Uninstall attempted: " + antivirusList);
        } else if (anyFailed) {
            Helper::printError("- Uninstall of 3rd party AV failed (" + antivirusList + ")");
            Helper::recordResult("3rd Party AV", "FAIL", "Uninstall failed: " + antivirusList);
        } else {
            Helper::printError("- 3rd party Anti-Virus detected: " + antivirusList);
            Helper::recordResult("3rd Party AV", "FAIL", "Found: " + antivirusList);
        }
        return;
    }

    Helper::recordResult("3rd Party AV", "OK", "None detected");
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

        char displayName[256] = {};
        DWORD displayNameSize = sizeof(displayName);
        result = RegQueryValueEx(hSubkey, "DisplayName", NULL, NULL, (LPBYTE)displayName, &displayNameSize);
        displayName[sizeof(displayName) - 1] = '\0'; // v1.2.2: force termination
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
                // v1.2.2 bugfix: _P_WAIT blocked headless runs forever on the
                // interactive uninstall dialog - detach in headless mode
                if (Helper::cliConfig.headless) {
                    _spawnl(_P_DETACH, vgPath, "installer.exe", NULL);
                }
                else {
                    _spawnl(_P_WAIT, vgPath, "installer.exe", NULL);
                }
                Helper::logAppliedChange("info:Riot Vanguard uninstall was started - reinstall it manually if it is still needed");
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

namespace {
    // v1.2.2: true when the VC runtime DLL exists in the given directory.
    // A 32-bit build must disable WOW64 file system redirection, otherwise
    // "System32" resolves to SysWOW64 and the x64 DLLs can never be seen.
    bool vcDllPresent(const char* dir, const char* name)
    {
        std::string path = std::string(dir) + name;
#if !defined(_WIN64)
        PVOID oldRedir = NULL;
        BOOL disabled = Wow64DisableWow64FsRedirection(&oldRedir);
        bool found = std::filesystem::exists(path);
        if (disabled) Wow64RevertWow64FsRedirection(oldRedir);
        return found;
#else
        return std::filesystem::exists(path);
#endif
    }

    // v1.2.2 bugfix: the old check ORed the architectures per DLL, so a
    // half-installed runtime (e.g. only the x64 DLLs) reported "already
    // installed" while x86 games still failed. Both architectures are now
    // required on 64-bit Windows; 32-bit Windows only has System32.
    bool vcRedistInstalled()
    {
        const bool is64bitOs = std::filesystem::exists("C:\\Windows\\SysWOW64");
        const bool system32 = vcDllPresent("C:\\Windows\\System32\\", "vcruntime140.dll") &&
                              vcDllPresent("C:\\Windows\\System32\\", "msvcp140.dll");
        if (!is64bitOs) return system32;
        const bool sysWow64 = vcDllPresent("C:\\Windows\\SysWOW64\\", "vcruntime140.dll") &&
                              vcDllPresent("C:\\Windows\\SysWOW64\\", "msvcp140.dll");
        return system32 && sysWow64;
    }

    // v1.2.2: at most one install run at a time - a detached phase-1 thread
    // and the phase-2 re-run would otherwise download/install to the same
    // target files (C:\Windows\VC_redist.*.exe) concurrently
    std::atomic<bool> g_vcInstallRunning{ false };
}

void Checks::installVCRedist()
{
    SetConsoleTitleA("Downloading VCRedist");

    if (g_vcInstallRunning.exchange(true)) {
        // another run is already working on it - wait for its result (bounded)
        for (int i = 0; i < 600 && !Helper::vcComplete; ++i) Sleep(100);
        if (Helper::vcComplete) return;   // the other run finished the job
        Helper::printConcern("- VCRedist install is already running elsewhere");
        Helper::recordResult("VC Redist", "WARN", "Concurrent install run still active");
        return;
    }
    struct VcRunningGuard {
        ~VcRunningGuard() { g_vcInstallRunning = false; }
    } vcGuard;

    Helper::vcComplete = false;

    // v1.2.2: both architectures per DLL (see vcRedistInstalled)
    bool alreadyInstalled = vcRedistInstalled();
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
    Helper::logAppliedChange("info:Microsoft Visual C++ Redistributable was installed - uninstall it via Apps & Features if unwanted");

    if (!vcRedistInstalled()) {
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

    // v1.2.1: know the boot mode first - legacy BIOS systems cannot have Secure Boot at all
    DWORD firmwareType = 0; // 0 = unknown, 1 = BIOS, 2 = UEFI
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (kernel32 != NULL) {
        typedef BOOL(WINAPI* GetFirmwareTypeFn)(DWORD*);
        GetFirmwareTypeFn getFirmwareType =
            reinterpret_cast<GetFirmwareTypeFn>(GetProcAddress(kernel32, "GetFirmwareType"));
        if (getFirmwareType != NULL) getFirmwareType(&firmwareType);
    }

    if (firmwareType == 1) {
        Helper::printSuccess("- Secure-Boot is not available (legacy BIOS boot)", false);
        Helper::recordResult("Secure Boot", "OK", "Legacy BIOS - not available");
        return;
    }

    DWORD secbootStatus = 0;
    bool stateRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", "UEFISecureBootEnabled", &secbootStatus);
    const char* stateSource = "registry";

    // v1.2.1 fix: the State key is missing on some machines (Windows originally
    // installed in legacy mode, later switched to UEFI, OEM images) - the registry
    // miss must not end the check, read the SecureBoot UEFI variable directly
    if (!stateRead && firmwareType == 2) {
        HANDLE hToken = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            TOKEN_PRIVILEGES tp = {};
            if (LookupPrivilegeValueA(NULL, SE_SYSTEM_ENVIRONMENT_NAME, &tp.Privileges[0].Luid)) {
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
            }
            CloseHandle(hToken);
        }
        // EFI_GLOBAL_VARIABLE: the SecureBoot variable is one byte, 1 = enabled
        // (the W-API takes the vendor GUID as a string, not as a GUID struct)
        unsigned char sbVar[4] = {};
        DWORD sbLen = GetFirmwareEnvironmentVariableW(L"SecureBoot",
            L"{8BE4DF61-93CA-11d2-AA0D-00E098032B8C}", sbVar, sizeof(sbVar));
        if (sbLen == 1) {
            secbootStatus = sbVar[0] ? 1 : 0;
            stateRead = true;
            stateSource = "firmware variable";
        }
        else {
            DWORD err = GetLastError();
            // no SecureBoot variable at all -> the firmware cannot enable it;
            // any OTHER error keeps the chain going (PowerShell fallback below)
            if (err == ERROR_ENVVAR_NOT_FOUND || err == ERROR_INVALID_FUNCTION) {
                Helper::printSuccess("- Secure-Boot is not available (firmware reports no Secure-Boot)", false);
                Helper::recordResult("Secure Boot", "OK", "Not supported by firmware");
                return;
            }
        }
    }

    // last resort: the PowerShell cmdlet reads the state via a different path
    if (!stateRead && firmwareType == 2) {
        std::string sbOut = Helper::runSystemCommandWithOutput(
            "powershell -NoProfile -Command \"Confirm-SecureBootUEFI\"", 20000);
        if (sbOut == "TIMEOUT") sbOut.clear();
        if (sbOut.find("True") != std::string::npos) {
            secbootStatus = 1; stateRead = true; stateSource = "Confirm-SecureBootUEFI";
        }
        else if (sbOut.find("False") != std::string::npos) {
            secbootStatus = 0; stateRead = true; stateSource = "Confirm-SecureBootUEFI";
        }
    }

    if (!stateRead) {
        if (firmwareType == 2) {
            Helper::printConcern("- UEFI boot detected but the Secure-Boot state could not be read");
            Helper::recordResult("Secure Boot", "WARN", "UEFI - state unreadable");
        }
        else {
            Helper::printError("- Unable to check Secure-Boot Status (Error 12)");
            Helper::recordResult("Secure Boot", "FAIL", "Unknown state");
        }
        return;
    }
    Helper::logWrite("  [i] Secure Boot state source: " + std::string(stateSource));

    if (secbootStatus == 0x00000000) {
        Helper::printSuccess("- Secure-Boot is disabled (UEFI)", false);
        Helper::recordResult("Secure Boot", "OK", "Disabled (UEFI)");
    }
    else if (secbootStatus == 0x00000001) {
        Helper::printError("- Secure-Boot is enabled, disable it in BIOS " + biosHint("secureboot"));
        Helper::recordResult("Secure Boot", "FAIL", "Enabled");
    }
    else {
        Helper::printError("- Unable to check Secure-Boot Status (Error 12, state=" +
            std::to_string(secbootStatus) + ")");
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
    SetConsoleTitleA("Checking Windows Time Sync");

    // v1.2.2: the old code requested "Repair and start Windows Time service"
    // whenever the service was not RUNNING at that moment - on machines with the
    // default manual/trigger start that produced a pending change on EVERY run.
    // Decide by START TYPE (registry, language-independent):
    //   disabled -> repair needs approval; manual/auto but stopped -> just start it.
    ServiceStatus currentStatus = Helper::getServiceStatus("W32Time");
    DWORD startType = 3;
    bool startKnown = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Services\\W32Time", "Start", &startType);
    if (startKnown && startType == 4) {
        // genuinely disabled - re-enable needs approval (Undo reverts the marker)
        SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (scmHandle == NULL) {
            DWORD err = GetLastError();
            Helper::printError("- Failed to sync Windows Time (Error 14, GLE=" + std::to_string(err) + ")");
            Helper::recordResult("Time Sync", "FAIL", "SCM open failed");
            return;
        }
        if (!Helper::requestChange("Enable and start Windows Time service")) {
            CloseServiceHandle(scmHandle);
            Helper::recordResult("Time Sync", "WARN", "Disabled - enable pending approval");
            return;
        }
        SC_HANDLE serviceHandle = OpenService(scmHandle, "W32Time", SERVICE_ALL_ACCESS);
        if (serviceHandle == NULL) {
            Helper::printError("- Failed to sync Windows Time (Error 15)");
            Helper::recordResult("Time Sync", "FAIL", "OpenService failed");
            CloseServiceHandle(scmHandle);
            return;
        }
        if (!ChangeServiceConfig(serviceHandle, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
            Helper::printError("- Failed to sync Windows Time (Error 18)");
            Helper::recordResult("Time Sync", "FAIL", "Start type change failed");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        // v1.2.2 bugfix: carry the ORIGINAL start type - Undo used to force
        // "demand" even when the start type was 4 (disabled) before this run
        Helper::logAppliedChange("service:W32Time=was" + std::to_string(startType));
        if (currentStatus == STATUS_SERVICE_STOPPED &&
            StartService(serviceHandle, 0, NULL) == FALSE && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            Helper::printError("- Failed to sync Windows Time (Error 19)");
            Helper::recordResult("Time Sync", "FAIL", "StartService failed");
            CloseServiceHandle(serviceHandle); CloseServiceHandle(scmHandle);
            return;
        }
        CloseServiceHandle(serviceHandle);
        CloseServiceHandle(scmHandle);
        Sleep(250);
        int rc = Helper::runSystemCommandExitCode("w32tm /resync");
        Helper::printSuccess(rc == 0 ? "- Windows Time enabled and synced" :
            "- Windows Time service enabled (resync will retry automatically)", true);
        Helper::recordResult("Time Sync", rc == 0 ? "OK" : "WARN",
            rc == 0 ? "Enabled + synced (changed)" : "Enabled, resync failed (changed)");
        return;
    }

    // start type manual/auto (Windows default is manual): if the service happens
    // to be stopped, start it WITHOUT asking - it is not a configuration change
    if (currentStatus != STATUS_SERVICE_RUNNING) {
        SC_HANDLE scmHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
        if (scmHandle != NULL) {
            SC_HANDLE serviceHandle = OpenService(scmHandle, "W32Time", SERVICE_START | SERVICE_QUERY_STATUS);
            if (serviceHandle != NULL) {
                StartService(serviceHandle, 0, NULL);
                CloseServiceHandle(serviceHandle);
            }
            CloseServiceHandle(scmHandle);
            Sleep(250);
        }
    }

    int rc = Helper::runSystemCommandExitCode("w32tm /resync");
    if (rc == 0) {
        Helper::printSuccess("- Windows Time is synced", false);
        Helper::recordResult("Time Sync", "OK", "Synced");
    }
    else {
        // service running but NTP unreachable: not a config problem - report, no pending
        Helper::printConcern("- Time service running but resync failed (exit " +
            std::to_string(rc) + ") - check internet/firewall");
        Helper::recordResult("Time Sync", "WARN", "Resync failed (exit " + std::to_string(rc) + ")");
    }
}

void Checks::disableChromeProtection()
{
    SetConsoleTitleA("Disabling Google Chrome Protection");

    // v1.2.2: Chrome's default install is per-user in %LOCALAPPDATA% - checking
    // only Program Files reported "Chrome not installed" on typical systems
    bool chromeInstalled = std::filesystem::exists(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe") ||
                           std::filesystem::exists(L"C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe");
    if (!chromeInstalled) {
        wchar_t localAppData[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
            chromeInstalled = std::filesystem::exists(
                std::wstring(localAppData) + L"\\Google\\Chrome\\Application\\chrome.exe");
        }
    }
    if (!chromeInstalled) {
        Helper::recordResult("Chrome Protection", "SKIPPED", "Chrome not installed");
        return;
    }

    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome");

    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        "SafeBrowsingProtectionLevel", &status)) {
        // v1.2.2 bugfix: 0 = NoProtection (off), 1 = Standard Protection (ON!) -
        // the check used to treat 1 as "already disabled" and wrote 1, so Safe
        // Browsing stayed enabled while the tool reported success
        if (status == 0x00000000) {
            Helper::printSuccess("- Chrome protection is already disabled", false);
            Helper::recordResult("Chrome Protection", "OK", "Already disabled");
            return;
        }
    }

    if (!Helper::requestChange("Disable Chrome Safe Browsing protection policy")) {
        Helper::recordResult("Chrome Protection", "WARN", "Enabled - policy change pending approval");
        return;
    }

    HKEY hKey = NULL; DWORD disp; DWORD value = 0x00000000; // v1.2.2: 0 = NoProtection (Chrome policy)
    LONG createKey = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Google\\Chrome",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    // v1.2.2 bugfix: policy value was 1 (= Standard Protection, i.e. STILL ON)
    LONG createDWORD = (createKey == ERROR_SUCCESS)
        ? RegSetValueEx(hKey, "SafeBrowsingProtectionLevel", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value))
        : createKey;
    if (createKey == ERROR_SUCCESS) RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Google\\Chrome");

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
    char buildStr[32] = {}; DWORD buildSize = sizeof(buildStr);
    LONG ret = RegQueryValueEx(hKey, "CurrentBuild", NULL, NULL, (LPBYTE)buildStr, &buildSize);
    RegCloseKey(hKey);
    buildStr[sizeof(buildStr) - 1] = '\0'; // v1.2.2: guarantee termination
    if (ret != ERROR_SUCCESS) {
        Helper::printError("- Failed to check Winver (Error 22)");
        Helper::recordResult("Winver", "FAIL", "Read failed");
        return;
    }
    // v1.2.2: validate before conversion - an unexpected registry value must not
    // throw std::out_of_range and wipe the whole run via an uncaught exception
    int build = 0;
    {
        bool digitsOnly = buildStr[0] != '\0';
        for (const char* p = buildStr; *p; ++p) {
            if (*p < '0' || *p > '9') { digitsOnly = false; break; }
        }
        if (!digitsOnly) {
            Helper::printError("- Failed to check Winver (unexpected CurrentBuild value)");
            Helper::recordResult("Winver", "FAIL", "Invalid CurrentBuild value");
            return;
        }
        build = std::atoi(buildStr);
    }

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
        {26100, "Win11 24H2"}, {26200, "Win11 25H2"},
        // v1.2.3: name the current releases instead of a bare "Build 26300" -
        // the 50/50 verdict itself stays (fiftyFiftyFrom below, user decision)
        {26300, "Win11 26H2"}, {28000, "Win11 26H1"},
    };
    int min_build = 19041;
    // v1.2.1: 22H2 (19045/22621) is supported, not a 50/50 (user decision) -
    // everything from 25H2 (26200) on is 50/50, 26H2 (26300) is not supported
    std::vector<int> trouble = {};

    // 25H2 and every build we do not know yet are 50/50: works for some users, breaks for others
    const int fiftyFiftyFrom = 26200;

    auto it = build_map.find(build);
    if (it != build_map.end()) { winver = it->second; }
    else if (build >= fiftyFiftyFrom) { winver = "Build " + std::to_string(build); }

    if (build < min_build) {
        Helper::printError("- Winver \"" + winver + "\" is unsupported, please upgrade");
        Helper::recordResult("Winver", "FAIL", "Unsupported: " + winver + " (Build " + std::to_string(build) + ")");
    }
    else if (build >= fiftyFiftyFrom ||
        std::find(trouble.begin(), trouble.end(), build) != trouble.end()) {
        // v1.2.4: 26H2 (26300) is explicitly not supported (user decision) -
        // wording only, the verdict stays WARN; 25H2 and unknown future
        // builds keep the 50/50 text
        if (build == 26300) {
            Helper::printConcern("- Winver \"" + winver + "\" (not supported)");
            Helper::recordResult("Winver", "WARN", winver + " (not supported)");
        }
        else {
            Helper::printConcern("- Winver \"" + winver + "\" is a 50/50, if error contact support");
            Helper::recordResult("Winver", "WARN", winver + " is a 50/50, if error contact support");
        }
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
        std::filesystem::remove_all(path, ec);
        // v1.2.2: remove_all returns the number of REMOVED entries - a partial
        // delete (files gone, root locked) still returned > 0 and was reported
        // as success. Verify the directory is actually gone.
        std::error_code existsEc;
        if (std::filesystem::exists(path, existsEc)) {
            std::string detail = ec ? ec.message() : "still present after delete";
            Helper::printError("- Unable to delete " + path + " (Error 24, " + detail + ")");
            Helper::recordResult("Symbols", "FAIL", "Delete failed: " + detail);
            return;
        }
        Helper::printSuccess("- Successfully deleted " + path, true);
        Helper::logAppliedChange("info:C:\\Symbols was deleted and cannot be restored automatically");
        Helper::recordResult("Symbols", "OK", "Deleted (changed)");
        return;
    }
    Helper::printSuccess("- C:\\Symbols does not exist", false);
    Helper::recordResult("Symbols", "OK", "Not present");
}

void Checks::checkFastBoot()
{
    SetConsoleTitleA("Checking Fast-Boot");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power");
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

    HKEY hKey = NULL; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    // v1.2.2: never touch/close an uninitialized handle when Create failed
    LONG cd = (ck == ERROR_SUCCESS)
        ? RegSetValueEx(hKey, "HiberbootEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value))
        : ck;
    if (ck == ERROR_SUCCESS) RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power");
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
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection");
    DWORD status;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        "DisallowExploitProtectionOverride", &status)) {
        // v1.2.2 bugfix: this policy only LOCKS the UI (users may not change the
        // settings) - it does NOT disable the protections themselves. The old
        // "is disabled" wording claimed the opposite of what the value does.
        if (status == 1) {
            // v1.2.3: no console line for the OK state - "locked by policy"
            // was screenshot noise nobody acts on; log/JSON/summary keep it
            Helper::recordResult("Exploit Protection", "OK", "Override locked by policy");
            return;
        }
    }
    if (!Helper::requestChange("Lock Exploit-Protection settings (DisallowExploitProtectionOverride)")) {
        Helper::recordResult("Exploit Protection", "WARN", "Not locked - lock pending approval");
        return;
    }

    HKEY hKey = NULL; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = (ck == ERROR_SUCCESS)
        ? RegSetValueEx(hKey, "DisallowExploitProtectionOverride", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value))
        : ck;
    if (ck == ERROR_SUCCESS) RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows Defender Security Center\\App and Browser protection");
    if (ck == ERROR_SUCCESS && cd == ERROR_SUCCESS) {
        Helper::printSuccess("- Locked Exploit-Protection settings by policy (protections stay active)", true);
        Helper::restartRequired = true;
        Helper::recordResult("Exploit Protection", "OK", "Override locked (changed)");
    }
    else {
        Helper::printError("- Failed to lock Exploit-Protection settings (Error 25/26)");
        Helper::recordResult("Exploit Protection", "FAIL", "Registry write failed");
    }
}

void Checks::checkSmartScreen()
{
    SetConsoleTitleA("Checking SmartScreen");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System");
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

    HKEY hKey = NULL; DWORD disp; DWORD value = 0;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Policies\\Microsoft\\Windows\\System",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = (ck == ERROR_SUCCESS)
        ? RegSetValueEx(hKey, "EnableSmartScreen", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value))
        : ck;
    if (ck == ERROR_SUCCESS) RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Policies\\Microsoft\\Windows\\System");
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
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR");
    // v1.2.2: a missing value is the Windows default (Game Bar capture is ON out
    // of the box) - only an explicit 0 means "disabled, enable pending", the old
    // code raised a pending change on every Windows 11 default install
    DWORD status = 1;
    Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR", "AppCaptureEnabled", &status);
    if (status == 1) {
        Helper::printSuccess("- Gamebar is enabled", false);
        Helper::recordResult("Game Bar", "OK", "Already enabled");
        return;
    }
    if (!Helper::requestChange("Enable Xbox Game Bar capture setting")) {
        Helper::recordResult("Game Bar", "WARN", "Disabled - enable pending approval");
        return;
    }

    HKEY hKey = NULL; DWORD disp; DWORD value = 1;
    LONG ck = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
    LONG cd = (ck == ERROR_SUCCESS)
        ? RegSetValueEx(hKey, "AppCaptureEnabled", NULL, REG_DWORD, (const BYTE*)&value, sizeof(value))
        : ck;
    if (ck == ERROR_SUCCESS) RegCloseKey(hKey);
    Helper::logAppliedChange("reg:SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\GameDVR");
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
            if (svc == NULL) {
                // v1.2.2 bugfix: only a genuinely MISSING service indicates a
                // modified OS - access denied / transient SCM errors must not
                // flag the system as modified
                if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
                    modified = true; reasons += std::string(" ") + label + " missing;";
                }
            }
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

    const char* tpmKey = "SYSTEM\\CurrentControlSet\\Control\\TPM";

    // v1.2.1: read every state value Windows exposes instead of the single TPMActive entry
    DWORD present = 0, enabled = 0, activated = 0, ready = 0, tpmActive = 0;
    bool presentRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Present", &present);
    bool enabledRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Enabled", &enabled);
    bool activatedRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Activated", &activated);
    bool readyRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "Ready", &ready);
    bool activeRead = Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE, tpmKey, "TPMActive", &tpmActive);

    std::string specVersion = readRegistryStringValue(HKEY_LOCAL_MACHINE, tpmKey, "SpecVersion");
    std::string manufacturer = readRegistryStringValue(HKEY_LOCAL_MACHINE, tpmKey, "ManufacturerIdTxt");

    std::string detail;
    if (!specVersion.empty()) detail += " (" + specVersion;
    if (!manufacturer.empty()) detail += detail.empty() ? " (" + manufacturer : ", " + manufacturer;
    if (!detail.empty()) detail += ")";

    bool registryUsable = presentRead || enabledRead || activatedRead || readyRead || activeRead;
    if (registryUsable) {
        bool tpmPresent = (presentRead && present == 1) || (activeRead && tpmActive == 1);
        bool tpmRunning = (enabledRead && enabled == 1) ||
                          (activatedRead && activated == 1) ||
                          (readyRead && ready == 1);

        if (tpmPresent && tpmRunning) {
            Helper::printError("- TPM is enabled" + detail + ", disable it in BIOS " + biosHint("tpm"));
            Helper::recordResult("TPM", "FAIL", "Enabled" + detail);
        }
        else if (tpmPresent) {
            Helper::printSuccess("- TPM present but not usable (disabled in BIOS)" + detail, false);
            Helper::recordResult("TPM", "OK", "Present but not usable" + detail);
        }
        else {
            Helper::printSuccess("- TPM is disabled / not found" + detail, false);
            Helper::recordResult("TPM", "OK", "Disabled/not found" + detail);
        }
        return;
    }

    // No registry state: ask the TPM WMI provider (same source as tpm.msc / Get-Tpm)
    // v1.2.2: the query emits explicit sentinels so three cases can be told
    // apart: real data ("spec|enabled|activated"), NOTPM (Windows reports no
    // TPM instance - the normal answer on machines without a TPM / with it
    // disabled in BIOS = OK) and TPMERR (the query itself failed: access
    // denied, WMI blocked, PowerShell missing). The old code saw an empty
    // string for both NOTPM and TPMERR and warned on every TPM-less machine.
    std::string tpmCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "try { $t = Get-CimInstance -Namespace root/cimv2/security/microsofttpm -ClassName Win32_Tpm -ErrorAction Stop | Select-Object -First 1; "
        "if ($t) { $t.SpecVersion + '|' + $t.IsEnabled_InitialValue + '|' + $t.IsActivated_InitialValue } else { 'NOTPM' } } "
        "catch { 'TPMERR' }\" 2>nul";
    std::string output = trimCopy(Helper::runSystemCommandWithOutput(tpmCommand.c_str(), 20000));
    if (output == "TIMEOUT") output.clear();

    if (output.find("NOTPM") != std::string::npos) {
        Helper::printSuccess("- TPM is disabled / not usable (no TPM reported by Windows)", false);
        Helper::recordResult("TPM", "OK", "Disabled/not usable");
        return;
    }
    if (output.find("TPMERR") != std::string::npos || output.empty()) {
        Helper::printConcern("- Could not determine TPM state (query failed)");
        Helper::recordResult("TPM", "WARN", "State unknown (query failed)");
        return;
    }

    size_t firstSep = output.find('|');
    if (firstSep == std::string::npos) {
        // non-empty but not a sentinel and no separator: PowerShell noise
        Helper::printConcern("- Could not determine TPM state (unexpected query output)");
        Helper::recordResult("TPM", "WARN", "State unknown (unexpected output)");
        return;
    }

    size_t secondSep = output.find('|', firstSep + 1);
    std::string spec = trimCopy(output.substr(0, firstSep));
    std::string isEnabled = toLowerCopy(trimCopy(output.substr(firstSep + 1,
        secondSep == std::string::npos ? std::string::npos : secondSep - firstSep - 1)));
    std::string isActivated = secondSep == std::string::npos ? "" :
        toLowerCopy(trimCopy(output.substr(secondSep + 1)));

    bool tpmEnabledFlag = (isEnabled == "true");
    bool tpmActivatedFlag = (isActivated == "true");
    std::string wmiDetail = spec.empty() ? "" : " (" + spec + ")";

    if (tpmEnabledFlag && tpmActivatedFlag) {
        Helper::printError("- TPM is enabled" + wmiDetail + ", disable it in BIOS " + biosHint("tpm"));
        Helper::recordResult("TPM", "FAIL", "Enabled" + wmiDetail);
    }
    else if (tpmEnabledFlag || tpmActivatedFlag) {
        Helper::printConcern("- TPM is partially available (Enabled=" + isEnabled +
            ", Activated=" + isActivated + ")" + wmiDetail + " " + biosHint("tpm"));
        Helper::recordResult("TPM", "WARN", "Partially available" + wmiDetail);
    }
    else {
        Helper::printSuccess("- TPM is disabled / not usable" + wmiDetail, false);
        Helper::recordResult("TPM", "OK", "Disabled/not usable" + wmiDetail);
    }
}

void Checks::checkCoreIsolation()
{
    SetConsoleTitleA("Checking Core Isolation (VBS/HVCI)");

    // Runs before checkSecurityMitigations, so these are the pre-change exports
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
    Helper::backupRegistryKey(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");

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

    // v1.2.2: register BOTH pendings before returning. The old early return after the
    // first unapproved change silently dropped the VBS pending from the approval list.
    bool hvciApproved = (hvci != 1) || Helper::requestChange("Disable Core Isolation HVCI");
    bool vbsApproved = (vbs != 1) || Helper::requestChange("Disable Virtualization Based Security");
    if (!hvciApproved || !vbsApproved) {
        std::string pending;
        if (hvci == 1) pending += "HVCI";
        if (vbs == 1) {
            if (!pending.empty()) pending += " + ";
            pending += "VBS";
        }
        Helper::recordResult("Core Isolation", "WARN", pending + " enabled - disable pending approval");
        return;
    }

    bool changed = false;
    std::string changedList;
    std::string failedList;
    HKEY hKey;

    auto appendName = [](std::string& list, const char* name) {
        if (!list.empty()) list += " + ";
        list += name;
    };

    if (hvci == 1) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "Enabled", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity");
                Helper::printSuccess("- Disabled HVCI", true);
                appendName(changedList, "HVCI");
                changed = true;
            }
            else {
                Helper::printError("- Failed to disable HVCI (access denied?)");
                Helper::printConcern("  Disable manually: Windows Security > Device Security > Core Isolation");
                appendName(failedList, "HVCI");
            }
            RegCloseKey(hKey);
        }
        else {
            appendName(failedList, "HVCI");
        }
    }

    if (vbs == 1) {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\DeviceGuard",
            0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            DWORD val = 0;
            if (RegSetValueEx(hKey, "EnableVirtualizationBasedSecurity", 0, REG_DWORD, (BYTE*)&val, sizeof(val)) == ERROR_SUCCESS) {
                Helper::logAppliedChange("reg:SYSTEM\\CurrentControlSet\\Control\\DeviceGuard");
                if (!changed) Helper::printSuccess("- Disabled VBS", true);
                appendName(changedList, "VBS");
                changed = true;
            }
            else {
                appendName(failedList, "VBS");
            }
            RegCloseKey(hKey);
        }
        else {
            appendName(failedList, "VBS");
        }
    }

    if (changed) {
        Helper::restartRequired = true;
    }

    // v1.2.2: exactly one result per check (upsert keeps the last one anyway)
    if (changed && failedList.empty()) {
        Helper::recordResult("Core Isolation", "OK", changedList + " disabled (changed)");
    }
    else if (changed) {
        Helper::recordResult("Core Isolation", "WARN",
            changedList + " disabled (changed); failed: " + failedList);
    }
    else if (!failedList.empty()) {
        Helper::printConcern("- Could not disable Core Isolation, disable manually in Windows Security");
        Helper::recordResult("Core Isolation", "FAIL",
            "Manual disable needed (" + failedList + " write failed)");
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

    // v1.2.3: read the actual state the way msinfo32 does -
    // NtQuerySystemInformation(SystemDmaGuardPolicyInformation = 202) returns
    // the DmaGuard on/off flag as a single BOOLEAN (the query the
    // Harden-Windows-Security compliance module uses to mirror the
    // "Kernel DMA Protection" field of msinfo32). Ends the old
    // "state unknown - verify in msinfo32" WARN.
    //   1 = protection enabled -> FAIL (same verdict as the registry branches)
    //   0 = system reports Off -> OK/WARN (support flag consulted below)
    //   query failed           -> WMI-only logic below (old behaviour)
    typedef LONG(NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);
    static NtQuerySystemInformation_t ntQuerySystemInformation =
        reinterpret_cast<NtQuerySystemInformation_t>(
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation"));
    // -1 = query failed (old WMI-only behaviour below), 0 = Off, 1 = enabled
    int dmaOn = -1;
    if (ntQuerySystemInformation) {
        // this class wants a buffer of exactly 1 byte (larger buffers fail
        // with STATUS_INFO_LENGTH_MISMATCH - verified on Win11)
        UCHAR stateByte = 0;
        ULONG retLen = 0;
        if (ntQuerySystemInformation(202, &stateByte, 1, &retLen) == 0 /* STATUS_SUCCESS */)
            dmaOn = stateByte ? 1 : 0;
    }
    if (dmaOn == 1) {
        Helper::printError("- Kernel DMA Protection is enabled");
        Helper::recordResult("DMA Protection", "FAIL", "Enabled (state read directly)");
        return;
    }

    // v1.2.1: registry has no state -> ask Win32_DeviceGuard (the source msinfo32 uses)
    std::string deviceGuardCommand =
        "powershell -NoProfile -NonInteractive -Command \""
        "(Get-CimInstance -Namespace root/Microsoft/Windows/DeviceGuard -ClassName Win32_DeviceGuard)"
        ".AvailableSecurityProperties -join ','\" 2>nul";
    std::string available = trimCopy(Helper::runSystemCommandWithOutput(deviceGuardCommand.c_str(), 20000));
    if (available == "TIMEOUT") available.clear();

    // v1.2.2 bugfix: AvailableSecurityProperties=3 means DMA protection is
    // SUPPORTED by the firmware, not that it is off (msinfo32 shows On/Off and
    // supporting systems enable it automatically) - claiming "currently off"
    // here could never fail. Without a readable state the answer is unknown.
    if (available.empty()) {
        if (dmaOn == 0) {
            // state read says Off even though the support flag is unavailable
            Helper::printSuccess("- Kernel DMA Protection is disabled (system reports Off)", false);
            Helper::recordResult("DMA Protection", "OK", "Disabled (state read directly)");
            return;
        }
        Helper::printConcern("- Could not determine Kernel DMA Protection state (WMI query failed)");
        Helper::recordResult("DMA Protection", "WARN", "State unknown (WMI query failed)");
    }
    else if (csvContains(available, 3)) {
        if (dmaOn == 0) {
            // supported by firmware but the system reports Off (typically
            // IOMMU/VT-d disabled in BIOS) - name it instead of "unknown"
            Helper::printConcern("- Kernel DMA Protection is supported by firmware but reports Off - verify state in msinfo32");
            Helper::recordResult("DMA Protection", "WARN", "Supported by firmware, reports Off");
        }
        else {
            Helper::printConcern("- Kernel DMA Protection is supported by firmware - verify state in msinfo32");
            Helper::recordResult("DMA Protection", "WARN", "Supported by firmware, state unknown");
        }
    }
    else {
        Helper::printSuccess("- Kernel DMA Protection is not supported / not active", false);
        Helper::recordResult("DMA Protection", "OK", "Not supported");
    }
}

void Checks::checkUAC()
{
    SetConsoleTitleA("Checking UAC");
    DWORD uacEnabled = 0;
    if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        "EnableLUA", &uacEnabled)) {
        // v1.2.2: the slider at "Never notify" keeps EnableLUA=1 and only sets
        // ConsentPromptBehaviorAdmin=0 - checking EnableLUA alone reported
        // "UAC is enabled" on machines where the user already turned it off
        DWORD consent = 5;
        Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            "ConsentPromptBehaviorAdmin", &consent);

        if (uacEnabled == 0) {
            // v1.2.2: OK states stay off the console - only actionable findings print
            Helper::recordResult("UAC", "OK", "Disabled");
        }
        else if (consent == 0) {
            Helper::recordResult("UAC", "OK", "Never notify (slider off)");
        }
        else {
            // v1.2.2: no console line either - log/JSON only (status INFO is
            // not counted as warning in the summary)
            Helper::recordResult("UAC", "INFO", "Enabled - consider disabling");
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

    // v1.2.2: judge by the START TYPE (registry, language-independent), not by
    // "is the service running right now". A running service with the default
    // manual/trigger start is normal Windows behaviour and used to raise a
    // warning on every machine ("why does it suddenly mention Windows Update").
    DWORD startType = 3; // SERVICE_AUTO_START / SERVICE_DELAYED_AUTO_START / SERVICE_DISABLED
    if (!Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Services\\wuauserv", "Start", &startType)) {
        // log/JSON only - no console noise
        Helper::recordResult("Windows Update", "SKIPPED", "Start type unreadable");
        return;
    }

    // 2 = auto, 3 = demand (Windows default), 4 = disabled, 0/1 = boot/system (never used here)
    if (startType == 4) {
        // v1.2.2: OK states stay off the console - only actionable findings print
        Helper::recordResult("Windows Update", "OK", "Disabled");
    }
    else if (startType == 2 || startType == 1 || startType == 0) {
        // v1.2.2: no console line either - log/JSON only (status INFO is
        // not counted as warning in the summary)
        Helper::recordResult("Windows Update", "INFO", "Start type: automatic");
    }
    else {
        Helper::recordResult("Windows Update", "OK", "Start type: manual (default)");
    }
}

// v1.2.1: CPUID + firmware-feature based virtualization check (no WMIC dependency)
void Checks::checkCPUVirtualization()
{
    SetConsoleTitleA("Checking CPU Virtualization");

    // CPUID: VT-x (Intel, leaf 1 ECX bit 5) / SVM (AMD, ext leaf 0x80000001 ECX bit 2)
    int regs[4] = { 0, 0, 0, 0 };
    bool vmx = false;
    bool svm = false;

    __cpuid(regs, 0);
    unsigned maxLeaf = static_cast<unsigned>(regs[0]);
    if (maxLeaf >= 1) {
        __cpuid(regs, 1);
        vmx = (regs[2] & (1 << 5)) != 0;
    }

    __cpuid(regs, static_cast<int>(0x80000000u));
    unsigned maxExtLeaf = static_cast<unsigned>(regs[0]);
    if (maxExtLeaf >= 0x80000001u) {
        __cpuid(regs, static_cast<int>(0x80000001u));
        svm = (regs[2] & (1 << 2)) != 0;
    }

    const bool hwVirtualization = vmx || svm;

    // v1.2.2 fix: the old code passed 23 (= PF_FASTFAIL_AVAILABLE, almost always
    // true) and 40 (= PF_AVX2_INSTRUCTIONS_AVAILABLE) - both wrong, which made
    // every machine report "CPU-V enabled in BIOS" even with virtualization off.
    // PF_VIRT_FIRMWARE_ENABLED is 21 per winnt.h / Microsoft docs.
    const bool firmwareEnabledByOs = IsProcessorFeaturePresent(21) != 0;
    // Hypervisor running: CPUID leaf 1 ECX bit 31 (official hypervisor-present bit)
    bool hypervisorPresent = false;
    __cpuid(regs, 1);
    hypervisorPresent = (regs[2] & (1u << 31)) != 0;
    if (!hypervisorPresent) {
        // Registry fallback (written by the kernel when the hypervisor boots)
        DWORD hvReg = 0;
        if (Helper::readDwordValueRegistry(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization",
            "HypervisorPresent", &hvReg)) {
            hypervisorPresent = hvReg != 0;
        }
    }
    // A running hypervisor cannot exist without firmware virtualization being on
    // (Windows hides/masks the PF_VIRT_FIRMWARE_ENABLED flag once Hyper-V runs),
    // so treat "hypervisor active" as firmware-enabled.
    const bool firmwareEnabled = firmwareEnabledByOs || hypervisorPresent;

    if (!hwVirtualization) {
        // CPUID virtualization bits also disappear when the BIOS switch is off
        Helper::printSuccess("- CPU does not expose hardware virtualization (unsupported or disabled in BIOS)", false);
        Helper::recordResult("CPU-V", "OK", "Not exposed (unsupported or disabled in BIOS)");
        return;
    }

    const std::string tech = vmx ? "VT-x" : "SVM";
    if (firmwareEnabled) {
        std::string detail = "Enabled in BIOS (" + tech + ")";
        if (hypervisorPresent) detail += ", hypervisor active";
        Helper::printConcern("- CPU-V is enabled in BIOS (" + tech + "), disable it in BIOS if not needed " +
            biosHint("cpuv", tech == "SVM"));
        Helper::recordResult("CPU-V", "WARN", detail);
    }
    else {
        std::string detail = "Disabled in BIOS (" + tech + ")";
        if (hypervisorPresent) detail += ", hypervisor active";
        Helper::printSuccess("- CPU virtualization disabled in BIOS (" + tech + ")", false);
        Helper::recordResult("CPU-V", "OK", detail);
    }
}

// v1.2.1: raw SMBIOS identity scoring + registry cross-check to spot perm HWID spoofers
void Checks::checkSpoofer()
{
    SetConsoleTitleA("Checking for HWID Spoofer");

    SmbiosIdentity id = readSmbiosIdentity();
    if (!id.valid) {
        Helper::printConcern("- Could not read SMBIOS identity");
        Helper::recordResult("HWID Spoofer", "WARN", "SMBIOS unreadable");
        return;
    }

    struct Field { const char* name; const std::string* value; bool identityCore; };
    const Field fields[] = {
        { "System Manufacturer", &id.systemManufacturer, true },
        { "System Product",      &id.systemProduct,      true },
        { "System Version",      &id.systemVersion,      false },
        { "System Serial",       &id.systemSerial,       false },
        { "Board Manufacturer",  &id.boardManufacturer,  true },
        { "Board Product",       &id.boardProduct,       true },
        { "Board Version",       &id.boardVersion,       false },
        { "Board Serial",        &id.boardSerial,        false },
    };

    int strongHits = 0;
    int weakHits = 0;
    std::string strongList;
    std::string weakList;

    auto addHit = [](std::string& list, const std::string& entry) {
        if (!list.empty()) list += "; ";
        list += entry;
    };

    for (const Field& field : fields) {
        bool wasStrong = false;
        const int score = identityScore(*field.value, &wasStrong);
        if (score == 2) {
            // Hex/synthetic values are suspicious in ANY field
            strongHits++;
            addHit(strongList, std::string(field.name) + "=" + *field.value);
        }
        else if (score == 1 && field.identityCore) {
            // Placeholders/random tokens only count in manufacturer/product:
            // "To be filled by O.E.M." in serial/version is normal OEM sloppiness
            weakHits++;
            addHit(weakList, std::string(field.name) + "=" + *field.value);
        }
    }

    if (id.uuidNil) {
        weakHits++;
        addHit(weakList, "SMBIOS UUID is empty");
    }
    // v1.2.2: all-same-byte UUID (11111111-...) is synthetic, never OEM
    if (id.uuidSameByte) {
        strongHits++;
        addHit(strongList, "SMBIOS UUID is a repeated-byte pattern (" + id.uuidText + ")");
    }

    // Odd SMBIOS version (real firmware reports 2.x or 3.x)
    if (id.smbiosMajor < 2 || id.smbiosMajor > 3) {
        weakHits++;
        addHit(weakList, "SMBIOS version " + std::to_string(id.smbiosMajor) + "." +
            std::to_string(id.smbiosMinor));
    }

    // Cross-check against the registry copy written at boot: a spoofer that
    // patches the SMBIOS view but not this key (or vice versa) shows up here
    struct RegPair { const char* regName; const std::string* smbiosValue; };
    const RegPair regPairs[] = {
        { "SystemManufacturer",     &id.systemManufacturer },
        { "SystemProductName",      &id.systemProduct },
        { "BaseBoardManufacturer",  &id.boardManufacturer },
        { "BaseBoardProduct",       &id.boardProduct },
    };
    for (const RegPair& pair : regPairs) {
        const std::string regValue = readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", pair.regName);
        if (identityDiffers(*pair.smbiosValue, regValue)) {
            strongHits++;
            addHit(strongList, std::string(pair.regName) + " differs (SMBIOS=" +
                *pair.smbiosValue + ", registry=" + regValue + ")");
        }
    }

    // v1.2.2: firmware layer cross-check (Type 0 vs registry) - only WEAK because
    // some OEMs format the registry copy differently from the SMBIOS string
    const RegPair biosPairs[] = {
        { "BIOSVendor",  &id.biosVendor },
        { "BIOSVersion", &id.biosVersion },
    };
    for (const RegPair& pair : biosPairs) {
        const std::string regValue = readRegistryStringValue(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\BIOS", pair.regName);
        if (identityDiffers(*pair.smbiosValue, regValue)) {
            weakHits++;
            addHit(weakList, std::string(pair.regName) + " differs (SMBIOS=" +
                *pair.smbiosValue + ", registry=" + regValue + ")");
        }
    }

    if (strongHits >= 2) {
        Helper::printError("- Likely HWID spoofer detected (" + std::to_string(strongHits) +
            " strong indicators): " + strongList);
        Helper::recordResult("HWID Spoofer", "FAIL",
            std::to_string(strongHits) + " strong indicators: " + strongList);
    }
    else if (strongHits == 1) {
        Helper::printConcern("- Suspicious identity value: " + strongList);
        // v1.2.3: summary carries the same wording as the console line -
        // "HWID Spoofer: Board Serial=..." alone read like a verdict
        Helper::recordResult("HWID Spoofer", "WARN", "Suspicious identity value: " + strongList);
    }
    else if (weakHits >= 3) {
        // v1.2.2 bugfix: two WEAK hits were too easy to reach on stock
        // hardware (nil SMBIOS UUID + a model-number-like board product such
        // as "H610M-K" already counted as 2) - require three independent
        // weak indicators before suggesting tampering
        Helper::printConcern("- Possible identity tampering: " + weakList);
        Helper::recordResult("HWID Spoofer", "WARN", "Possible identity tampering: " + weakList);
    }
    else {
        Helper::printSuccess("- No spoofer indicators (SMBIOS + registry consistent)", false);
        Helper::recordResult("HWID Spoofer", "OK", "Identity looks consistent");
    }
}

namespace {
    // v1.2.1: small helper for the watermark hide - writes one REG_DWORD,
    // creating the key when needed; returns the Reg* result code
    LONG writeDwordValue(HKEY parent, const char* subkey, const char* valueName, DWORD value)
    {
        HKEY hKey = NULL; DWORD disp = 0;
        LONG rc = RegCreateKeyEx(parent, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_WRITE, NULL, &hKey, &disp);
        if (rc != ERROR_SUCCESS) return rc;
        rc = RegSetValueEx(hKey, valueName, NULL, REG_DWORD,
            (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
        return rc;
    }

    // v1.2.1: hide the visible activation watermark (COSMETIC ONLY - this does
    // NOT activate Windows, no key is installed, the license state is never
    // touched). Both keys are exported before the write so Undo-FBI-Setup.ps1
    // can restore them. Returns true when the change was applied; when it is
    // only pending approval or the write failed the result is recorded here.
    bool offerActivationWatermarkHide(const std::string& stateMsg)
    {
        if (!Helper::requestChange("Hide the activation watermark (cosmetic - does not activate Windows)")) {
            Helper::recordResult("Activation", "WARN", stateMsg + " - watermark hide pending approval");
            return false;
        }

        const char* actKey =
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SoftwareProtectionPlatform\\Activation";
        Helper::backupRegistryKey(HKEY_LOCAL_MACHINE, actKey);
        Helper::backupRegistryKey(HKEY_CURRENT_USER, "Control Panel\\Desktop");

        // NotificationDisabled=1 stops the notification-mode watermark, and
        // PaintDesktopVersion=0 hides the desktop watermark layer - both need
        // a restart to take effect (restartRequired is set below)
        LONG rc1 = writeDwordValue(HKEY_LOCAL_MACHINE, actKey, "NotificationDisabled", 1);
        LONG rc2 = writeDwordValue(HKEY_CURRENT_USER, "Control Panel\\Desktop",
            "PaintDesktopVersion", 0);
        if (rc1 == ERROR_SUCCESS && rc2 == ERROR_SUCCESS) {
            Helper::logAppliedChange("reg:" + std::string(actKey));
            Helper::logAppliedChange("reg:Control Panel\\Desktop");
            Helper::restartRequired = true;
            Helper::printSuccess("- Activation watermark hidden (restart required to apply)", true);
            Helper::recordResult("Activation", "OK", stateMsg + " - watermark hidden (cosmetic)");
            return true;
        }
        Helper::printError("- Failed to hide the activation watermark");
        Helper::recordResult("Activation", "FAIL", stateMsg + " - watermark hide failed");
        return false;
    }
}

// v1.2.2: reports the state, never activates anything (no key installation).
// v1.2.1: the license state itself stays untouched, but the VISIBLE watermark
// can now be hidden on approval - cosmetic only, restart required
void Checks::checkActivation()
{
    SetConsoleTitleA("Checking Activation Status");

    // v1.2.2: filter by the fixed Windows ApplicationId GUID instead of
    // Name -like 'Windows*' (product names are localizable); keep the name
    // filter as a fallback for odd systems
    std::string command =
        "powershell -NoProfile -NonInteractive -Command \""
        "$a = '55c92734-d682-4d71-983e-d6ec3f16059f'; "
        "$p = Get-CimInstance -ClassName SoftwareLicensingProduct | "
        "Where-Object { $_.PartialProductKey -and ($_.ApplicationId -eq $a -or $_.Name -like 'Windows*') } | "
        "Select-Object -First 1; "
        "if ($p) { $p.LicenseStatus } else { 'NA' }\" 2>nul";
    std::string out = trimCopy(Helper::runSystemCommandWithOutput(command.c_str(), 30000));
    if (out == "TIMEOUT") out.clear();

    // first token: a LicenseStatus digit, "NA", or PowerShell noise on failure
    std::string token = out.substr(0, out.find_first_of("\r\n \t"));
    bool numeric = !token.empty() &&
        token.find_first_not_of("0123456789") == std::string::npos;

    if (token.empty() || token == "NA" || !numeric) {
        // v1.2.2: no console warning for unreadable status - the tool never
        // activates anything anyway, so a failed read is not actionable
        Helper::recordResult("Activation", "SKIPPED",
            token.empty() ? "Status unreadable" :
            (token == "NA" ? "No Windows product key found" : "Unexpected PowerShell output"));
        return;
    }

    // SoftwareLicensingProduct.LicenseStatus
    // v1.2.2: activated states (1/2/3) stay OFF the console - nobody needs
    // "Windows is activated" in a support screenshot. The result is still
    // recorded (counted as passed, written to log/JSON); only PROBLEMS
    // (not activated, grace, watermark) print a line.
    switch (atoi(token.c_str())) {
    case 1:
        Helper::recordResult("Activation", "OK", "Activated (licensed)");
        break;
    case 2:
        Helper::recordResult("Activation", "OK", "Activated (OOB grace)");
        break;
    case 3:
        Helper::recordResult("Activation", "OK", "Activated (OOT grace)");
        break;
    case 0:
        Helper::printConcern("- Windows is not activated - activate with a valid license key "
            "(Settings > System > Activation)");
        offerActivationWatermarkHide("Not activated");
        break;
    case 4:
        Helper::printConcern("- Windows reports a non-genuine grace period");
        offerActivationWatermarkHide("Non-genuine grace period");
        break;
    case 5:
        Helper::printConcern("- Windows is in notification mode - the activation watermark "
            "can be visible until a valid license key is entered");
        offerActivationWatermarkHide("Notification mode");
        break;
    case 6:
        Helper::printConcern("- Windows is in extended grace period");
        offerActivationWatermarkHide("Extended grace period");
        break;
    default:
        Helper::printConcern("- Unknown activation state (LicenseStatus=" + token + ")");
        // unknown state - no watermark verdict, keep it read-only
        Helper::recordResult("Activation", "WARN", "Unknown LicenseStatus " + token);
        break;
    }
}

void Helper::setupConsole()
{
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_EXTENDED_FLAGS));

    // v1.2.2: enable ANSI/VT color sequences explicitly - consoles that never
    // switch them on print raw escape codes like "[38;2;255;255;255m"
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outMode = 0;
    if (hStdout != NULL && hStdout != INVALID_HANDLE_VALUE &&
        GetConsoleMode(hStdout, &outMode)) {
        SetConsoleMode(hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    SetConsoleTitleA("Initializing");

    // v1.2.1: start maximized so every check line is visible without scrolling
    HWND console = GetConsoleWindow();
    if (console != NULL) {
        ShowWindow(console, SW_MAXIMIZE);
    }
}

void Helper::printSuccess(const std::string& message, bool changed)
{
    if (cliConfig.quiet) return;
    std::lock_guard<std::mutex> lock(consoleMutex);
    // v1.2.2: print each line at most once per run (apply phase re-runs checks)
    if (!g_printedLines.insert(std::string("[+]") + message + (changed ? "!" : "")).second) return;
    Color::setForegroundColor(Color::Green);
    std::cout << "[+] ";
    Color::setForegroundColor(Color::White);
    std::cout << message;
    if (changed) { Color::setForegroundColor(Color::Yellow); std::cout << " (CHANGED)"; }
    std::cout << std::endl;
}

void Helper::printConcern(const std::string& message)
{
    // v1.2.2 bugfix: --quiet promised "only show errors and warnings" but this
    // returned early like the others - quiet now shows concerns and errors
    std::lock_guard<std::mutex> lock(consoleMutex);
    if (!g_printedLines.insert(std::string("[-]") + message).second) return;
    Color::setForegroundColor(Color::Yellow);
    std::cout << "[-] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

void Helper::printError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(consoleMutex); // v1.2.2: visible in --quiet too
    if (!g_printedLines.insert(std::string("[X]") + message).second) return;
    Color::setForegroundColor(Color::Red);
    std::cout << "[X] ";
    Color::setForegroundColor(Color::White);
    std::cout << message << std::endl;
}

// v1.2.4: adapter line with an orange kick-risk suffix for flagged
// (VPN/virtual) adapters; --quiet keeps only the warning itself
void Helper::printAdapterLine(const std::string& message, bool used, const std::string& orangeSuffix)
{
    std::lock_guard<std::mutex> lock(consoleMutex);
    if (!g_printedLines.insert(std::string(used ? "[+]" : "[i]") + message + orangeSuffix).second) return;
    if (cliConfig.quiet) {
        if (orangeSuffix.empty()) return;
        Color::setForegroundColor(Color::Orange);
        std::cout << "[!] " << message << orangeSuffix << std::endl;
        return;
    }
    if (used) {
        Color::setForegroundColor(Color::Green);
        std::cout << "[+] ";
        Color::setForegroundColor(Color::White);
    }
    else {
        Color::setForegroundColor(Color::LightGray);
        std::cout << "  ";
    }
    std::cout << message;
    if (!orangeSuffix.empty()) {
        Color::setForegroundColor(Color::Orange);
        std::cout << orangeSuffix;
        Color::setForegroundColor(Color::LightGray);
    }
    std::cout << std::endl;
}

void Helper::runSystemCommand(const char* command)
{
    // v1.2.2 bugfix: bounded runner - a hung child (DISM, pnputil, installer)
    // must not freeze the tool forever; default budget 10 minutes
    std::string modifiedCommand = command ? command : "";
    if (modifiedCommand.empty()) return;
    if (modifiedCommand.find("2>nul") == std::string::npos &&
        modifiedCommand.find("2>&1") == std::string::npos) {
        modifiedCommand += " 2>nul";
    }
    runBoundedCommand(modifiedCommand.c_str(), 10 * 60 * 1000, NULL, NULL);
}

int Helper::runSystemCommandExitCode(const char* command)
{
    if (command == NULL) return -1;
    std::string modifiedCommand = command;
    if (modifiedCommand.find("2>nul") == std::string::npos &&
        modifiedCommand.find("2>&1") == std::string::npos) {
        modifiedCommand += " 2>nul";
    }
    // v1.2.2 bugfix: bounded - pnputil/dism hang used to block forever
    DWORD exitCode = (DWORD)-1;
    int rc = runBoundedCommand(modifiedCommand.c_str(), 10 * 60 * 1000, NULL, &exitCode);
    if (rc == 1) return -2;   // timed out (distinct from real exit codes)
    if (rc < 0) return -1;
    return static_cast<int>(exitCode);
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

