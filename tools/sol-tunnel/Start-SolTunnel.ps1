# Start-SolTunnel.ps1
# Starts the SOL reverse SSH tunnel as a hidden background process.
# EC2's localhost:2223 will forward to sol.asu.edu:22 via this machine.
# Run once — close the terminal, tunnel keeps going.

param(
    [string]$EC2Host   = "100.52.254.81",
    [string]$EC2Key    = "$HOME\.ssh\Nova-Key.pem",
    [string]$SolHost   = "sol.asu.edu",
    [int]   $SolPort   = 22,
    [int]   $TunnelPort = 2223
)

$pidFile = "$PSScriptRoot\sol_tunnel.pid"

# Kill any existing tunnel process from a previous run
if (Test-Path $pidFile) {
    $oldPid = Get-Content $pidFile -ErrorAction SilentlyContinue
    if ($oldPid) {
        Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidFile -Force
}

$sshArgs = @(
    "-N",
    "-R", "${TunnelPort}:${SolHost}:${SolPort}",
    "-i", $EC2Key,
    "-o", "ServerAliveInterval=30",
    "-o", "ServerAliveCountMax=3",
    "-o", "StrictHostKeyChecking=no",
    "-o", "ExitOnForwardFailure=yes",
    "ubuntu@$EC2Host"
)

# Wrapper script that restarts the tunnel on failure
$watcherScript = {
    param($sshArgs, $pidFile)
    while ($true) {
        $proc = Start-Process ssh -ArgumentList $sshArgs -PassThru -WindowStyle Hidden
        $proc.Id | Out-File $pidFile -Force
        $proc.WaitForExit()
        Start-Sleep -Seconds 5
    }
}

$job = Start-Job -ScriptBlock $watcherScript -ArgumentList $sshArgs, $pidFile
$job.Id | Out-File "$PSScriptRoot\sol_tunnel_job.txt" -Force

Write-Host "SOL tunnel started (Job ID: $($job.Id))"
Write-Host "EC2 localhost:$TunnelPort -> $SolHost`:$SolPort"
Write-Host "Run .\Stop-SolTunnel.ps1 to stop it."
