param(
  [string]$EmulatorPath = "",
  [int]$TimeoutMs = 240000,
  [int]$DelayCapMs = 15000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Acquire-EmulatorSessionLock {
  param([int]$TimeoutMs = 600000)

  $mutex = [System.Threading.Mutex]::new($false, "Global\CrosspointEmulatorSessionLock")
  if (-not $mutex.WaitOne($TimeoutMs)) {
    $mutex.Dispose()
    throw "Timed out waiting for Crosspoint emulator session lock"
  }
  return $mutex
}

function Release-EmulatorSessionLock {
  param($Mutex)

  if ($null -eq $Mutex) {
    return
  }

  try {
    $Mutex.ReleaseMutex()
  } catch {
  }
  $Mutex.Dispose()
}

function Resolve-EmulatorPath {
  param([string]$RequestedPath)

  if ($RequestedPath) {
    return (Resolve-Path $RequestedPath).Path
  }

  $candidates = @(
    (Join-Path $PSScriptRoot "..\build\Release\crosspoint_emulator.exe"),
    (Join-Path $PSScriptRoot "..\build\crosspoint_emulator.exe"),
    (Join-Path $PSScriptRoot "..\build\crosspoint_emulator")
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return (Resolve-Path $candidate).Path
    }
  }

  throw "Could not find emulator binary. Use -EmulatorPath to specify it."
}

function Get-BenchmarkSessionPaths {
  param([string]$ResolvedEmulatorPath)

  $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
  $emulatorDir = Split-Path $ResolvedEmulatorPath -Parent
  $localSdcard = Join-Path $emulatorDir "sdcard"
  $parentSdcard = Join-Path (Split-Path $emulatorDir -Parent) "sdcard"
  $repoSdcard = Join-Path $repoRoot "sdcard"
  if (Test-Path $localSdcard) {
    $sdcardRoot = (Resolve-Path $localSdcard).Path
  } elseif (Test-Path $parentSdcard) {
    $sdcardRoot = (Resolve-Path $parentSdcard).Path
  } elseif (Test-Path $repoSdcard) {
    $sdcardRoot = (Resolve-Path $repoSdcard).Path
  } else {
    $sdcardRoot = $repoSdcard
  }
  $crosspointRoot = Join-Path $sdcardRoot ".crosspoint"
  $dataRoot = Join-Path $crosspointRoot "data"
  [pscustomobject]@{
    SdcardRoot = $sdcardRoot
    CrosspointRoot = $crosspointRoot
    DataRoot = $dataRoot
    TrackedSeriesPath = Join-Path $dataRoot "tracked_series.json"
    LegacyTrackedSeriesPath = Join-Path (Join-Path $crosspointRoot "plugins") "tracked_series.json"
    DownloadJobsPath = Join-Path $dataRoot "download_jobs.json"
    OnlineLibrarySettingsPath = Join-Path $dataRoot "online_library_settings.json"
    RecentJsonPath = Join-Path $crosspointRoot "recent.json"
    StateJsonPath = Join-Path $crosspointRoot "state.json"
    OnlineLibraryRoot = Join-Path $sdcardRoot "Online Library"
  }
}

function Reset-BenchmarkSessionData {
  param($Paths)

  $null = New-Item -ItemType Directory -Force -Path $Paths.SdcardRoot
  $null = New-Item -ItemType Directory -Force -Path $Paths.CrosspointRoot
  $null = New-Item -ItemType Directory -Force -Path $Paths.DataRoot

  foreach ($filePath in @(
      $Paths.TrackedSeriesPath,
      $Paths.LegacyTrackedSeriesPath,
      $Paths.DownloadJobsPath,
      $Paths.OnlineLibrarySettingsPath,
      $Paths.RecentJsonPath,
      $Paths.StateJsonPath
    )) {
    if (Test-Path $filePath) {
      Remove-Item -LiteralPath $filePath -Force -ErrorAction SilentlyContinue
    }
  }

  if (Test-Path $Paths.CrosspointRoot) {
    Get-ChildItem -LiteralPath $Paths.CrosspointRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object {
      if ($_.Name -eq "plugins" -or $_.Name -eq "data") {
        return
      }
      if ($_.Name.StartsWith("epub_") -or $_.Name.StartsWith("hako-epub-")) {
        Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
      }
    }
  }

  if (Test-Path $Paths.OnlineLibraryRoot) {
    Remove-Item -LiteralPath $Paths.OnlineLibraryRoot -Recurse -Force -ErrorAction SilentlyContinue
  }
}

