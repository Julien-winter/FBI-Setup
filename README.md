# FBI-Setup

**Original Creator:** coby69 (Coby) for APPLECHEATS.CC  
**Maintained by:** Julien-winter

This code is released under the GNU General Public License (GPL).
See http://www.gnu.org/licenses/gpl-3.0.txt for more information.

The purpose of this program is to help the user setup the FBI Client by doing all the things that you used to have do yourself, automatically.

## Changes & Bug Fixes

### v1.2.1

**Added**
- **Full re-sync with CL-Setup v1.2.4**: every check, fix and improvement of the maintained CL-Setup line - single-instance lock, bounded command runner (no more hung tools), service-state and Chrome Safe Browsing policy fixes, HWID/VM/Activation checks, DMA state read directly via `NtQuerySystemInformation`, AMD SVM BIOS hints, Winver knows 26H1/26H2, hardening of all registry writes, undo chain (per-key backups + `applied-changes.log`), end summary with every WARN/FAIL by name, JSON export, and all console/log cleanups
- **Kick warning on flagged network adapters**: every adapter classified as VPN/virtual gets an orange ` (can cause game kicks)` suffix on its console line (also in the summary/JSON)
- **Undo scripts**: `Undo-FBI-Setup.ps1`/`.bat` revert everything the tool changed (backups in `%TEMP%\FBI-Setup\Backup` taken before every registry write)

**Changed**
- **Winver 26H2 is reported as "(not supported)"** (still WARN); 25H2 and unknown future builds keep the 50/50 wording
- **Loopback no longer listed**: `Loopback Pseudo-Interface 1` (Windows' own 127.0.0.1 interface) is hidden from the adapter list and no longer counted

**Removed**
- **The shop promo box is completely gone**: no MessageBox after the apply dialog, no `settings.ini` opt-out (nothing to opt out of anymore) - this build never shows `shop.uchiha.us` / `cheatloverz.store`

### v1.2.0

- Ported all improvements from CL-Setup
- Added async VCRedist installation (non-blocking thread)
- Added additional checks: GameBar, Modified OS detection, TPM status, Core Isolation (HVCI), DMA Protection
- Improved Winver check with Windows 11 23H2 / 24H2 support
- Improved Windows Defender check (service-based instead of registry)
- Better error handling with detailed error codes
- Improved build settings: C++20, static linking, admin rights
- Updated title loop animation with FBI branding
- Various bug fixes and stability improvements

## Build

Open `FBI Setup.sln` in Visual Studio 2022 and build for Release|x64 or Release|x86.
