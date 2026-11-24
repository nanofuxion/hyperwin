# Installs or updates one HyperWin driver package on its root devices:
#   install-driver.ps1 hwaudio Root\HwAudio      (files in .\hwaudio\: hwaudio.inf, hwaudio.sys)
#   install-driver.ps1 hwhid Root\HwHid    (the package also serves devices that come later, such
#                                           as hwvsinput.sys's DualShock 4, HWPAD\VID_054C&PID_09CC)
# Run as SYSTEM or administrator, by gpu-install.cmd. Windows wants a signed catalog for a driver
# package, and nothing on the build side can make one, so it is made here and signed with the
# hyperwin-pkg key (audio/README.md). Writes .\<name>\install.log.
param([string]$Name, [string]$Ids, [switch]$IfChanged)
$Id = $Ids -split ','
$ErrorActionPreference = 'Stop'
$d = $PSScriptRoot
# Older builds shipped Microsoft's devcon.exe here; hwdev.exe replaces it (its license forbids passing it on).
Remove-Item "$d\devcon.exe" -Force -ErrorAction SilentlyContinue
$src = "$d\$Name"
$log = "$src\install.log"

$parts = Get-ChildItem $src -File |
    Where-Object { $_.Name -notin 'installed.sha', 'install.log' -and $_.Extension -ne '.cat' } |
    Sort-Object Name |
    ForEach-Object { "$($_.Name):$((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }
$hash = [BitConverter]::ToString(
    [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes(($parts -join ';')))
).Replace('-', '')
$shaFile = "$src\installed.sha"
if ($IfChanged -and (Test-Path $shaFile)) {
    $old = (Get-Content $shaFile -Raw -ErrorAction SilentlyContinue).Trim()
    if ($old -eq $hash) {
        $allOk = $true
        foreach ($one in $Id) {
            if (-not (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $one -and $_.Status -eq 'OK' })) {
                $allOk = $false
                break
            }
        }
        if ($allOk) { exit 0 }
    }
}

$w = "$env:SystemRoot\Temp\$Name"
"$(Get-Date -Format s) install $Name" | Out-File $log
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
New-Item -ItemType Directory $w | Out-Null
Copy-Item "$src\*" $w -Exclude install.log, installed.sha, *.cat
New-FileCatalog -Path (Get-ChildItem $w -File).FullName -CatalogFilePath "$w\$Name.cat" -CatalogVersion 2.0 | Out-Null
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2("$d\hyperwin-pkg.pfx", 'hyperwin')
$sig = Set-AuthenticodeSignature -FilePath "$w\$Name.cat" -Certificate $cert -HashAlgorithm SHA256
"catalog: $($sig.Status)" | Out-File $log -Append
if ($sig.Status -ne 'Valid') { throw "catalog signature: $($sig.StatusMessage)" }

$reboot = $false
foreach ($one in $Id) {
    $have = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $one }
    $verb = if ($have) { 'update' } else { 'install' }
    # The audio service keeps the sound device open, and an update would then wait for a restart.
    $audio = $Name -eq 'hwaudio' -and $have
    if ($audio) { Stop-Service Audiosrv, AudioEndpointBuilder -Force -ErrorAction SilentlyContinue }
    & "$d\hwdev.exe" $verb "$w\$Name.inf" $one 2>&1 | Out-File $log -Append
    $rc = $LASTEXITCODE
    "hwdev $verb $one exit $rc (1: takes effect at the next restart)" | Out-File $log -Append
    if ($audio) { Start-Service AudioEndpointBuilder, Audiosrv -ErrorAction SilentlyContinue }
    if ($rc -eq 1) { $reboot = $true }
    elseif ($rc -ne 0) { throw "hwdev $verb $one failed (exit $rc)" }
}

Set-Content -Path $shaFile -Value $hash -NoNewline
if ($reboot) { Set-Content -Path "$d\reboot.needed" -Value "1" -NoNewline }

# Older packages of this driver, so Windows never goes back to one.
$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $Id[0] } | Select-Object -First 1
$inuse = if ($dev) { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId DEVPKEY_Device_DriverInfPath).Data }
pnputil /enum-drivers | Out-String | Select-String -Pattern "Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+$Name\.inf" -AllMatches |
    ForEach-Object { $_.Matches } | Where-Object { $_.Groups[1].Value -ne $inuse } |
    ForEach-Object { pnputil /delete-driver $_.Groups[1].Value } | Out-File $log -Append

# The Xbox 360 HID twin of an earlier build; players are USB Xbox 360 controllers now, and player 1
# can be a DualShock 4 instead.
if ($Name -eq 'hwhid' -and (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\VID_045E&PID_028E&IG_00' })) {
    & "$d\hwdev.exe" remove 'Root\VID_045E&PID_028E&IG_00' 2>&1 | Out-File $log -Append
}

# Scream sent sound over the network; hwaudio replaces it.
if ($Name -eq 'hwaudio' -and (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains '*Scream' })) {
    & "$d\hwdev.exe" remove '*Scream' 2>&1 | Out-File $log -Append
    pnputil /enum-drivers | Out-String | Select-String -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+scream\.inf' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { pnputil /delete-driver $_.Groups[1].Value /uninstall /force } |
        Out-File $log -Append
}