function Write-BenchmarkTrackedSeriesFixture {
  param(
    $Paths,
    [int]$Count = 10
  )

  $null = New-Item -ItemType Directory -Force -Path $Paths.DataRoot
  $items = @()
  for ($i = 1; $i -le $Count; $i++) {
    $padded = "{0:D2}" -f $i
    $pluginId = if ($i % 2 -eq 0) { "truyenfull" } else { "hako" }
    $runtimeProfile = if ($pluginId -eq "truyenfull") { "truyenfull" } else { "hako" }
    $items += [ordered]@{
      id = "bench-$pluginId-$padded"
      pluginId = $pluginId
      runtimeProfile = $runtimeProfile
      title = "Benchmark Story $padded"
      author = "Author $padded"
      seriesUrl = "https://b/$padded"
      coverUrl = ""
      epubPath = ""
      lastChapterUrl = "https://b/$padded/c"
      lastChapterTitle = "Ch $i"
      lastReadChapterUrl = ""
      lastReadChapterTitle = ""
      lastReadPage = 0
      lastReadPageCount = 0
      chapterCount = 100 + $i
    }
  }

  $payload = [ordered]@{ items = $items } | ConvertTo-Json -Depth 5 -Compress
  $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($Paths.TrackedSeriesPath, $payload, $utf8NoBom)
}

function Parse-SimctlEvents {
  param([string]$StdoutText)

  $events = New-Object System.Collections.Generic.List[object]
  $lines = $StdoutText -split "`r?`n"
  foreach ($line in $lines) {
    $trimmedLine = $line.Trim()
    if (-not $trimmedLine.StartsWith("[SIMCTL] ")) {
      continue
    }
    $payload = $trimmedLine.Substring(9).Trim()
    $firstBrace = $payload.IndexOf("{")
    $lastBrace = $payload.LastIndexOf("}")
    if ($firstBrace -lt 0 -or $lastBrace -lt $firstBrace) {
      continue
    }
    $payload = $payload.Substring($firstBrace, $lastBrace - $firstBrace + 1)
    $events.Add(($payload | ConvertFrom-Json -ErrorAction Stop))
  }
  return $events
}

function Get-StateDiagnostics {
  param($State)

  if ($null -eq $State) {
    return $null
  }
  if ($State.PSObject.Properties.Name -contains "diagnostics" -and $null -ne $State.diagnostics) {
    return $State.diagnostics
  }
  if ($State.PSObject.Properties.Name -contains "screen" -and $null -ne $State.screen -and
    $State.screen.PSObject.Properties.Name -contains "diagnostics" -and $null -ne $State.screen.diagnostics) {
    return $State.screen.diagnostics
  }
  return $null
}

function Assert-StateDiagnostics {
  param(
    $State,
    [string]$Label,
    [hashtable]$Expected = @{}
  )

  $diagnostics = Get-StateDiagnostics -State $State
  if ($null -eq $diagnostics) {
    throw "$Label is missing diagnostics"
  }
  if ([string]::IsNullOrWhiteSpace([string]$diagnostics.summary)) {
    throw "$Label diagnostics summary is missing"
  }

  foreach ($entry in $Expected.GetEnumerator()) {
    $name = [string]$entry.Key
    if ($diagnostics.PSObject.Properties.Name -notcontains $name) {
      throw "$Label diagnostics missing property '$name'"
    }
    $actualValue = $diagnostics.$name
    $expectedValue = $entry.Value
    if ($expectedValue -is [scriptblock]) {
      $ok = & $expectedValue $actualValue $diagnostics
      if (-not $ok) {
        throw "$Label diagnostics '$name' failed custom check. Actual='$actualValue'"
      }
    } elseif ($actualValue -ne $expectedValue) {
      throw "$Label diagnostics '$name' expected '$expectedValue' but got '$actualValue'"
    }
  }
}

