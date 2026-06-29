// Original creator: Coby (coby69) for APPLECHEATS.CC
// Bug fixes and improvements by: Julien-winter
// This version was made on 17/08/2023 (DD/MM/YYYY).
// This is an open-source project and any and all code can be modified at any point in time.

// This code is released under the GNU General Public License (GPL).
// See https://www.gnu.org/licenses/gpl-3.0.txt for more information.

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

    std::thread titleLoopT(Helper::titleLoop);
    Helper::titleLoopBool = true;

    Color::setForegroundColor(Color::Cyan);
    std::cout << "Please wait 1-2 minutes while we run through and check everything\n";
    Color::setForegroundColor(Color::Green);
    std::cout << "Wait till the program says to take a screenshot to send one\n";
    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::cliConfig.headless) Sleep(1500);

    std::vector<std::thread> parallelChecks;

    if (!Helper::isCheckSkipped(1)) parallelChecks.emplace_back([]() { Checks::checkWindowsDefender(); });
    else { Helper::printConcern("- Skipped: Windows Defender"); Helper::recordResult("Windows Defender", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(2)) parallelChecks.emplace_back([]() { Checks::check3rdPartyAntiVirus(); });
    else { Helper::printConcern("- Skipped: 3rd Party AV"); Helper::recordResult("3rd Party AV", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(7)) parallelChecks.emplace_back([]() { Checks::isChromeInstalled(); });
    else { Helper::printConcern("- Skipped: Chrome Install"); Helper::recordResult("Google Chrome", "SKIPPED", ""); }

    for (auto& t : parallelChecks) t.join();
    parallelChecks.clear();

    if (!Helper::isCheckSkipped(3)) Checks::checkSecureBoot();
    else { Helper::printConcern("- Skipped: Secure Boot"); Helper::recordResult("Secure Boot", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(4)) Checks::checkCPUV();
    else { Helper::printConcern("- Skipped: CPU-V"); Helper::recordResult("CPU-V", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(5)) Checks::uninstallRiotVanguard();
    else { Helper::printConcern("- Skipped: Riot Vanguard"); Helper::recordResult("Riot Vanguard", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(6)) {
        std::thread vcThread(Checks::installVCRedist);
        while (!Helper::vcComplete) { Sleep(10); Helper::vcCheckSleepTimes += 10; if (Helper::vcCheckSleepTimes >= 30000) { Helper::printConcern("- VCRedist taking too long"); break; } }
        vcThread.detach();
    } else { Helper::printConcern("- Skipped: VCRedist"); Helper::recordResult("VC Redist", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(8)) Checks::disableChromeProtection();
    else { Helper::printConcern("- Skipped: Chrome Protection"); Helper::recordResult("Chrome Protection", "SKIPPED", ""); }

    if (!Helper::isCheckSkipped(9)) Checks::syncWindowsTime();
    else { Helper::printConcern("- Skipped: Time Sync"); Helper::recordResult("Time Sync", "SKIPPED", ""); }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";
    Color::setForegroundColor(Color::Cyan);
    std::cout << "Successfully ran standard checks, now running additional checks\n";
    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::isCheckSkipped(10)) Checks::checkWinver();
    else { Helper::printConcern("- Skipped: Winver"); Helper::recordResult("Winver", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(11)) Checks::deleteSymbols();
    else { Helper::printConcern("- Skipped: Symbols"); Helper::recordResult("Symbols", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(12)) Checks::checkFastBoot();
    else { Helper::printConcern("- Skipped: Fast Boot"); Helper::recordResult("Fast Boot", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(13)) Checks::checkExploitProtection();
    else { Helper::printConcern("- Skipped: Exploit Protection"); Helper::recordResult("Exploit Protection", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(14)) Checks::checkSmartScreen();
    else { Helper::printConcern("- Skipped: SmartScreen"); Helper::recordResult("SmartScreen", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(15)) Checks::checkGameBar();
    else { Helper::printConcern("- Skipped: Game Bar"); Helper::recordResult("Game Bar", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(16)) Checks::checkTPMStatus();
    else { Helper::printConcern("- Skipped: TPM"); Helper::recordResult("TPM", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(17)) Checks::checkCoreIsolation();
    else { Helper::printConcern("- Skipped: Core Isolation"); Helper::recordResult("Core Isolation", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(18)) Checks::checkDMAProtection();
    else { Helper::printConcern("- Skipped: DMA Protection"); Helper::recordResult("DMA Protection", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(19)) Checks::checkModifiedOS();
    else { Helper::printConcern("- Skipped: Modified OS"); Helper::recordResult("Modified OS", "SKIPPED", ""); }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";

    if (!Helper::isCheckSkipped(20)) Checks::checkInternet();
    else { Helper::printConcern("- Skipped: Internet"); Helper::recordResult("Internet", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(21)) Checks::checkVM();
    else { Helper::printConcern("- Skipped: VM Detection"); Helper::recordResult("VM Detection", "SKIPPED", ""); }
    if (!Helper::isCheckSkipped(22)) Checks::checkForUpdate();
    else { Helper::printConcern("- Skipped: Auto-Update"); Helper::recordResult("Auto-Update", "SKIPPED", ""); }

    Color::setForegroundColor(Color::LightGray);
    std::cout << "-----------------------------------------------------------------\n";
    Color::setForegroundColor(Color::Cyan);
    std::cout << "Successfully ran all checks, feel free to close the application\n";
    Color::setForegroundColor(Color::Green);
    std::cout << "Please take a screenshot of the program now and send it support";
    if (Helper::restartRequired) { Color::setForegroundColor(Color::Red); std::cout << "\n(Restart required to apply all changes)"; }
    std::cout << "\n";
    SetConsoleTitleA("Checking completed!");

    Helper::titleLoopBool = false;
    Sleep(300);
    titleLoopT.join();

    if (!Helper::cliConfig.exportPath.empty()) Helper::exportResultsJSON();
    Helper::closeLogging();

    if (!Helper::cliConfig.headless) { std::cout << "\nPress any key to exit..."; std::cin.get(); }
    return 0;
}
