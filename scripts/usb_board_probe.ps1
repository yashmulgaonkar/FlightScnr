<#
.SYNOPSIS
  Collects USB and esptool identity data for one attached board.

.DESCRIPTION
  Used to find properties that could distinguish the LilyGO T-Encoder Pro from
  the Waveshare ESP32-S3-Knob-Touch-LCD-1.8 in the web flasher. Connect exactly
  one board, then run:

    pwsh -File scripts/usb_board_probe.ps1 -Label "tencoder-pro-1"

  Results are appended to scripts/board-probe-results.txt.
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$Label,
  [string]$Port,
  [string]$OutFile = "$PSScriptRoot\board-probe-results.txt"
)

$ErrorActionPreference = 'Continue'

function Write-Section {
  param([string]$Text)
  $line = "`n=== $Text ==="
  Write-Host $line
  Add-Content -Path $OutFile -Value $line
}

function Write-Line {
  param([string]$Text)
  Write-Host $Text
  Add-Content -Path $OutFile -Value $Text
}

Write-Section "BOARD: $Label  ($(Get-Date -Format s))"

# ---------- USB layer ----------
Write-Section 'USB devices (VID/PID/serial/strings)'
$usb = Get-CimInstance Win32_PnPEntity | Where-Object {
  $_.PNPDeviceID -match 'VID_(303A|1A86|10C4|0403|1B4F)'
}
foreach ($dev in $usb) {
  Write-Line "Name        : $($dev.Name)"
  Write-Line "Manufacturer: $($dev.Manufacturer)"
  Write-Line "Service     : $($dev.Service)"
  Write-Line "PNPDeviceID : $($dev.PNPDeviceID)"
  foreach ($key in @(
      'DEVPKEY_Device_BusReportedDeviceDesc',
      'DEVPKEY_Device_Manufacturer',
      'DEVPKEY_Device_FriendlyName',
      'DEVPKEY_Device_LocationInfo')) {
    $prop = Get-PnpDeviceProperty -InstanceId $dev.PNPDeviceID -KeyName $key -ErrorAction SilentlyContinue
    if ($prop -and $prop.Data) {
      Write-Line "  $($key -replace 'DEVPKEY_Device_', '') = $($prop.Data)"
    }
  }
  Write-Line ''
}

# ---------- Serial port ----------
if (-not $Port) {
  $comDev = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match '\(COM\d+\)' -and $_.PNPDeviceID -match 'VID_(303A|1A86|10C4|0403)' } |
    Select-Object -First 1
  if ($comDev -and $comDev.Name -match '\((COM\d+)\)') {
    $Port = $Matches[1]
  }
}

if (-not $Port) {
  Write-Line 'No matching COM port found; skipping esptool probe.'
  return
}
Write-Line "Using port: $Port"

# ---------- esptool layer (what the browser flasher can also see) ----------
$python = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
$env:PYTHONPATH = Join-Path $env:USERPROFILE '.platformio\packages\tool-esptoolpy'
if (-not (Test-Path $python)) {
  Write-Line "PlatformIO python not found at $python"
  return
}

foreach ($cmd in @('chip_id', 'flash_id', 'read_mac')) {
  Write-Section "esptool $cmd"
  $output = & $python -m esptool --port $Port --after no_reset $cmd 2>&1
  Write-Line ($output | Out-String).TrimEnd()
}

Write-Section 'espefuse summary'
$efuse = & $python -m espefuse --port $Port summary 2>&1
Write-Line ($efuse | Out-String).TrimEnd()

Write-Line "`n--- end $Label ---`n"
