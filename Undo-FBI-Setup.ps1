#Requires -Version 5.1
<#
    Undo-FBI-Setup.ps1
    Reverts the changes made by FBI Setup.

    RUN THIS ONE FILE - no .bat helper needed:
      right-click  ->  "Run with PowerShell"        (recommended)
      or: powershell -NoProfile -ExecutionPolicy Bypass -File .\Undo-FBI-Setup.ps1
    It asks for administrator rights itself (UAC prompt) and stays open at the
    end so you can read the result (pass -NoPause to skip the final "Press Enter").

    FBI Setup appends every non-trivial change to:
        %LOCALAPPDATA%\FBI-Setup\applied-changes.log
        markers:  backup:<subkey>|<exported .reg path>
                  reg:<subkey>
                  bcdedit:hypervisorlaunchtype=off
                  dism:<featurename>
                  service:WinDefend=disabled
                  service:W32Time=auto
                  info:<free text - not automatically revertible>
    and every removed unused virtual adapter to:
        %LOCALAPPDATA%\FBI-Setup\removed-adapters.log   (<instanceId>|<name>)

    This script:
      1. imports the registry backup that was taken BEFORE the first recorded
         write of each key (fallback: oldest matching backup in C:\Windows\Temp)
      2. deletes values the tool added that were not present in that backup
      3. sets bcdedit hypervisorlaunchtype back to auto
      4. re-enables the Hyper-V Windows features the tool disabled
      5. re-enables the Windows Defender / Windows Time services
      6. re-scans devices to bring removed virtual adapters back
      7. prints "info:" entries that cannot be reverted automatically

    A reboot is recommended when finished.
#>
param([switch]$NoPause)

$ErrorActionPreference = 'Continue'
$script:failures = 0

# ---------------------------------------------------------------- helpers
function Write-Step([string]$text) {
    Write-Host ""
    Write-Host "=== $text ===" -ForegroundColor Cyan
}
function Write-Ok([string]$text)   { Write-Host "  [+] $text" -ForegroundColor Green }
function Write-Warn([string]$text) { Write-Host "  [!] $text" -ForegroundColor Yellow }
function Write-Fail([string]$text) {
    Write-Host "  [x] $text" -ForegroundColor Red
    $script:failures++
}
# Keeps the window open so the user can read the outcome (a double-clicked
# window would close instantly on exit) - the old .bat provided the "pause".
function Stop-Undo([int]$code) {
    if (-not $NoPause) {
        Write-Host ""
        try { $null = Read-Host "Press Enter to close" } catch { }
    }
    exit $code
}

# ---------------------------------------------------------------- admin check
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Administrator rights are required - re-launching elevated..." -ForegroundColor Yellow
    $elevArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoPause) { $elevArgs += '-NoPause' }
    try {
        Start-Process powershell.exe -Verb RunAs -ArgumentList $elevArgs
        exit 0
    }
    catch {
        Write-Host "  [x] Elevation was cancelled - cannot undo without admin rights." -ForegroundColor Red
        Stop-Undo 1
    }
}

# Every registry value FBI Setup writes: key -> list of {Sub = extra path under
# the key, Name = value name}. Keys use single backslashes (log format).
$regWrites = [ordered]@{
    'SYSTEM\CurrentControlSet\Control\DeviceGuard' = @(
        @{ Sub = ''; Name = 'EnableVirtualizationBasedSecurity' },
        @{ Sub = ''; Name = 'RequirePlatformSecurityFeatures' },
        @{ Sub = ''; Name = 'Locked' }
    )
    'SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' = @(
        @{ Sub = ''; Name = 'Enabled' },
        @{ Sub = ''; Name = 'Locked' }
    )
    'SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management' = @(
        @{ Sub = ''; Name = 'FeatureSettingsOverride' },
        @{ Sub = ''; Name = 'FeatureSettingsOverrideMask' }
    )
    'SOFTWARE\Policies\Microsoft\Windows Defender' = @(
        @{ Sub = ''; Name = 'DisableAntiSpyware' },
        @{ Sub = '\Real-Time Protection'; Name = 'DisableRealtimeMonitoring' },
        @{ Sub = '\Real-Time Protection'; Name = 'DisableBehaviorMonitoring' },
        @{ Sub = '\Real-Time Protection'; Name = 'DisableOnAccessProtection' },
        @{ Sub = '\Real-Time Protection'; Name = 'DisableScanOnRealtimeEnable' }
    )
    'SOFTWARE\Policies\Google\Chrome' = @(
        @{ Sub = ''; Name = 'SafeBrowsingProtectionLevel' }
    )
    'SYSTEM\CurrentControlSet\Control\Session Manager\Power' = @(
        @{ Sub = ''; Name = 'HiberbootEnabled' }
    )
    'SOFTWARE\Policies\Microsoft\Windows Defender Security Center\App and Browser protection' = @(
        @{ Sub = ''; Name = 'DisallowExploitProtectionOverride' }
    )
    'SOFTWARE\Policies\Microsoft\Windows\System' = @(
        @{ Sub = ''; Name = 'EnableSmartScreen' }
    )
    'SOFTWARE\Microsoft\Windows\CurrentVersion\GameDVR' = @(
        @{ Sub = ''; Name = 'AppCaptureEnabled' }
    )
}

