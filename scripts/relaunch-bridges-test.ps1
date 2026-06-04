# Relaunch all 3 bridges with console + struct log capture (per MEMORY spec).
$exe = "D:\BambuBridge\identical-win\build\src\Release\bambu-studio-bridge.exe"
$plugin = "C:\Users\danie\AppData\Roaming\BambuStudio\plugins\bambu_networking.dll"
$logdir = "C:\Users\danie\AppData\Local\BambuBridge\logs"
$devs = @(
  @{name="A1";  id="03900D610219434"},
  @{name="H2S"; id="0938BC582502312"},
  @{name="H2D"; id="0948DB561601642"}
)
Get-Process bambu-studio-bridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
foreach ($d in $devs) {
  $name = $d.name; $id = $d.id
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "cmd.exe"
  $psi.Arguments = "/c `"`"$exe`" --bridge-only --only-dev-id $id --plugin `"$plugin`" --bind 0.0.0.0 --mqtt-port-base 8883 --ftps-port-base 39990 --rtsp-port-base 38322 --vtun-port-base 39998 > `"$logdir\console-$name.log`" 2>&1`""
  $psi.UseShellExecute = $false
  $psi.EnvironmentVariables["BAMBU_BRIDGE_VERBOSE"] = "1"
  $psi.EnvironmentVariables["BAMBU_BRIDGE_SKIP_AUTH"] = "1"
  $psi.EnvironmentVariables["BBL_STRUCT_LOG"] = "$logdir\struct-$name.jsonl"
  $p = [System.Diagnostics.Process]::Start($psi)
  "launched $name pid=$($p.Id) devid=$id"
  Start-Sleep -Milliseconds 1500
}
"all launched; waiting 16s"
Start-Sleep -Seconds 16
Get-Process bambu-studio-bridge -ErrorAction SilentlyContinue | Select-Object Id,StartTime
