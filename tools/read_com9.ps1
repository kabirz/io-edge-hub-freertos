$ErrorActionPreference = 'Stop'
$port = New-Object System.IO.Ports.SerialPort('COM9',115200,'None',8,'One')
$port.ReadTimeout = 2000
$port.Open()

# Reset MCU via ST-LINK CLI while monitoring COM9
$stlink = '"C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"'
Start-Process -FilePath $stlink.Replace('"','') -ArgumentList '-c SWD SWCLK=4000 -Rst' -NoNewWindow -Wait

$buf = New-Object byte[] 4096
$sb = New-Object System.Text.StringBuilder
$deadline = [DateTime]::Now.AddSeconds(10)
while ([DateTime]::Now -lt $deadline) {
    try {
        $n = $port.Read($buf, 0, $buf.Length)
        if ($n -gt 0) {
            $text = [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
            [void]$sb.Append($text)
        }
    } catch {
        # Timeout on read is expected, continue until deadline
    }
}
$port.Close()
Write-Output $sb.ToString()