# Remove tool-written values that the backup does not contain.
# $BackupText = $null  -> remove every known value (key was created by the tool)
# v1.2.2 bugfix: search ONLY inside the section of the target key+subkey - the
# old whole-file match saw e.g. "Locked" from Scenarios\HypervisorEnforcedCodeIntegrity
# inside a DeviceGuard export and then never removed the tool's DeviceGuard\Locked
function Remove-ToolValues {
    param([string]$Key, [string]$BackupText)

    if (-not $regWrites.Contains($Key)) { return }
    foreach ($entry in $regWrites[$Key]) {
        $path = "HKLM:\$Key$($entry.Sub)"
        $existing = Get-ItemProperty -Path $path -Name $entry.Name -ErrorAction SilentlyContinue
        if ($null -eq $existing) { continue }   # value not set - nothing to do

        $inBackup = $false
        if ($BackupText) {
            $sectionName = "HKEY_LOCAL_MACHINE\$Key$($entry.Sub)"
            # .reg export: [HKEY_LOCAL_MACHINE\...] sections, optional quotes,
            # values until the next section header
            $pattern = '(?ims)^\["?]?' + [regex]::Escape($sectionName) + '["?]?\](.*?)(?=^\["?]?\[?HKEY_|\z)'
            $m = [regex]::Match($BackupText, $pattern)
            if ($m.Success) {
                $inBackup = $m.Groups[1].Value -match ('"' + [regex]::Escape($entry.Name) + '"')
            }
        }
        if ($inBackup) { continue }             # backup already restored the original

        try {
            Remove-ItemProperty -Path $path -Name $entry.Name -ErrorAction Stop
            Write-Host "  [-] removed $($entry.Name) (was added by FBI Setup)" -ForegroundColor DarkGray
        }
        catch {
            Write-Fail "could not remove $($entry.Name) under $Key : $($_.Exception.Message)"
        }
    }
}

# ---------------------------------------------------------------- read logs
$logDir      = Join-Path $env:LOCALAPPDATA 'FBI-Setup'
$changeLog   = Join-Path $logDir 'applied-changes.log'
$adapterLog  = Join-Path $logDir 'removed-adapters.log'

$lines = @()
if (Test-Path -LiteralPath $changeLog) {
    $lines = @(Get-Content -LiteralPath $changeLog)
}
if ($lines.Count -eq 0 -and -not (Test-Path -LiteralPath $adapterLog)) {
    Write-Host "No FBI Setup changes recorded - nothing to undo." -ForegroundColor Yellow
    Stop-Undo 0
}

Write-Host "FBI Setup - Undo" -ForegroundColor Cyan
Write-Host "Reverting recorded changes from: $changeLog"

# ------------------------------------------------- 1) registry restoration
Write-Step "Registry"

# first recorded write per key + the backup taken directly before it
$firstReg  = @{}
$backupFor = @{}
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($line.StartsWith('reg:')) {
        $key = $line.Substring(4)
        if (-not $firstReg.ContainsKey($key)) { $firstReg[$key] = $i }
    }
}
foreach ($key in @($firstReg.Keys)) {
    $idx = $firstReg[$key]
    for ($j = $idx - 1; $j -ge 0; $j--) {
        $line = $lines[$j]
        if (-not $line.StartsWith('backup:')) { continue }
        $payload = $line.Substring(7)
        $sep = $payload.IndexOf('|')
        if ($sep -lt 1) { continue }
        if ($payload.Substring(0, $sep) -eq $key) {
            $backupFor[$key] = $payload.Substring($sep + 1)
            break
        }
    }
}