function Get-MatchingEvent {
  param(
    [object[]]$Events,
    [scriptblock]$Predicate
  )

  foreach ($event in $Events) {
    if (& $Predicate $event) {
      return $event
    }
  }
  return $null
}

function Invoke-BenchmarkRun {
  param(
    [string[]]$Commands,
    [string]$ResolvedEmulatorPath,
    [int]$TimeoutMs,
    [int]$DelayCapMs,
    [scriptblock]$Setup,
    [bool]$ResetSession = $true
  )

  $sessionPaths = Get-BenchmarkSessionPaths -ResolvedEmulatorPath $ResolvedEmulatorPath
  if ($ResetSession) {
    Reset-BenchmarkSessionData -Paths $sessionPaths
  }
  if ($null -ne $Setup) {
    & $Setup $sessionPaths
  }

  $psi = [System.Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = $ResolvedEmulatorPath
  $psi.WorkingDirectory = Split-Path $ResolvedEmulatorPath -Parent
  $psi.UseShellExecute = $false
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $psi.CreateNoWindow = $true
  $arguments = @("--control-stdio", "--delay-cap-ms", "$DelayCapMs")
  $psi.Arguments = $arguments -join " "

  $proc = [System.Diagnostics.Process]::new()
  $proc.StartInfo = $psi
  if (-not $proc.Start()) {
    throw "Failed to start emulator process"
  }

  $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
  $stderrTask = $proc.StandardError.ReadToEndAsync()

  foreach ($command in $Commands) {
    $proc.StandardInput.WriteLine($command)
  }
  $proc.StandardInput.Close()

  if (-not $proc.WaitForExit($TimeoutMs)) {
    try { $proc.Kill() } catch {}
    throw "Timed out after ${TimeoutMs}ms"
  }

  $stdoutText = $stdoutTask.GetAwaiter().GetResult()
  $stderrText = $stderrTask.GetAwaiter().GetResult()
  if ($proc.ExitCode -ne 0) {
    throw "Emulator exited with code $($proc.ExitCode). stderr: $stderrText"
  }

  $events = Parse-SimctlEvents -StdoutText $stdoutText
  $failedEvent = $events | Where-Object { ($_.PSObject.Properties.Name -contains "ok" -and -not $_.ok) -or $_.event -eq "wait_timeout" -or $_.event -eq "action_failed" } | Select-Object -First 1
  if ($failedEvent) {
    throw ("SIMCTL failure: " + ($failedEvent | ConvertTo-Json -Compress -Depth 10))
  }
  return $events
}

function Get-EventMillis {
  param([object[]]$Events, [scriptblock]$Predicate)
  $event = Get-MatchingEvent -Events $Events -Predicate $Predicate
  if ($null -ne $event -and $event.PSObject.Properties.Name -contains "state" -and $null -ne $event.state) {
    return [int]$event.state.millis
  }
  return $null
}

$resolvedEmulatorPath = Resolve-EmulatorPath -RequestedPath $EmulatorPath

$benchmarks = @(
  @{
    Name = "home_to_online_library"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "ready" }
    End = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Online Library" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Online Library" }
      Assert-StateDiagnostics -State $endEvent.state -Label "home_to_online_library end state"
    }
  },
  @{
    Name = "online_library_to_sources"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Online Library" }
    End = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Sources" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Sources" }
      Assert-StateDiagnostics -State $endEvent.state -Label "online_library_to_sources end state"
    }
  },
  @{
    Name = "hako_search_results"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Hako",
      "WAIT_HEADER Hako 30000",
      "TAP LEFT 180",
      "WAIT_ACTIVITY KeyboardEntry 10000",
      "TYPE_TEXT re zero",
      "WAIT_LIST_ITEM Re:Zero Kara Hajimeru Isekai Seikatsu 25000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "activity" -and $e.value -eq "KeyboardEntry" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "list_item" -and $e.value -eq "Re:Zero Kara Hajimeru Isekai Seikatsu" }
  },
  @{
    Name = "hako_source_enter_ready"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Hako",
      "WAIT_HEADER Hako 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Sources" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Hako" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Hako" }
      Assert-StateDiagnostics -State $endEvent.state -Label "hako_source_enter_ready end state"
    }
  },
  @{
    Name = "hako_source_feed_populated"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Hako",
      "WAIT_HEADER Hako 15000",
      "TAP RIGHT 180",
      "WAIT_BODY Loading source... 5000",
      "WAIT_BODY Summary 20000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Hako" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "body" -and $e.value -eq "Summary" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "body" -and $e.value -eq "Summary" }
      Assert-StateDiagnostics -State $endEvent.state -Label "hako_source_feed_populated end state"
    }
  },
  @{
    Name = "truyen_full_search_results"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Truyen Full",
      "WAIT_HEADER Truyen Full 30000",
      "TAP LEFT 180",
      "WAIT_ACTIVITY KeyboardEntry 10000",
      "TYPE_TEXT tien",
      "WAIT_LIST_ITEM Bach Luyen Thanh Tien 20000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "activity" -and $e.value -eq "KeyboardEntry" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "list_item" -and $e.value -eq "Bach Luyen Thanh Tien" }
  },
  @{
    Name = "webtruyen_source_enter_ready"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Web Truyen",
      "WAIT_HEADER Web Truyen 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Sources" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Web Truyen" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Web Truyen" }
      Assert-StateDiagnostics -State $endEvent.state -Label "webtruyen_source_enter_ready end state"
    }
  },
  @{
    Name = "webtruyen_source_feed_populated"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Web Truyen",
      "WAIT_HEADER Web Truyen 15000",
      "WAIT_BODY Summary 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "header" -and $e.value -eq "Web Truyen" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "body" -and $e.value -eq "Summary" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "body" -and $e.value -eq "Summary" }
      Assert-StateDiagnostics -State $endEvent.state -Label "webtruyen_source_feed_populated end state"
    }
  },
  @{
    Name = "hako_downloads_visible"
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Sources",
      "WAIT_HEADER Sources 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Hako",
      "WAIT_HEADER Hako 30000",
      "TAP LEFT 180",
      "WAIT_ACTIVITY KeyboardEntry 10000",
      "TYPE_TEXT re zero",
      "WAIT_LIST_ITEM Re:Zero Kara Hajimeru Isekai Seikatsu 25000",
      "ACTIVATE_VISIBLE_LIST_ITEM Re:Zero Kara Hajimeru Isekai Seikatsu",
      "WAIT_ACTIVITY HakoDetail 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Download EPUB",
      "WAIT_POPUP Added to downloads 10000",
      "GO_BACK 10000",
      "WAIT_HEADER Hako 10000",
      "GO_BACK 10000",
      "WAIT_HEADER Sources 10000",
      "GO_BACK 10000",
      "WAIT_HEADER Online Library 10000",
      "ACTIVATE_VISIBLE_LIST_ITEM Downloads",
      "WAIT_HEADER Downloads 15000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Download EPUB" }
    End = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Downloads" }
    Validate = {
      param($Events)
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_list_item" -and $e.value -eq "Downloads" }
      Assert-StateDiagnostics -State $endEvent.state -Label "hako_downloads_visible end state" -Expected @{
        totalJobCount = { param($value) $value -ge 1 }
        activeJobCount = { param($value) $value -ge 1 }
      }
    }
  },
  @{
    Name = "story_library_many_items_cold_visible"
    ResetSession = $false
    Setup = {
      param($Paths)
      Reset-BenchmarkSessionData -Paths $Paths
      Write-BenchmarkTrackedSeriesFixture -Paths $Paths -Count 10
    }
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
      "WAIT_HEADER Story Library 15000",
      "WAIT_LIST_ITEM Benchmark Story 01 10000",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Online Library" }
    End = { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "list_item" -and $e.value -eq "Benchmark Story 01" }
    Validate = {
      param($Events)
      $startEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "action_completed" -and $e.action -eq "activate_visible_menu_item" -and $e.value -eq "Online Library" }
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "wait_satisfied" -and $e.waitType -eq "list_item" -and $e.value -eq "Benchmark Story 01" }
      Assert-StateDiagnostics -State $startEvent.state -Label "story_library_many_items_cold_visible start state" -Expected @{
        trackedSeriesCount = 10
      }
      Assert-StateDiagnostics -State $endEvent.state -Label "story_library_many_items_cold_visible end state" -Expected @{
        trackedSeriesLoaded = $true
        trackedSeriesCount = 10
      }
    }
  },
  @{
    Name = "story_library_many_items_warm_visible"
    ResetSession = $false
    Setup = {
      param($Paths)
      Reset-BenchmarkSessionData -Paths $Paths
      Write-BenchmarkTrackedSeriesFixture -Paths $Paths -Count 10
    }
    Commands = @(
      "WAIT_MENU_ITEM Online Library 20000",
      "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
      "WAIT_HEADER Online Library 15000",
      "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
      "WAIT_HEADER Story Library 15000",
      "WAIT_LIST_ITEM Benchmark Story 01 10000",
      "GO_BACK 10000",
      "WAIT_HEADER Online Library 10000",
      "GET_STATE",
      "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
      "WAIT_HEADER Story Library 15000",
      "WAIT_LIST_ITEM Benchmark Story 01 10000",
      "GET_STATE",
      "QUIT"
    )
    Start = { param($e) $e.event -eq "state" -and $e.state.currentActivity -eq "OnlineLibrary" -and $e.state.screen.diagnostics.trackedSeriesLoaded -eq $true }
    End = { param($e) $e.event -eq "state" -and $e.state.currentActivity -eq "TrackedSeries" -and $e.state.screen.list.selectedTitle -eq "Benchmark Story 01" }
    Validate = {
      param($Events)
      $startEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "state" -and $e.state.currentActivity -eq "OnlineLibrary" -and $e.state.screen.diagnostics.trackedSeriesLoaded -eq $true }
      $endEvent = Get-MatchingEvent -Events $Events -Predicate { param($e) $e.event -eq "state" -and $e.state.currentActivity -eq "TrackedSeries" -and $e.state.screen.list.selectedTitle -eq "Benchmark Story 01" }
      Assert-StateDiagnostics -State $startEvent.state -Label "story_library_many_items_warm_visible start state" -Expected @{
        trackedSeriesLoaded = $true
        trackedSeriesCount = 10
      }
      Assert-StateDiagnostics -State $endEvent.state -Label "story_library_many_items_warm_visible end state" -Expected @{
        trackedSeriesLoaded = $true
        trackedSeriesCount = 10
      }
    }
  }
)

