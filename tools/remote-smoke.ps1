param(
  [string]$EmulatorPath = "",
  [int]$TimeoutMs = 120000,
  [int]$DelayCapMs = 15000,
  [string]$Device = "x3",
  [string]$HeapProfile = "x3",
  [string]$RemoteText = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Net.Http

function Resolve-EmulatorPath {
  param([string]$RequestedPath)

  if ($RequestedPath) {
    if (-not (Test-Path $RequestedPath)) {
      throw "Emulator binary not found: $RequestedPath"
    }
    return (Resolve-Path $RequestedPath).Path
  }

  $candidates = @(
    (Join-Path $PSScriptRoot "..\build-current\Release\crosspoint_emulator.exe"),
    (Join-Path $PSScriptRoot "..\build-cg9x11\Release\crosspoint_emulator.exe"),
    (Join-Path $PSScriptRoot "..\build\Release\crosspoint_emulator.exe")
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return (Resolve-Path $candidate).Path
    }
  }

  throw "Could not find emulator binary. Use -EmulatorPath to specify it."
}

function Write-Utf8NoBomFile {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Content
  )

  $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($Path, $Content, $utf8NoBom)
}

function ConvertFrom-SimctlLine {
  param([string]$Line)

  if ([string]::IsNullOrWhiteSpace($Line)) {
    return $null
  }

  $trimmedLine = $Line.Trim()
  if (-not $trimmedLine.StartsWith("[SIMCTL]")) {
    return $null
  }

  $payload = $trimmedLine.Substring(8).Trim()
  $firstBrace = $payload.IndexOf("{")
  $lastBrace = $payload.LastIndexOf("}")
  if ($firstBrace -lt 0 -or $lastBrace -lt $firstBrace) {
    return $null
  }

  try {
    return ($payload.Substring($firstBrace, $lastBrace - $firstBrace + 1) | ConvertFrom-Json -ErrorAction Stop)
  } catch {
    return $null
  }
}

function New-JsonContent {
  param([hashtable]$Value)

  $json = $Value | ConvertTo-Json -Compress
  return [System.Net.Http.StringContent]::new($json, [System.Text.Encoding]::UTF8, "application/json")
}

if ([string]::IsNullOrWhiteSpace($RemoteText)) {
  $RemoteText = "remote-user-" + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
}

$emulatorExe = Resolve-EmulatorPath -RequestedPath $EmulatorPath
$controlScriptPath = Join-Path $PSScriptRoot "tmp-remote-smoke-control.txt"
$stdoutLogPath = Join-Path $PSScriptRoot "tmp-remote-smoke-stdout.log"
$stderrLogPath = Join-Path $PSScriptRoot "tmp-remote-smoke-stderr.log"

$controlScript = @"
WAIT_MENU_ITEM File Transfer 15000
ACTIVATE_VISIBLE_MENU_ITEM File Transfer
WAIT_ACTIVITY NetworkModeSelection 10000
ACTIVATE_VISIBLE_LIST_ITEM Join a Network
WAIT_ACTIVITY WifiSelection 10000
WAIT_LIST_ITEM Crosspoint-Emulator 10000
ACTIVATE_VISIBLE_LIST_ITEM Crosspoint-Emulator
WAIT_ACTIVITY CrossPointWebServer 15000
WAIT_ACTIVITY Home 60000
ACTIVATE_VISIBLE_MENU_ITEM Settings
WAIT_ACTIVITY Settings 10000
TAP CONFIRM 180
WAIT_MS 600
TAP CONFIRM 180
WAIT_MS 600
TAP CONFIRM 180
WAIT_MS 600
ACTIVATE_VISIBLE_LIST_ITEM KOReader Sync
WAIT_ACTIVITY KOReaderSettings 10000
ACTIVATE_VISIBLE_LIST_ITEM Username
WAIT_ACTIVITY KeyboardEntry 10000
WAIT_ACTIVITY KOReaderSettings 10000
GET_STATE
QUIT
"@