if ($firstReg.Count -eq 0) {
    Write-Host "  no registry changes recorded" -ForegroundColor DarkGray
}
else {
    foreach ($key in ($firstReg.Keys | Sort-Object)) {
        Write-Host " [*] $key" -ForegroundColor White
        $backupPath = $null
        if ($backupFor.ContainsKey($key)) { $backupPath = $backupFor[$key] }

        # fallback: the recorded file may be gone (temp cleanup) - use the
        # OLDEST matching backup in C:\Windows\Temp (closest to pre-change state)
        if (-not $backupPath -or -not (Test-Path -LiteralPath $backupPath)) {
            $keyName = $key -replace '\\', '_'
            # exact name match: <KeyPath_underscored>_<YYYYMMDD-HHMMSS>.reg
            # (a plain glob would also match longer keys with the same prefix,
            #  e.g. ...\DeviceGuard matching ...\DeviceGuard\Scenarios\...)
            # v1.2.2 fix: the pattern demanded 7 digits after the dash but FBI Setup
            # writes HHMMSS (6) - the fallback never matched before
            $old = Get-ChildItem -Path 'C:\Windows\Temp' -Filter "$keyName`_*.reg" -File -ErrorAction SilentlyContinue |
                   Where-Object { $_.BaseName -match ('^' + [regex]::Escape($keyName) + '_\d{8}-\d{6}$') } |
                   Sort-Object Name | Select-Object -First 1
            # v1.2.2 fix: never take a fallback NEWER than the recorded pre-change
            # backup - a later run's backup already contains this run's changes and
            # would then be imported as "original state"
            if ($old -and $backupPath -and $backupPath -match '_(\d{8}-\d{6})\.reg$') {
                $recordedTs = $Matches[1]
                if ($old.BaseName -match '_(\d{8}-\d{6})$' -and $Matches[1] -gt $recordedTs) {
                    Write-Warn "recorded backup missing and newer fallback rejected: $($old.Name)"
                    $old = $null
                }
            }
            if ($old) {
                if ($backupPath) { Write-Warn "recorded backup missing, using oldest available: $($old.Name)" }
                $backupPath = $old.FullName
            }
        }

        if ($backupPath -and (Test-Path -LiteralPath $backupPath)) {
            $null = & reg.exe import "$backupPath" 2>&1
            if ($LASTEXITCODE -eq 0) {
                Write-Ok "restored backup: $(Split-Path $backupPath -Leaf)"
                $backupText = Get-Content -LiteralPath $backupPath -Raw -ErrorAction SilentlyContinue
                Remove-ToolValues -Key $key -BackupText $backupText
            }
            else {
                Write-Fail "reg import failed for $key"
                Remove-ToolValues -Key $key -BackupText $null
            }
        }
        else {
            # no backup exists -> the key/value did not exist before FBI Setup
            Write-Warn "no pre-change backup found - removing values added by FBI Setup"
            Remove-ToolValues -Key $key -BackupText $null
        }
    }
}

# ------------------------------------------------- 2) bcdedit hypervisor
if ($lines -contains 'bcdedit:hypervisorlaunchtype=off') {
    Write-Step "Boot configuration"
    $null = & bcdedit.exe /set '{current}' hypervisorlaunchtype auto 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "hypervisorlaunchtype set back to auto"
    }
    else {
        Write-Fail "bcdedit failed (exit $LASTEXITCODE)"
    }
}

# ------------------------------------------------- 3) DISM Hyper-V features
$dismFeatures = @($lines | Where-Object { $_.StartsWith('dism:') } |
                  ForEach-Object { $_.Substring(5) } | Select-Object -Unique)
