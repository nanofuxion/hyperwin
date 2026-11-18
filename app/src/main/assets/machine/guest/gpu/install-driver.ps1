# Installs or updates one HyperWin driver package on its root device:
#   install-driver.ps1 hwaudio Root\HwAudio      (files in .\hwaudio\: hwaudio.inf, hwaudio.sys)
# Run as SYSTEM or administrator, by gpu-install.cmd. Windows wants a signed catalog for a driver
# package, and nothing on the build side can make one, so it is made here and signed with the
# hyperwin-pkg key (audio/README.md). Writes .\<name>\install.log.
param([string]$Name, [string]$Id)
$ErrorActionPreference = 'Stop'
$d = $PSScriptRoot
$src = "$d\$Name"
$w = "$env:SystemRoot\Temp\$Name"
$log = "$src\install.log"
"$(Get-Date -Format s) install $Name" | Out-File $log
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
New-Item -ItemType Directory $w | Out-Null
Copy-Item "$src\$Name.inf", "$src\$Name.sys" $w
New-FileCatalog -Path "$w\$Name.inf", "$w\$Name.sys" -CatalogFilePath "$w\$Name.cat" -CatalogVersion 2.0 | Out-Null
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2("$d\hyperwin-pkg.pfx", 'hyperwin')
$sig = Set-AuthenticodeSignature -FilePath "$w\$Name.cat" -Certificate $cert -HashAlgorithm SHA256
"catalog: $($sig.Status)" | Out-File $log -Append
if ($sig.Status -ne 'Valid') { throw "catalog signature: $($sig.StatusMessage)" }

$find = { Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $Id } }
$have = & $find
$verb = if ($have) { 'update' } else { 'install' }
# The audio service keeps the sound device open, and an update would then wait for a restart.
$audio = $Name -eq 'hwaudio' -and $have
if ($audio) { Stop-Service Audiosrv, AudioEndpointBuilder -Force -ErrorAction SilentlyContinue }
& "$d\devcon.exe" $verb "$w\$Name.inf" $Id 2>&1 | Out-File $log -Append
"devcon $verb exit $LASTEXITCODE (1: takes effect at the next restart)" | Out-File $log -Append
if ($audio) { Start-Service AudioEndpointBuilder, Audiosrv -ErrorAction SilentlyContinue }

# Older packages of this driver, so Windows never goes back to one.
$dev = & $find
$inuse = if ($dev) { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId DEVPKEY_Device_DriverInfPath).Data }
pnputil /enum-drivers | Out-String | Select-String -Pattern "Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+$Name\.inf" -AllMatches |
    ForEach-Object { $_.Matches } | Where-Object { $_.Groups[1].Value -ne $inuse } |
    ForEach-Object { pnputil /delete-driver $_.Groups[1].Value } | Out-File $log -Append

# Scream sent sound over the network; hwaudio replaces it.
if ($Name -eq 'hwaudio' -and (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains '*Scream' })) {
    & "$d\devcon.exe" remove '*Scream' 2>&1 | Out-File $log -Append
    pnputil /enum-drivers | Out-String | Select-String -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+scream\.inf' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { pnputil /delete-driver $_.Groups[1].Value /uninstall /force } |
        Out-File $log -Append
}
