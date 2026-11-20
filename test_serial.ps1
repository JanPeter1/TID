$port = New-Object System.IO.Ports.SerialPort('COM3', 115200)
$port.ReadTimeout = 5000
$port.Open()
$data = ""
try {
    for ($i = 0; $i -lt 80; $i++) {
        $data += $port.ReadLine() + "`n"
    }
} catch { $data += "read timeout after $i reads`n" }
$port.Close()
Write-Host $data