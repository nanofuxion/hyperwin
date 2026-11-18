# Installs or updates HyperWin Audio (hwaudio.sys) from this folder. Run as SYSTEM or administrator,
# by gpu-install.cmd. Windows wants a signed catalog for a driver package, and nothing on the build
# side can make one, so it is made here and signed with the hyperwin-pkg key (audio/README.md).
# Also takes out the Scream sound card that machines made before hwaudio have.
$ErrorActionPreference = 'Stop'
$d = $PSScriptRoot
$w = "$env:SystemRoot\Temp\hwaudio"
$log = "$d\install.log"
"$(Get-Date -Format s) install" | Out-File $log
Remove-Item -Recurse -Force $w -ErrorAction SilentlyContinue
New-Item -ItemType Directory $w | Out-Null
Copy-Item "$d\hwaudio.inf", "$d\hwaudio.sys" $w
New-FileCatalog -Path "$w\hwaudio.inf", "$w\hwaudio.sys" -CatalogFilePath "$w\hwaudio.cat" -CatalogVersion 2.0 | Out-Null
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2("$d\hyperwin-pkg.pfx", 'hyperwin')
$sig = Set-AuthenticodeSignature -FilePath "$w\hwaudio.cat" -Certificate $cert -HashAlgorithm SHA256
"catalog: $($sig.Status)" | Out-File $log -Append
if ($sig.Status -ne 'Valid') { throw "catalog signature: $($sig.StatusMessage)" }

$have = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\HwAudio' }
$verb = if ($have) { 'update' } else { 'install' }
# The audio service keeps the device open, and an update would then wait for a restart.
if ($have) { Stop-Service Audiosrv, AudioEndpointBuilder -Force -ErrorAction SilentlyContinue }
& "$d\devcon.exe" $verb "$w\hwaudio.inf" 'Root\HwAudio' 2>&1 | Out-File $log -Append
"devcon $verb exit $LASTEXITCODE (1: takes effect at the next restart)" | Out-File $log -Append
if ($have) { Start-Service AudioEndpointBuilder, Audiosrv -ErrorAction SilentlyContinue }

# Older hwaudio packages, so Windows never goes back to one.
$dev = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'Root\HwAudio' }
$inuse = if ($dev) { (Get-PnpDeviceProperty -InstanceId $dev.InstanceId DEVPKEY_Device_DriverInfPath).Data }
pnputil /enum-drivers | Out-String | Select-String -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+hwaudio\.inf' -AllMatches |
    ForEach-Object { $_.Matches } | Where-Object { $_.Groups[1].Value -ne $inuse } |
    ForEach-Object { pnputil /delete-driver $_.Groups[1].Value } | Out-File $log -Append

# Scream sent sound over the network; hwaudio replaces it.
if (Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains '*Scream' }) {
    & "$d\devcon.exe" remove '*Scream' 2>&1 | Out-File $log -Append
    pnputil /enum-drivers | Out-String | Select-String -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+scream\.inf' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { pnputil /delete-driver $_.Groups[1].Value /uninstall /force } |
        Out-File $log -Append
}
