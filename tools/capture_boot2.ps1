# Open COM9 first, then reset MCU - ensures we catch boot output
$port = New-Object System.IO.Ports.SerialPort('COM9',115200,'None',8,'One')
$port.ReadTimeout = 200
$port.Open()
$port.DiscardInBuffer()
Write-Host "COM9 opened, discarding old data..."

$buf = New-Object byte[] 8192
$sb = New-Object System.Text.StringBuilder

# Reset MCU now (port already open and listening)
$stlink = "C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"
& $stlink -c SWD SWCLK=4000 -Rst 2>&1 | Out-Null

Write-Host "MCU reset sent, reading for 15 seconds..."
$deadline = [DateTime]::Now.AddSeconds(15)
$lastLen = 0
$stallCount = 0
while ([DateTime]::Now -lt $deadline) {
    try {
        $n = $port.Read($buf, 0, $buf.Length)
        if ($n -gt 0) {
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
            $stallCount = 0
        } else {
            $stallCount++
        }
    } catch {
        $stallCount++
    }
    # If we got data and then 3s of silence, stop early
    if ($sb.Length -gt 0 -and $stallCount -gt 15) { break }
}
$port.Close()

Write-Host ("Total bytes: " + $sb.Length)
if ($sb.Length -gt 0) {
    Write-Output $sb.ToString()
} else {
    Write-Output "STILL NO DATA"
}
