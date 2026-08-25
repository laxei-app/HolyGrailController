# Reconnect the PC Wi-Fi to an edge SoftAP.
#
# When the edge is flashed or reboots its AP goes down, and Windows keeps the
# association in a "connected" state while nothing actually gets through.
# One `netsh wlan connect` often misses (the AP is not up yet), so retry.
# Messages are ASCII on purpose: this file is read by powershell.exe with the
# system code page, and non-ASCII text here breaks parsing (2026-08-26).
param([string]$Ssid = "edge00", [int]$Tries = 6)

$ErrorActionPreference = "Continue"
for ($i = 1; $i -le $Tries; $i++) {
    & netsh wlan disconnect interface="Wi-Fi" | Out-Null
    Start-Sleep -Seconds 2
    & netsh wlan connect name=$Ssid ssid=$Ssid interface="Wi-Fi" | Out-Null
    Start-Sleep -Seconds 8
    if (Test-Connection -ComputerName 192.168.4.1 -Count 2 -Quiet -ErrorAction SilentlyContinue) {
        $ip = (Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress
        Write-Output ("connected (try {0}) PC={1}" -f $i, $ip)
        exit 0
    }
    Write-Output ("  try {0} failed, retrying" -f $i)
}
Write-Output "could not connect"
exit 1
