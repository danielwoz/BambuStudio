# Relaunch the single-printer A1 bridge with the cloud-command-route build.
# Stops any running bambu-studio-bridge.exe, then starts a fresh one with
# the debug env vars set via ProcessStartInfo (Start-Process does not
# inherit $env: into the child in this environment).

$ErrorActionPreference = 'Stop'

Get-Process bambu-studio-bridge -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "stopping old bridge PID $($_.Id)"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Milliseconds 800

$exe    = "D:\BambuBridge\BambuStudio-bridge\build\src\Release\bambu-studio-bridge.exe"
$plugin = "C:\Users\danie\AppData\Roaming\BambuStudio\plugins\bambu_networking.dll"

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName        = $exe
$psi.UseShellExecute = $false
$psi.WorkingDirectory = (Split-Path $exe)
$psi.Arguments = @(
    '--bridge-only',
    '--only-dev-id', '03900D610219434',
    '--plugin', "`"$plugin`"",
    '--bind', '0.0.0.0',
    '--mqtt-port-base', '8885',
    '--ftps-port-base', '39992',
    '--rtsp-port-base', '38324',
    '--vtun-port-base', '40000'
) -join ' '

$psi.EnvironmentVariables['BAMBU_BRIDGE_VERBOSE']   = '1'
$psi.EnvironmentVariables['BAMBU_BRIDGE_LOGLEVEL']  = '5'
$psi.EnvironmentVariables['BAMBU_BRIDGE_SKIP_AUTH'] = '1'
# prefer_lan defaults to false in the new build (cloud command route).
# Uncomment to force LAN command route instead:
# $psi.EnvironmentVariables['BAMBU_BRIDGE_PREFER_LAN'] = '1'

$p = [System.Diagnostics.Process]::Start($psi)
Write-Host "started new bridge PID $($p.Id)"
