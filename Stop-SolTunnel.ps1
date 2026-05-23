# Stop-SolTunnel.ps1
# Stops the SOL reverse SSH tunnel started by Start-SolTunnel.ps1

$jobFile = "$PSScriptRoot\sol_tunnel_job.txt"
$pidFile = "$PSScriptRoot\sol_tunnel.pid"

# Stop the watcher job
if (Test-Path $jobFile) {
    $jobId = Get-Content $jobFile -ErrorAction SilentlyContinue
    if ($jobId) {
        Get-Job -Id $jobId -ErrorAction SilentlyContinue | Stop-Job -PassThru | Remove-Job -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $jobFile -Force -ErrorAction SilentlyContinue
}

# Kill the SSH process by PID
if (Test-Path $pidFile) {
    $savedPid = Get-Content $pidFile -ErrorAction SilentlyContinue
    if ($savedPid) {
        Stop-Process -Id $savedPid -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

# Kill any SSH process tunneling port 2223 (belt-and-suspenders)
$procs = Get-CimInstance Win32_Process -Filter "Name = 'ssh.exe'" -ErrorAction SilentlyContinue
foreach ($p in $procs) {
    if ($p.CommandLine -like "*2223*") {
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "SOL tunnel stopped."