Write-Utf8NoBomFile -Path $controlScriptPath -Content $controlScript

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $emulatorExe
$psi.Arguments = "--control-stdio --control-script `"$controlScriptPath`" --delay-cap-ms $DelayCapMs --device $Device --heap-profile $HeapProfile --reset-session --exit-after-ms $TimeoutMs"
$psi.WorkingDirectory = Split-Path $emulatorExe -Parent
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$process = New-Object System.Diagnostics.Process
$process.StartInfo = $psi
$null = $process.Start()

$httpHandler = New-Object System.Net.Http.HttpClientHandler
$httpHandler.AutomaticDecompression = [System.Net.DecompressionMethods]::GZip -bor [System.Net.DecompressionMethods]::Deflate
$httpClient = New-Object System.Net.Http.HttpClient($httpHandler)
$httpClient.Timeout = [TimeSpan]::FromSeconds(8)

$stdoutLines = New-Object System.Collections.Generic.List[string]
$remoteStatus = $null
$remotePageHtml = ""
$remoteBmpMagic = ""
$remoteButtonResult = $null
$keyboardStatus = $null
$remoteTextResult = $null
$finalState = $null
$buttonPosted = $false
$textPosted = $false

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
while (-not $process.HasExited -and $stopwatch.ElapsedMilliseconds -lt $TimeoutMs) {
  while (-not $process.StandardOutput.EndOfStream) {
    $line = $process.StandardOutput.ReadLine()
    if ($null -eq $line) {
      break
    }

    $stdoutLines.Add($line)
    $event = ConvertFrom-SimctlLine -Line $line
    if ($null -eq $event) {
      continue
    }

    if ($event.PSObject.Properties.Name -contains "event" -and $event.event -eq "state") {
      $finalState = $event.state
    }

    if (-not $buttonPosted -and $event.PSObject.Properties.Name -contains "event" -and
      $event.event -eq "wait_satisfied" -and $event.waitType -eq "activity" -and $event.value -eq "CrossPointWebServer") {
      Start-Sleep -Milliseconds 700
      $remotePageHtml = $httpClient.GetStringAsync("http://127.0.0.1/remote").GetAwaiter().GetResult()
      $bmpBytes = $httpClient.GetByteArrayAsync("http://127.0.0.1/api/remote/screen.bmp?t=1").GetAwaiter().GetResult()
      if ($bmpBytes.Length -ge 2) {
        $remoteBmpMagic = [System.BitConverter]::ToString($bmpBytes[0..1])
      }
      $remoteStatus = $httpClient.GetStringAsync("http://127.0.0.1/api/remote/status").GetAwaiter().GetResult() | ConvertFrom-Json
      $remoteButtonResult = $httpClient.PostAsync("http://127.0.0.1/api/remote/button", (New-JsonContent @{ button = "confirm" })).GetAwaiter().GetResult().Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
      $buttonPosted = $true
      continue
    }

    if (-not $textPosted -and $event.PSObject.Properties.Name -contains "event" -and
      $event.event -eq "wait_satisfied" -and $event.waitType -eq "activity" -and $event.value -eq "KeyboardEntry") {
      Start-Sleep -Milliseconds 500
      $keyboardStatus = $httpClient.GetStringAsync("http://127.0.0.1/api/remote/status").GetAwaiter().GetResult() | ConvertFrom-Json
      $remoteTextResult = $httpClient.PostAsync("http://127.0.0.1/api/remote/text", (New-JsonContent @{ text = $RemoteText })).GetAwaiter().GetResult().Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
      $textPosted = $true
      continue
    }
  }

  Start-Sleep -Milliseconds 100
}

if (-not $process.HasExited) {
  $process.Kill()
  $process.WaitForExit()
  throw "Remote smoke timed out after $TimeoutMs ms"
}

$stderrText = $process.StandardError.ReadToEnd()
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllLines($stdoutLogPath, $stdoutLines, $utf8NoBom)
[System.IO.File]::WriteAllText($stderrLogPath, $stderrText, $utf8NoBom)

if ($process.ExitCode -ne 0) {
  throw "Emulator exited with code $($process.ExitCode). See $stdoutLogPath and $stderrLogPath"
}

if ([string]::IsNullOrWhiteSpace($remotePageHtml) -or $remotePageHtml -notmatch "CrossPoint Remote") {
  throw "Remote page did not load correctly"
}
if ($remoteBmpMagic -ne "42-4D") {
  throw "Remote BMP snapshot did not start with BM header"
}
if ($null -eq $remoteStatus -or $remoteStatus.currentActivity -ne "CrossPointWebServer" -or $remoteStatus.canAcceptText) {
  throw "Remote status before handoff was not the expected file transfer screen"
}
if ($null -eq $remoteButtonResult -or -not $remoteButtonResult.accepted) {
  throw "Remote confirm button was not accepted"
}
if ($null -eq $keyboardStatus -or $keyboardStatus.currentActivity -ne "KeyboardEntry" -or -not $keyboardStatus.canAcceptText) {
  throw "Remote status on keyboard screen did not advertise text input"
}
if ($null -eq $remoteTextResult -or -not $remoteTextResult.accepted) {
  throw "Remote text submit was not accepted"
}
if ($null -eq $finalState -or $finalState.currentActivity -ne "KOReaderSettings") {
  throw "Final emulator state did not return to KOReaderSettings"
}

$usernameItem = $null
foreach ($item in $finalState.screen.list.visibleItems) {
  if ($item.title -eq "Username") {
    $usernameItem = $item
    break
  }
}
if ($null -eq $usernameItem) {
  throw "Final KOReaderSettings state did not expose the Username row"
}
if ($usernameItem.value -ne $RemoteText) {
  throw "Remote text round-trip failed. Expected '$RemoteText' but final value was '$($usernameItem.value)'"
}

Write-Host "Remote smoke passed."
Write-Host ("  Emulator: {0}" -f $emulatorExe)
Write-Host ("  Device/heap: {0}/{1}" -f $Device, $HeapProfile)
Write-Host ("  Remote mode: {0}" -f $remoteStatus.mode)
Write-Host ("  Remote page + BMP: OK ({0})" -f $remoteBmpMagic)
Write-Host ("  Remote text round-trip: {0}" -f $RemoteText)
Write-Host ("  Logs: {0}, {1}" -f $stdoutLogPath, $stderrLogPath)
