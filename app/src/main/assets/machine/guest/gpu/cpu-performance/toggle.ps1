param([switch]$Enable)
$ErrorActionPreference = 'Stop'
try {
    $s = Get-Service hwcpufreq -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') {
        Stop-Service hwcpufreq
        $s.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(15))
    }
    if ($Enable) {
        $target = 'C:\HyperWin\hwcpufreq.sys'
        Copy-Item 'C:\HyperWin\hwcpufreq.next.sys' $target -Force
        if (-not $s) {
            & sc.exe create hwcpufreq type= kernel start= demand binPath= $target
            if ($LASTEXITCODE -ne 0) { throw 'Cannot create CPU performance service' }
        } else {
            & sc.exe config hwcpufreq start= demand binPath= $target
            if ($LASTEXITCODE -ne 0) { throw 'Cannot configure CPU performance service' }
        }
        Start-Service hwcpufreq
        (Get-Service hwcpufreq).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
    } elseif ($s) {
        & sc.exe config hwcpufreq start= demand
        if ($LASTEXITCODE -ne 0) { throw 'Cannot keep CPU performance service manual' }
    }
    exit 0
} catch {
    Write-Output $_
    Stop-Service hwcpufreq -ErrorAction SilentlyContinue
    exit 1
}
