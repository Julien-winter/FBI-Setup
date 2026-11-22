#include "Includes.h"

int main(int argc, char* argv[])
{
    Helper::setupConsole();
    Helper::cliConfig = Helper::parseCLI(argc, argv);

    if (Helper::cliConfig.showHelp) {
        Helper::showHelp();
        std::cout << "\nPress any key to exit...";
        std::cin.get();
        return 0;
    }

    // v1.2.3 bugfix: a second instance writing into the SAME console interleaved
    // its output with the already running one - identical check lines appeared
    // back to back (e.g. "- Fast-Boot is disabled" twice). One instance per
    // session now: the second start gets a short notice and exits instead of
    // mixing its lines into the running check.
    HANDLE hSingleInstance = CreateMutexA(NULL, TRUE, "Local\\FBI-Setup-Running");
    if (hSingleInstance != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        Color::setForegroundColor(Color::Yellow);
        std::cout << "[-] FBI-Setup is already running - a second window would only mix up the output.\n";
        std::cout << "    Close the other window first, then start it again.\n";
        Color::setForegroundColor(Color::LightGray);
        if (!Helper::cliConfig.headless) {
            std::cout << "\nPress any key to exit...";
            std::cin.get();
        }
        return 1;
    }
    // hSingleInstance is intentionally kept open for the whole process lifetime
    // (released by the OS on exit) - closing it would reopen the race

    if (!Helper::isAdmin()) {
        Color::setForegroundColor(Color::Red);
        std::cout << "[!] This program requires administrator privileges.\n";
        std::cout << "    Please right-click and select 'Run as administrator'.\n";
        Color::setForegroundColor(Color::LightGray);
        std::cout << "\nPress any key to exit...";
        std::cin.get();
        return 1;
    }

    Helper::initLogging();

    Color::setForegroundColor(Color::Cyan);
    std::cout << "Please wait 1-2 minutes while we run through and check everything\n";
    Color::setForegroundColor(Color::Green);
    std::cout << "Wait till the program says to take a screenshot to send one\n";
    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::cliConfig.headless) {
        Sleep(1500);
    }

    int warningCount = 0;
    int failedCount = 0;

    std::vector<std::thread> parallelChecks;

    if (!Helper::isCheckSkipped(1)) {
        parallelChecks.emplace_back([]() { Checks::checkWindowsDefender(); });
    }
    else { Helper::recordResult("Windows Defender", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(2)) {
        parallelChecks.emplace_back([]() { Checks::check3rdPartyAntiVirus(); });
    }
    else { Helper::recordResult("3rd Party AV", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(6)) {
        parallelChecks.emplace_back([]() { Checks::isChromeInstalled(); });
    }
    else { Helper::recordResult("Google Chrome", "SKIPPED", ""); }

    for (auto& t : parallelChecks) t.join();
    parallelChecks.clear();

    if (!Helper::isCheckSkipped(3)) Checks::checkSecureBoot();
    else { Helper::recordResult("Secure Boot", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(4)) Checks::uninstallRiotVanguard();
    else { Helper::recordResult("Riot Vanguard", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(5)) {
        std::thread vcThread(Checks::installVCRedist);
        while (Helper::vcComplete == false) {
            Sleep(10);
            Helper::vcCheckSleepTimes = Helper::vcCheckSleepTimes + 10;
            if (Helper::vcCheckSleepTimes >= 30000) {
                Helper::printConcern("- VCRedist is taking too long, continuing without waiting");
                break;
            }
        }
        // v1.2.2 bugfix: join instead of detach while the result is fresh - a
        // detached thread could still record/push into g_results while the
        // summary counts and the JSON export iterate it (data race).
        // Only reached when vcComplete was set (all recordResult calls done).
        if (Helper::vcComplete) vcThread.join();
        else vcThread.detach();   // pathological hang: old behaviour, counters are mutex-protected now
    }
    else { Helper::recordResult("VC Redist", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(7)) Checks::disableChromeProtection();
    else { Helper::recordResult("Chrome Protection", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(8)) Checks::syncWindowsTime();
    else { Helper::recordResult("Time Sync", "SKIPPED", ""); }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";
    Color::setForegroundColor(Color::Cyan);
    std::cout << "Successfully ran standard checks, now running additional checks\n";
    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::isCheckSkipped(9)) Checks::checkWinver();
    else { Helper::recordResult("Winver", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(10)) Checks::deleteSymbols();
    else { Helper::recordResult("Symbols", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(11)) Checks::checkFastBoot();
    else { Helper::recordResult("Fast Boot", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(12)) Checks::checkExploitProtection();
    else { Helper::recordResult("Exploit Protection", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(13)) Checks::checkSmartScreen();
    else { Helper::recordResult("SmartScreen", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(14)) Checks::checkGameBar();
    else { Helper::recordResult("Game Bar", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(15)) Checks::checkTPMStatus();
    else { Helper::recordResult("TPM", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(16)) Checks::checkCoreIsolation();
    else { Helper::recordResult("Core Isolation", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(17)) Checks::checkDMAProtection();
    else { Helper::recordResult("DMA Protection", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(18)) Checks::checkModifiedOS();
    else { Helper::recordResult("Modified OS", "SKIPPED", ""); }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";
    Color::setForegroundColor(Color::Cyan);
    std::cout << "Network, update and hardening checks\n";
    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::isCheckSkipped(19)) Checks::checkInternet();
    else { Helper::recordResult("Internet", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(20)) Checks::checkNetworkAdapters();
    else { Helper::recordResult("Network Adapters", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(21)) Checks::checkVM();
    else { Helper::recordResult("VM Detection", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(22)) Checks::checkForUpdate();
    else { Helper::recordResult("Auto-Update", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(23)) Checks::checkSecurityMitigations();
    else { Helper::recordResult("Security Mitigations", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(24)) Checks::checkUAC();
    else { Helper::recordResult("UAC", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(25)) Checks::checkWindowsUpdate();
    else { Helper::recordResult("Windows Update", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(26)) Checks::checkCPUVirtualization();
    else { Helper::recordResult("CPU-V", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(27)) Checks::checkSpoofer();
    else { Helper::recordResult("HWID Spoofer", "SKIPPED", ""); }

    // v1.2.1: no longer purely read-only - the watermark hide runs through
    // requestChange, so the check IS re-run in the apply phase (like the others)
    if (!Helper::isCheckSkipped(28)) Checks::checkActivation();
    else { Helper::recordResult("Activation", "SKIPPED", ""); }

    // Count warnings and failures (v1.2.2: under the results mutex - the VCRedist
    // thread may still be finishing, and recordResult is called from threads)
    decltype(Helper::g_results) resultsSnapshot;
    {
        std::lock_guard<std::mutex> lock(Helper::g_resultsMutex);
        resultsSnapshot = Helper::g_results;
    }
    for (const auto& r : resultsSnapshot) {
        if (r.status == "WARN") warningCount++;
        if (r.status == "FAIL") failedCount++;
    }

    if (Helper::hasPendingChanges()) {
        Color::setForegroundColor(Color::LightGray);
        std::cout << "-----------------------------------------------------------------\n";
        Color::setForegroundColor(Color::Cyan);
        std::cout << "Initial checks completed. No changes have been applied yet.\n";
        Color::setForegroundColor(Color::LightGray);

        bool approved = Helper::cliConfig.autoApply || Helper::promptApplyChanges();
        if (approved) {
            Helper::applyChanges = true;
            Helper::clearPendingChanges();
            // v1.2.2: do NOT clear results here - recordResult upserts per check, so the
            // final summary keeps every check from both phases (all checks, not just the
            // 18 re-run ones)
            Helper::vcComplete = false;
            Helper::vcCheckSleepTimes = 0;

            Helper::logWrite("=== phase 2: applying approved changes ===");
            Color::setForegroundColor(Color::Cyan);
            std::cout << "\nApplying approved changes...\n";
            Color::setForegroundColor(Color::LightGray);
            std::cout << "-----------------------------------------------------------------\n";

            // v1.2.3: NO system restore point anymore - it was invisible in
            // System Restore on the target machines and duplicated what
            // Undo-FBI-Setup.ps1 already does via the %TEMP%\FBI-Setup\Backup
            // exports + applied-changes.log markers (every registry write is
            // exported before it happens)

            if (!Helper::isCheckSkipped(1)) Checks::checkWindowsDefender();
            if (!Helper::isCheckSkipped(2)) Checks::check3rdPartyAntiVirus();
            if (!Helper::isCheckSkipped(4)) Checks::uninstallRiotVanguard();
            if (!Helper::isCheckSkipped(5)) Checks::installVCRedist();
            if (!Helper::isCheckSkipped(6)) Checks::isChromeInstalled();
            if (!Helper::isCheckSkipped(7)) Checks::disableChromeProtection();
            if (!Helper::isCheckSkipped(8)) Checks::syncWindowsTime();
            if (!Helper::isCheckSkipped(10)) Checks::deleteSymbols();
            if (!Helper::isCheckSkipped(11)) Checks::checkFastBoot();
            if (!Helper::isCheckSkipped(12)) Checks::checkExploitProtection();
            if (!Helper::isCheckSkipped(13)) Checks::checkSmartScreen();
            if (!Helper::isCheckSkipped(14)) Checks::checkGameBar();
            if (!Helper::isCheckSkipped(16)) Checks::checkCoreIsolation();
            if (!Helper::isCheckSkipped(19)) Checks::checkInternet();
            // v1.2.1: adapter removal runs through requestChange - without the
            // apply re-run the approved pnputil removal never executed
            if (!Helper::isCheckSkipped(20)) Checks::checkNetworkAdapters();
            if (!Helper::isCheckSkipped(23)) Checks::checkSecurityMitigations();
            // v1.2.1: the watermark hide is a pending change now - re-run so the
            // approved NotificationDisabled/PaintDesktopVersion write executes
            if (!Helper::isCheckSkipped(28)) Checks::checkActivation();

            // Re-count after applying changes (v1.2.2: under the results mutex)
            warningCount = 0;
            failedCount = 0;
            {
                std::lock_guard<std::mutex> lock(Helper::g_resultsMutex);
                for (const auto& r : Helper::g_results) {
                    if (r.status == "WARN") warningCount++;
                    if (r.status == "FAIL") failedCount++;
                }
            }
        }
        else {
            Helper::printConcern("- User declined changes. Leaving system unchanged.");
        }
    }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";
    Color::setForegroundColor(Color::Cyan);
    std::cout << "Successfully ran all checks, feel free to close the application\n";
    Color::setForegroundColor(Color::Green);
    std::cout << "Please take a screenshot of the program now and send it support";
    if (Helper::restartRequired) {
        Color::setForegroundColor(Color::Red);
        std::cout << "\n(Restart required to apply all changes)";
    }

    // v1.2.2: one snapshot under the mutex, reused for counts AND the named list
    decltype(Helper::g_results) finalResults;
    {
        std::lock_guard<std::mutex> lock(Helper::g_resultsMutex);
        finalResults = Helper::g_results;
    }
    int passedCount = 0;
    int skippedCount = 0;
    for (const auto& r : finalResults) {
        if (r.status == "OK") passedCount++;
        else if (r.status == "SKIPPED") skippedCount++;
    }

    // v1.2.2: name the remaining problems - the top of the console has usually
    // scrolled away by now, and this screenshot is what support gets to see
    std::vector<const CheckResult*> problems;
    for (const auto& r : finalResults) {
        if (r.status == "FAIL" || r.status == "WARN") problems.push_back(&r);
    }
    if (!problems.empty()) {
        Color::setForegroundColor(Color::LightGray);
        std::cout << "\nRemaining issues:\n";
        for (const auto* r : problems) {
            Color::setForegroundColor(r->status == "FAIL" ? Color::Red : Color::Yellow);
            std::cout << (r->status == "FAIL" ? "[X] " : "[-] ") << r->check;
            if (!r->message.empty()) std::cout << ": " << r->message;
            std::cout << "\n";
        }
    }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "\n";
    Color::setForegroundColor(Color::Green);
    std::cout << "[+] " << passedCount << " passed";
    if (warningCount > 0) {
        Color::setForegroundColor(Color::Yellow);
        std::cout << "  [-] " << warningCount << " warning" << (warningCount != 1 ? "s" : "");
    }
    if (failedCount > 0) {
        Color::setForegroundColor(Color::Red);
        std::cout << "  [X] " << failedCount << " error" << (failedCount != 1 ? "s" : "");
    }
    if (skippedCount > 0) {
        Color::setForegroundColor(Color::DarkGray);
        std::cout << "  ~ " << skippedCount << " skipped";
    }
    std::cout << "\n";
    if (!Helper::g_logPath.empty()) {
        Color::setForegroundColor(Color::DarkGray);
        std::cout << "Log: " << Helper::g_logPath << "\n";
    }

    std::cout << "\n";
    SetConsoleTitleA("Checking completed!");

    if (!Helper::cliConfig.exportPath.empty()) {
        Helper::exportResultsJSON();
    }

    Helper::closeLogging();

    if (!Helper::cliConfig.headless) {
        std::cout << "\nPress any key to exit...";
        std::cin.get();
    }

    // v1.2.2: exit 3 = nothing was actually checked (e.g. --only 99 / all skipped)
    // - exit 0 with "0 passed" used to masquerade as a clean run
    if (failedCount > 0) Helper::g_exitCode = 1;
    else if (warningCount > 0) Helper::g_exitCode = 2;
    else if (passedCount == 0 && skippedCount > 0) Helper::g_exitCode = 3;
    else Helper::g_exitCode = 0;
    return Helper::g_exitCode;
}
