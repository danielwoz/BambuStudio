<#
.SYNOPSIS
  Launch the bridge under a GENUINE, Bambu-signed bambu-studio.exe so the network
  plugin's "signed studio" gate is satisfied and control commands get signed.

.DESCRIPTION
  The plugin verifies BOTH the host exe AND BambuStudio.dll via Authenticode.
  We run a genuine official exe (satisfies the exe check) that loads OUR forked
  BambuStudio.dll (the bridge); the baked-in PluginVerifyRedirect hook
  (BAMBU_BRIDGE_SIGN_REDIRECT) redirects the plugin's check of our DLL to the
  genuine official BambuStudio.dll. No injector, no fake certs, no private key.

  REQUIREMENT: an official Bambu Studio whose version MATCHES this build
  (SLIC3R_VERSION, currently 02.07.01.57) must be installed — it provides both
  the launcher exe and the genuine BambuStudio.dll redirect target. A version
  skew (e.g. 2.6 launcher vs 2.7 plugin) crashes the plugin's signing path.

.PARAMETER OfficialDir
  Install dir of the genuine, version-matched Bambu Studio.
  Default: C:\Program Files\Bambu Studio

.PARAMETER Mode
  gui (default) | bridge-only | invisible-gui

.PARAMETER ExtraArgs
  Extra args passed through to the launcher (e.g. --mqtt-port-base 8883).
#>
param(
  [string]$OfficialDir = "C:\Program Files\Bambu Studio",
  [ValidateSet('gui','bridge-only','invisible-gui')]
  [string]$Mode = 'gui',
  # For invisible-gui (relay) mode: the REAL device id to bridge (scopes the
  # virtual printer). Control relay requires invisible-gui — --bridge-only never
  # establishes the signing session.
  [string]$TargetDev = '',
  [switch]$SkipAuth,             # bypass virtual-client MQTT auth (testing)
  [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'
$ReleaseDir   = Join-Path $PSScriptRoot '..\build\src\Release' | Resolve-Path | Select-Object -ExpandProperty Path
$ourDll       = Join-Path $ReleaseDir 'BambuStudio.dll'
$officialExe  = Join-Path $OfficialDir 'bambu-studio.exe'
$officialDll  = Join-Path $OfficialDir 'BambuStudio.dll'
$genuineLnch  = Join-Path $ReleaseDir 'bambu-studio-genuine.exe'
$expectVer    = '02.07.01.57'   # keep in sync with version.inc

foreach ($p in @($ourDll,$officialExe,$officialDll)) {
  if (-not (Test-Path $p)) { throw "missing required file: $p" }
}

# verify the genuine binaries are signed + version-matched
$sig = Get-AuthenticodeSignature $officialExe
if ($sig.Status -ne 'Valid') { throw "official exe is not validly signed (Status=$($sig.Status)): $officialExe" }
$exeVer = (Get-Item $officialExe).VersionInfo.FileVersion
if ($exeVer -ne $expectVer) {
  Write-Warning "Genuine exe version $exeVer != build $expectVer. A version skew will crash the plugin signing path. Install matching Bambu Studio $expectVer."
}
Write-Host "genuine launcher: $officialExe  ($exeVer, $($sig.Status))"
Write-Host "redirect target : $officialDll"
Write-Host "bridge DLL      : $ourDll"

# stage the genuine exe next to OUR DLL so it loads the bridge DLL (its own dir)
Copy-Item $officialExe $genuineLnch -Force

# Launch via ProcessStartInfo so env reliably reaches the child.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName         = $genuineLnch
$psi.UseShellExecute  = $false
$psi.WorkingDirectory = $ReleaseDir

$argList = @()
switch ($Mode) {
  'bridge-only'   { $argList += '--bridge-only' }          # NOTE: control will NOT sign in this mode
  'invisible-gui' { $psi.EnvironmentVariables['BAMBU_BRIDGE_INVISIBLE_GUI'] = '1' }
  'gui'           { }
}
$argList += $ExtraArgs
$psi.Arguments = ($argList -join ' ')

# baked-in DLL-verification redirect (satisfies the plugin's "signed studio" gate)
$psi.EnvironmentVariables['BAMBU_BRIDGE_SIGN_REDIRECT'] = '1'
$psi.EnvironmentVariables['BAMBU_BRIDGE_GENUINE_DLL']   = $officialDll
if ($TargetDev) { $psi.EnvironmentVariables['BAMBU_BRIDGE_TARGET_DEV'] = $TargetDev }
if ($SkipAuth)  { $psi.EnvironmentVariables['BAMBU_BRIDGE_SKIP_AUTH']  = '1' }

if ($Mode -eq 'bridge-only') {
  Write-Warning "--bridge-only never establishes the signing session; control commands will not sign. Use -Mode invisible-gui for control relay."
}
Write-Host "launching ($Mode): $genuineLnch $($psi.Arguments)"
$proc = [System.Diagnostics.Process]::Start($psi)
Write-Host "started PID $($proc.Id)"