$sessionMutex = Acquire-EmulatorSessionLock
try {
  $results = New-Object System.Collections.Generic.List[object]

  foreach ($benchmark in $benchmarks) {
    $setup = $null
    if ($benchmark.ContainsKey("Setup")) {
      $setup = $benchmark.Setup
    }
    $resetSession = $true
    if ($benchmark.ContainsKey("ResetSession")) {
      $resetSession = [bool]$benchmark.ResetSession
    }
    $events = Invoke-BenchmarkRun -Commands $benchmark.Commands -ResolvedEmulatorPath $resolvedEmulatorPath -TimeoutMs $TimeoutMs -DelayCapMs $DelayCapMs -Setup $setup -ResetSession $resetSession
    if ($benchmark.ContainsKey("Validate") -and $null -ne $benchmark.Validate) {
      & $benchmark.Validate $events
    }
    $startMs = Get-EventMillis -Events $events -Predicate $benchmark.Start
    $endMs = Get-EventMillis -Events $events -Predicate $benchmark.End
    if ($null -eq $startMs -or $null -eq $endMs) {
      throw "Missing benchmark markers for '$($benchmark.Name)'"
    }
    $results.Add([pscustomobject]@{
        Name = $benchmark.Name
        StartMs = $startMs
        EndMs = $endMs
        DurationMs = ($endMs - $startMs)
      })
  }

  $results | Sort-Object DurationMs -Descending | Format-Table -AutoSize
} finally {
  Release-EmulatorSessionLock -Mutex $sessionMutex
}
