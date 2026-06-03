# Multi-printer Bambu Bridge launcher (cloud command route).
#
# One `bambu-studio-bridge.exe --bridge-only` child per printer — the
# proprietary plugin holds ONE LAN slot per process, so each printer gets
# its own process with its own --only-dev-id and its own port bases.
#
# Uses ProcessStartInfo (NOT Start-Process): in this environment
# Start-Process does not inherit $env: vars into the child, which silently
# dropped BAMBU_BRIDGE_* in the older multiproc script. Children share the
# real AppData (per-child AppData isolation hangs the plugin at init).
#
# All children inherit the baked-in defaults from this session:
#   - run_headless calls connect_server() -> cloud MQTT comes up
#   - SessionRouter prefer_lan=false      -> commands take the cloud route
# The shared bridge-relay.log is filterable per printer by `dev=<id>`.

$ErrorActionPreference = 'Stop'

$exe    = "D:\BambuBridge\BambuStudio-bridge\build\src\Release\bambu-studio-bridge.exe"
$plugin = "C:\Users\danie\AppData\Roaming\BambuStudio\plugins\bambu_networking.dll"

# dev_id : name : port offset (index). Each child's single printer sits at
# the *-port-base (index 0 within its own process), so distinct bases give
# distinct per-printer ports.
$Printers = @(
    @{ DevId = "03900D610219434"; Name = "A1"  ; Idx = 0 },
    @{ DevId = "0938BC582502312"; Name = "H2S" ; Idx = 1 },
    @{ DevId = "0948DB561601642"; Name = "H2D" ; Idx = 2 }
)

# Stop any existing bridge children first.
Get-Process bambu-studio-bridge -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host "stopping old bridge PID $($_.Id)"
    Stop-Process -Id $_.Id -Force
}
Start-Sleep -Milliseconds 800

foreach ($p in $Printers) {
    $mqtt = 8883  + $p.Idx
    $ftps = 39990 + $p.Idx
    $rtsp = 38322 + $p.Idx
    $vtun = 39998 + $p.Idx

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName         = $exe
    $psi.UseShellExecute  = $false
    $psi.WorkingDirectory = (Split-Path $exe)
    $psi.Arguments = @(
        '--bridge-only',
        '--only-dev-id',    $p.DevId,
        '--plugin',         "`"$plugin`"",
        '--bind',           '0.0.0.0',
        '--mqtt-port-base', "$mqtt",
        '--ftps-port-base', "$ftps",
        '--rtsp-port-base', "$rtsp",
        '--vtun-port-base', "$vtun"
    ) -join ' '

    $psi.EnvironmentVariables['BAMBU_BRIDGE_VERBOSE']   = '1'
    $psi.EnvironmentVariables['BAMBU_BRIDGE_LOGLEVEL']  = '5'
    $psi.EnvironmentVariables['BAMBU_BRIDGE_SKIP_AUTH'] = '1'

    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host ("started {0} dev={1} PID={2}  mqtt={3} ftps={4} rtsp={5} vtun={6}" -f `
        $p.Name, $p.DevId, $proc.Id, $mqtt, $ftps, $rtsp, $vtun)
    Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "All bridges launched. Tail per printer:"
Write-Host '  Get-Content $env:LOCALAPPDATA\BambuBridge\logs\bridge-relay.log -Wait | Select-String "dev=0948DB561601642"'