if ($dismFeatures.Count -gt 0) {
    Write-Step "Windows features"
    foreach ($feature in $dismFeatures) {
        Write-Host " [*] enabling $feature (this can take a while)..." -ForegroundColor White
        $null = & dism.exe /online /enable-feature /featurename:$feature /norestart 2>&1
        if ($LASTEXITCODE -eq 0) {
            Write-Ok "$feature re-enabled"
        }
        else {
            Write-Fail "could not re-enable $feature (dism exit $LASTEXITCODE)"
        }
    }
    Write-Warn "a reboot is required to finish re-enabling the Hyper-V features"
}

# ------------------------------------------------- 4) services
if ($lines -contains 'service:WinDefend=disabled') {
    Write-Step "Windows Defender service"
    & sc.exe config WinDefend start= delayed-auto | Out-Null
    if ($LASTEXITCODE -ne 0) {
        & sc.exe config WinDefend start= auto | Out-Null
    }
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "WinDefend start type restored"
    }
    else {
        Write-Fail "could not change the WinDefend start type (tamper protection?)"
    }
    & sc.exe start WinDefend | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "WinDefend started"
    }
    else {
        Write-Warn "WinDefend could not be started - enable it manually in Windows Security"
    }
    Write-Warn "the Defender policy values are removed in the Registry section above"
}

# v1.2.2 bugfix: marker now carries the original start type
# ("service:W32Time=was4" = was disabled, "=was3" = was manual/demand, ...).
# The old marker "service:W32Time=auto" is still accepted (maps to demand).
$w32Marker = $lines | Where-Object { $_ -like 'service:W32Time=*' } | Select-Object -First 1
if ($w32Marker) {
    Write-Step "Windows Time service"
    $original = ($w32Marker -split '=', 2)[1]
    $target = 'demand'
    if ($original -like 'was*') {
        switch ($original.Substring(3)) {
            '0' { $target = 'boot' }
            '1' { $target = 'system' }
            '2' { $target = 'auto' }
            '3' { $target = 'demand' }
            '4' { $target = 'disabled' }
            default { $target = 'demand' }
        }
    }
    & sc.exe config W32Time start= $target | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Ok "W32Time start type set back to '$target' (original state)"
    }
    else {
        Write-Fail "could not change the W32Time start type"
    }
}

# ------------------------------------------------- 5) network adapters
if (Test-Path -LiteralPath $adapterLog) {
    $removed = @(Get-Content -LiteralPath $adapterLog | Where-Object { $_ -match '\|' })
    if ($removed.Count -gt 0) {
        Write-Step "Removed network adapters"
        foreach ($entry in $removed) {
            $parts = $entry.Split('|', 2)
            Write-Host "    - $($parts[1])  ($($parts[0]))" -ForegroundColor White
        }
        Write-Host " [*] re-scanning devices..." -ForegroundColor White
        $null = & pnputil.exe /scan-devices 2>&1
        Write-Warn "if an adapter is still missing: reboot, or reinstall the software that created it (VPN/virtualization)"
    }
}

# ------------------------------------------------- 6) info (not revertible)
$infos = @($lines | Where-Object { $_.StartsWith('info:') } |
           ForEach-Object { $_.Substring(5) } | Select-Object -Unique)
if ($infos.Count -gt 0) {
    Write-Step "Recorded changes that cannot be reverted automatically"
    foreach ($text in $infos) { Write-Warn $text }
}

# ------------------------------------------------- 7) summary / log archive
Write-Host ""
if ($script:failures -eq 0) {
    Write-Host "Undo finished successfully. A reboot is recommended." -ForegroundColor Green
    # v1.2.2 fix: only claim a restore point when the tool actually logged one
    if ($lines -contains 'restorepoint:created') {
        Write-Host "(A system restore point created by FBI Setup is also still available.)" -ForegroundColor DarkGray
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    if (Test-Path -LiteralPath $changeLog) {
        Move-Item -LiteralPath $changeLog -Destination "$changeLog.undone-$stamp" -Force
    }
    if (Test-Path -LiteralPath $adapterLog) {
        Move-Item -LiteralPath $adapterLog -Destination "$adapterLog.undone-$stamp" -Force
    }
    Write-Host "Change logs archived (*.undone-$stamp)." -ForegroundColor DarkGray
}
else {
    Write-Host "Undo finished with $script:failures problem(s) - see messages above." -ForegroundColor Red
    Write-Host "The change logs were kept so you can run this script again." -ForegroundColor Yellow
    Stop-Undo 1
}
Stop-Undo 0
