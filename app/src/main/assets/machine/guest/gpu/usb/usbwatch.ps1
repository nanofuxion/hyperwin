# Plug and play for phone USB devices (usb/README.md): the app exports what is plugged into the phone as USB/IP
# on 10.0.2.2:3240. This keeps asking and attaches each new device through usbip-win2. A device that is unplugged
# closes its connection and usbip-win2 removes the port by itself.
$u = 'C:\Program Files\USBip\usbip.exe'
while ($true) {
    try {
        if (Test-Path $u) {
            $have = (& $u port 2>$null) -join "`n"
            foreach ($l in (& $u list -r 10.0.2.2 2>$null)) {
                if ($l -match '^\s*(\d+-\d+)\s*:') {
                    $b = $Matches[1]
                    if ($have -notmatch [regex]::Escape("/$b")) { & $u attach -r 10.0.2.2 -b $b 2>$null | Out-Null }
                }
            }
        }
    } catch {}
    Start-Sleep -Seconds 2
}
