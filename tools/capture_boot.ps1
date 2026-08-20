# Open COM9 and discard any old data
$port = New-Object System.IO.Ports.SerialPort('COM9',115200,'None',8,'One')
$port.ReadTimeout = 300
$port.Open()
$port.DiscardInBuffer()

$buf = New-Object byte[] 8192
$sb = New-Object System.Text.StringBuilder

# Launch ST-LINK reset in a background PowerShell process (non-blocking)
$stlinkPath = "C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"
$resetJob = Start-Job -ScriptBlock {
    param($path)
    & $path -c SWD SWCLK=4000 -Rst 2>&1 | Out-Null
} -ArgumentList $stlinkPath

# Wait for reset to actually happen (ST-LINK connects, resets, disconnects)
Start-Sleep -Milliseconds 500

# Read for 10 seconds
$deadline = [DateTime]::Now.AddSeconds(10)
while ([DateTime]::Now -lt $deadline) {
    try {
        $n = $port.Read($buf, 0, $buf.Length)
        if ($n -gt 0) {
            [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
        }
    } catch {}
}
$port.Close()
Remove-Job -Job $resetJob -Force 2>$null

if ($sb.Length -gt 0) {
    Write-Output $sb.ToString()
} else {
    Write-Output "NO DATA RECEIVED"
}
