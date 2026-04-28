param(
  [string]$Case = "all",
  [string]$EmulatorPath = "",
  [int]$TimeoutMs = 180000,
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

  $payload = $payload.Substring($firstBrace, $lastBrace - $firstBrace + 1)
  try {
    return ($payload | ConvertFrom-Json -ErrorAction Stop)
  } catch {
    return $null
  }
}

function Format-SimctlStateSummary {
  param($State)

  if ($null -eq $State) {
    return "state unavailable"
  }

  $segments = New-Object System.Collections.Generic.List[string]
  if ($State.PSObject.Properties.Name -contains "pendingAction" -and $State.pendingAction) {
    $segments.Add("pendingAction=$($State.pendingAction)")
  }
  if ($State.PSObject.Properties.Name -contains "pendingActivity" -and $State.pendingActivity) {
    $segments.Add("pendingActivity=$($State.pendingActivity)")
  }

  if ($State.PSObject.Properties.Name -contains "screen" -and $null -ne $State.screen) {
    if ($State.screen.PSObject.Properties.Name -contains "headerTitle" -and $State.screen.headerTitle) {
      $segments.Add("header=$($State.screen.headerTitle)")
    }
    if ($State.screen.PSObject.Properties.Name -contains "selectedTitle" -and $State.screen.selectedTitle) {
      $segments.Add("selected=$($State.screen.selectedTitle)")
    }
    if ($State.screen.PSObject.Properties.Name -contains "popupMessage" -and $State.screen.popupMessage) {
      $segments.Add("popup=$($State.screen.popupMessage)")
    }
  }

  if ($segments.Count -eq 0) {
    return "state captured"
  }
  return ($segments -join ", ")
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

function Write-SimctlProgress {
  param(
    [string]$CaseName,
    [string]$Line
  )

  $event = ConvertFrom-SimctlLine -Line $Line
  if ($null -eq $event) {
    return
  }

  if ($event.PSObject.Properties.Name -notcontains "event") {
    return
  }

  switch ($event.event) {
    "wait_started" {
      Write-Host ("  [{0}] wait {1}: {2} (timeout {3} ms)" -f $CaseName, $event.waitType, $event.value, $event.timeoutMs)
      return
    }
    "wait_satisfied" {
      Write-Host ("  [{0}] ready {1}: {2}" -f $CaseName, $event.waitType, $event.matched)
      return
    }
    "wait_progress" {
      Write-Host ("  [{0}] waiting {1}: {2} ({3}/{4} ms) | {5}" -f $CaseName, $event.waitType, $event.value,
        $event.elapsedMs, $event.timeoutMs, (Format-SimctlStateSummary $event.state))
      return
    }
    "action_started" {
      $value = if ($event.PSObject.Properties.Name -contains "value" -and $event.value) { ": $($event.value)" } else { "" }
      Write-Host ("  [{0}] action {1}{2}" -f $CaseName, $event.action, $value)
      return
    }
    "action_completed" {
      $value = if ($event.PSObject.Properties.Name -contains "matched" -and $event.matched) { ": $($event.matched)" } else { "" }
      Write-Host ("  [{0}] done {1}{2}" -f $CaseName, $event.action, $value)
      return
    }
    "wait_timeout" {
      Write-Host ("  [{0}] timeout {1}: {2} | {3}" -f $CaseName, $event.waitType, $event.value, (Format-SimctlStateSummary $event.state))
      return
    }
    "action_failed" {
      Write-Host ("  [{0}] failed {1}: {2} | {3}" -f $CaseName, $event.action, $event.error, (Format-SimctlStateSummary $event.state))
      return
    }
  }
}

function Get-SmokeSessionPaths {
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
    RepoRoot = $repoRoot
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

function Reset-SmokeSessionData {
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

function Write-TrackedSeriesFixture {
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
      id = "fixture-$pluginId-$padded"
      pluginId = $pluginId
      runtimeProfile = $runtimeProfile
      title = "Fixture Story $padded"
      author = "Author $padded"
      seriesUrl = "https://f/$padded"
      coverUrl = ""
      epubPath = ""
      lastChapterUrl = "https://f/$padded/c"
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

function Write-TrackedSeriesFilterFixture {
  param($Paths)

  $null = New-Item -ItemType Directory -Force -Path $Paths.DataRoot
  $libraryRoot = Join-Path $Paths.SdcardRoot "Online Library"
  $null = New-Item -ItemType Directory -Force -Path $libraryRoot

  $downloadedPath = Join-Path $libraryRoot "Downloaded Fixture.epub"
  $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($downloadedPath, "fixture", $utf8NoBom)

  $items = @(
    [ordered]@{
      id = "fixture-reading-current"
      pluginId = "hako"
      runtimeProfile = "hako"
      title = "Reading Current"
      author = "Fixture"
      seriesUrl = "https://f/reading-current"
      coverUrl = ""
      epubPath = ""
      lastChapterUrl = "https://f/reading-current/ch-2"
      lastChapterTitle = "Ch 2"
      lastReadChapterUrl = "https://f/reading-current/ch-2"
      lastReadChapterTitle = "Ch 2"
      lastReadPage = 2
      lastReadPageCount = 8
      chapterCount = 2
    },
    [ordered]@{
      id = "fixture-needs-update"
      pluginId = "hako"
      runtimeProfile = "hako"
      title = "Needs Update"
      author = "Fixture"
      seriesUrl = "https://f/needs-update"
      coverUrl = ""
      epubPath = ""
      lastChapterUrl = "https://f/needs-update/ch-5"
      lastChapterTitle = "Ch 5"
      lastReadChapterUrl = "https://f/needs-update/ch-4"
      lastReadChapterTitle = "Ch 4"
      lastReadPage = 10
      lastReadPageCount = 12
      chapterCount = 5
    },
    [ordered]@{
      id = "fixture-downloaded-library"
      pluginId = "truyenfull"
      runtimeProfile = "truyenfull"
      title = "Downloaded Library"
      author = "Fixture"
      seriesUrl = "https://f/downloaded-library"
      coverUrl = ""
      epubPath = "/Online Library/Downloaded Fixture.epub"
      lastChapterUrl = "https://f/downloaded-library/ch-7"
      lastChapterTitle = "Ch 7"
      lastReadChapterUrl = ""
      lastReadChapterTitle = ""
      lastReadPage = 0
      lastReadPageCount = 0
      chapterCount = 7
    }
  )

  $payload = [ordered]@{ items = $items } | ConvertTo-Json -Depth 5 -Compress
  [System.IO.File]::WriteAllText($Paths.TrackedSeriesPath, $payload, $utf8NoBom)
}

function Write-TrackedSeriesDownloadedDetailFixture {
  param($Paths)

  $null = New-Item -ItemType Directory -Force -Path $Paths.DataRoot
  $libraryRoot = Join-Path $Paths.SdcardRoot "Online Library"
  $null = New-Item -ItemType Directory -Force -Path $libraryRoot

  $downloadedPath = Join-Path $libraryRoot "Bach Luyen Thanh Tien.epub"
  $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($downloadedPath, "fixture", $utf8NoBom)

  $items = @(
    [ordered]@{
      id = "fixture-downloaded-real-detail"
      pluginId = "truyenfull"
      runtimeProfile = "truyenfull"
      title = "Bach Luyen Thanh Tien"
      author = "Fixture"
      seriesUrl = "https://truyenfull.vision/truyen-bach-luyen-thanh-tien-837581/"
      coverUrl = ""
      epubPath = "/Online Library/Bach Luyen Thanh Tien.epub"
      lastChapterUrl = "https://truyenfull.vision/truyen-bach-luyen-thanh-tien-837581/chuong-1/"
      lastChapterTitle = "Chuong 1"
      lastReadChapterUrl = ""
      lastReadChapterTitle = ""
      lastReadPage = 0
      lastReadPageCount = 0
      chapterCount = 2850
    }
  )

  $payload = [ordered]@{ items = $items } | ConvertTo-Json -Depth 5 -Compress
  [System.IO.File]::WriteAllText($Paths.TrackedSeriesPath, $payload, $utf8NoBom)
}

function Write-DownloadJobsFixture {
  param(
    $Paths,
    [string]$Mode = "mixed"
  )

  $null = New-Item -ItemType Directory -Force -Path $Paths.DataRoot
  $jobs = @()

  switch ($Mode) {
    "mixed" {
      $jobs = @(
        [ordered]@{
          id = "job-queued"
          kind = "hako_download"
          status = "queued"
          pluginId = "hako"
          runtimeProfile = "hako"
          title = "Queued Download"
          author = "Fixture"
          seriesUrl = "https://d/queued"
          epubPath = ""
          trackedSeriesId = "tracked-queued"
          totalChapters = 20
          completedChapters = 0
          retryCount = 0
          nextRetryAtMs = 0
          createdAtMs = 1000
          updatedAtMs = 3000
          statusMessage = "Waiting in queue"
          currentChapterTitle = ""
        },
        [ordered]@{
          id = "job-failed"
          kind = "tracked_sync"
          status = "failed"
          pluginId = "truyenfull"
          runtimeProfile = "truyenfull"
          title = "Failed Download"
          author = "Fixture"
          seriesUrl = "https://d/failed"
          epubPath = ""
          trackedSeriesId = "tracked-failed"
          totalChapters = 12
          completedChapters = 4
          retryCount = 3
          nextRetryAtMs = 0
          createdAtMs = 1000
          updatedAtMs = 2000
          statusMessage = "Timeout"
          currentChapterTitle = "Ch 4"
        },
        [ordered]@{
          id = "job-completed"
          kind = "hako_sync"
          status = "completed"
          pluginId = "hako"
          runtimeProfile = "hako"
          title = "Completed Download"
          author = "Fixture"
          seriesUrl = "https://d/completed"
          epubPath = ""
          trackedSeriesId = "tracked-completed"
          totalChapters = 8
          completedChapters = 8
          retryCount = 0
          nextRetryAtMs = 0
          createdAtMs = 1000
          updatedAtMs = 1500
          statusMessage = "EPUB ready"
          currentChapterTitle = ""
        }
      )
    }
    "failed_only" {
      $jobs = @(
        [ordered]@{
          id = "job-failed-only"
          kind = "tracked_sync"
          status = "failed"
          pluginId = "truyenfull"
          runtimeProfile = "truyenfull"
          title = "Retry Target"
          author = "Fixture"
          seriesUrl = "https://d/retry"
          epubPath = ""
          trackedSeriesId = "tracked-retry"
          totalChapters = 16
          completedChapters = 2
          retryCount = 2
          nextRetryAtMs = 0
          createdAtMs = 1000
          updatedAtMs = 4000
          statusMessage = "Proxy blocked"
          currentChapterTitle = "Ch 2"
        }
      )
    }
  }

  $payload = [ordered]@{ jobs = $jobs } | ConvertTo-Json -Depth 5 -Compress
  $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
  [System.IO.File]::WriteAllText($Paths.DownloadJobsPath, $payload, $utf8NoBom)
}

function Get-SmokeCases {
  return @(
    @{
      Name = "search_preview_cover_unavailable"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Web Truyen",
        "WAIT_HEADER Web Truyen 30000",
        "TAP LEFT 180",
        "WAIT_ACTIVITY KeyboardEntry 10000",
        "TYPE_TEXT kiem",
        "WAIT_LIST_ITEM Kiem dao de Nhat Tien 20000",
        "WAIT_BODY Cover unavailable 10000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.screen.headerTitle -ne "Web Truyen") {
          throw "Expected headerTitle 'Web Truyen' but got '$($state.screen.headerTitle)'"
        }
        if ($state.screen.bodyPrimaryText -ne "Summary") {
          throw "Expected summary preview body but got '$($state.screen.bodyPrimaryText)'"
        }
        if ($state.screen.bodyTertiaryText -ne "Cover unavailable") {
          throw "Expected tertiary cover status 'Cover unavailable' but got '$($state.screen.bodyTertiaryText)'"
        }
      }
    },
    @{
      Name = "add_to_library_and_story_library_preview"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Web Truyen",
        "WAIT_HEADER Web Truyen 30000",
        "TAP LEFT 180",
        "WAIT_ACTIVITY KeyboardEntry 10000",
        "TYPE_TEXT kiem",
        "WAIT_LIST_ITEM Kiem dao de Nhat Tien 20000",
        "WAIT_BODY Cover unavailable 10000",
        "ACTIVATE_VISIBLE_LIST_ITEM Kiem dao de Nhat Tien",
        "WAIT_ACTIVITY HakoDetail 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Add to Library",
        "WAIT_POPUP Added to library 10000",
        "GO_BACK 10000",
        "WAIT_HEADER Web Truyen 10000",
        "GO_BACK 10000",
        "WAIT_HEADER Sources 10000",
        "GO_BACK 10000",
        "WAIT_HEADER Online Library 10000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "WAIT_BODY Cover unavailable 10000",
        "GET_SCREEN",
        "QUIT"
      )
      Assert = {
        param($Result)
        $screen = $Result.LastScreen
        if (-not $screen) {
          throw "Missing GET_SCREEN output"
        }
        if ($screen.headerTitle -ne "Story Library") {
          throw "Expected headerTitle 'Story Library' but got '$($screen.headerTitle)'"
        }
        if ($screen.bodyPrimaryText -ne "Summary") {
          throw "Expected bodyPrimaryText 'Summary' but got '$($screen.bodyPrimaryText)'"
        }
        if ($screen.bodyTertiaryText -ne "Cover unavailable") {
          throw "Expected tertiary cover status 'Cover unavailable' but got '$($screen.bodyTertiaryText)'"
        }
      }
    },
    @{
      Name = "browse_chapters_latest_page"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Web Truyen",
        "WAIT_HEADER Web Truyen 30000",
        "TAP LEFT 180",
        "WAIT_ACTIVITY KeyboardEntry 10000",
        "TYPE_TEXT kiem",
        "WAIT_LIST_ITEM Kiem dao de Nhat Tien 20000",
        "ACTIVATE_VISIBLE_LIST_ITEM Kiem dao de Nhat Tien",
        "WAIT_ACTIVITY HakoDetail 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Browse Chapters",
        "WAIT_ACTIVITY HakoChapters 30000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.currentActivity -ne "HakoChapters") {
          throw "Expected currentActivity 'HakoChapters' but got '$($state.currentActivity)'"
        }
        if ($state.screen.headerSubtitle -notlike "*Page 75/75*") {
          throw "Expected HakoChapters headerSubtitle to contain 'Page 75/75' but got '$($state.screen.headerSubtitle)'"
        }
        if ($state.screen.list.selectedTitle -notlike "*Chuong 3719*") {
          throw "Expected selected chapter to contain 'Chuong 3719' but got '$($state.screen.list.selectedTitle)'"
        }
      }
    },
    @{
      Name = "sources_list_has_truyen_full"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "WAIT_LIST_ITEM Truyen Full 10000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.screen.headerTitle -ne "Sources") {
          throw "Expected headerTitle 'Sources' but got '$($state.screen.headerTitle)'"
        }
        $labels = @($state.screen.list.visibleItems | ForEach-Object { $_.title })
        if ($labels -notcontains "Truyen Full") {
          throw "Expected 'Truyen Full' to be visible in source list"
        }
      }
    },
    @{
      Name = "sources_selection_persists_after_back"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "TAP DOWN 180",
        "WAIT_SELECTED_LIST_ITEM Truyen Full 10000",
        "GET_STATE",
        "ACTIVATE_VISIBLE_LIST_ITEM Truyen Full",
        "WAIT_HEADER Truyen Full 15000",
        "GO_BACK 10000",
        "WAIT_HEADER Sources 10000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 2) {
          throw "Expected 2 state snapshots but saw $($stateEvents.Count)"
        }

        $beforeOpen = $stateEvents[0].state
        $afterBack = $stateEvents[1].state

        if ($beforeOpen.screen.list.selectedTitle -ne "Truyen Full") {
          throw "Expected initial selected source 'Truyen Full' but got '$($beforeOpen.screen.list.selectedTitle)'"
        }
        if ($afterBack.screen.headerTitle -ne "Sources") {
          throw "Expected to return to 'Sources' but got '$($afterBack.screen.headerTitle)'"
        }
        if ($afterBack.screen.list.selectedTitle -ne "Truyen Full") {
          throw "Expected source selection to stay on 'Truyen Full' after back but got '$($afterBack.screen.list.selectedTitle)'"
        }
      }
    },
    @{
      Name = "truyen_full_search_to_detail"
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
        "ACTIVATE_VISIBLE_LIST_ITEM Bach Luyen Thanh Tien",
        "WAIT_ACTIVITY HakoDetail 20000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.currentActivity -ne "HakoDetail") {
          throw "Expected currentActivity 'HakoDetail' but got '$($state.currentActivity)'"
        }
        if ($state.screen.headerTitle -ne "Bach Luyen Thanh Tien") {
          throw "Expected detail title 'Bach Luyen Thanh Tien' but got '$($state.screen.headerTitle)'"
        }
        if ($state.screen.buttonMenu.selectedLabel -ne "Read Latest") {
          throw "Expected selected action 'Read Latest' but got '$($state.screen.buttonMenu.selectedLabel)'"
        }
        $labels = @()
        if ($state.screen.buttonMenu.PSObject.Properties.Name -contains "labels" -and $null -ne $state.screen.buttonMenu.labels) {
          $labels = @($state.screen.buttonMenu.labels)
        }
        if ($labels -notcontains "Browse Chapters") {
          throw "Expected detail menu to include 'Browse Chapters'"
        }
        if ($labels -notcontains "Add to Library" -and $labels -notcontains "Remove from Library") {
          throw "Expected detail menu to include a library tracking action but got '$(($labels -join ', '))'"
        }
      }
    },
    @{
      Name = "truyen_full_browse_chapters"
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
        "ACTIVATE_VISIBLE_LIST_ITEM Bach Luyen Thanh Tien",
        "WAIT_ACTIVITY HakoDetail 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Browse Chapters",
        "WAIT_ACTIVITY HakoChapters 30000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        $state = if ($stateEvents.Count -gt 0) { $stateEvents[-1].state } else { $Result.LastState }
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.currentActivity -ne "HakoChapters") {
          throw "Expected currentActivity 'HakoChapters' but got '$($state.currentActivity)'"
        }
        if ($state.screen.headerSubtitle -notlike "*Bach Luyen Thanh Tien*") {
          throw "Expected chapter header subtitle to contain 'Bach Luyen Thanh Tien' but got '$($state.screen.headerSubtitle)'"
        }
        if ($state.screen.list.selectedTitle -notlike "*Chuong*") {
          throw "Expected selected chapter title to contain 'Chuong' but got '$($state.screen.list.selectedTitle)'"
        }
      }
    },
    @{
      Name = "hako_download_queue_visible"
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
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.currentActivity -ne "Downloads") {
          throw "Expected currentActivity 'Downloads' but got '$($state.currentActivity)'"
        }
        if ($state.screen.list.selectedTitle -notlike "*Re:Zero kara Hajimeru Isekai Seikatsu*") {
          throw "Expected queued job title to contain 'Re:Zero kara Hajimeru Isekai Seikatsu' but got '$($state.screen.list.selectedTitle)'"
        }
        if ($state.screen.list.selectedSubtitle -notmatch "Queued|Updating|Retry wait") {
          throw "Expected active download status but got '$($state.screen.list.selectedSubtitle)'"
        }
      }
    },
    @{
      Name = "hako_search_page_size"
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
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $state = $Result.LastState
        if (-not $state) {
          throw "Missing GET_STATE output"
        }
        if ($state.currentActivity -ne "HakoSearch") {
          throw "Expected currentActivity 'HakoSearch' but got '$($state.currentActivity)'"
        }
        if ($state.screen.list.itemCount -ne 21) {
          throw "Expected Hako search page itemCount 21 (20 results + Next Page) but got '$($state.screen.list.itemCount)'"
        }
      }
    },
    @{
      Name = "hako_search_right_refresh_and_home"
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Sources",
        "WAIT_HEADER Sources 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Hako",
        "WAIT_HEADER Hako 30000",
        "GET_STATE",
        "TAP RIGHT 180",
        "WAIT_BODY Loading source... 5000",
        "WAIT_BODY Summary 20000",
        "GET_STATE",
        "TAP LEFT 180",
        "WAIT_ACTIVITY KeyboardEntry 10000",
        "TYPE_TEXT re zero",
        "WAIT_LIST_ITEM Re:Zero Kara Hajimeru Isekai Seikatsu 25000",
        "GET_STATE",
        "TAP RIGHT 180",
        "WAIT_BODY Pause to load source... 10000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $initialEvent = $Result.Events | Where-Object {
          $_.event -eq "wait_satisfied" -and $_.waitType -eq "header" -and $_.value -eq "Hako"
        } | Select-Object -First 1
        $refreshedEvent = $Result.Events | Where-Object {
          $_.event -eq "wait_satisfied" -and $_.waitType -eq "body" -and $_.value -eq "Summary"
        } | Select-Object -First 1
        $searchedEvent = $Result.Events | Where-Object {
          $_.event -eq "wait_satisfied" -and $_.waitType -eq "list_item" -and $_.value -eq "Re:Zero Kara Hajimeru Isekai Seikatsu"
        } | Select-Object -First 1
        $returnedHomeEvent = $Result.Events | Where-Object {
          $_.event -eq "wait_satisfied" -and $_.waitType -eq "body" -and $_.value -eq "Pause to load source..."
        } | Select-Object -First 1

        if (-not $initialEvent -or -not $refreshedEvent -or -not $searchedEvent -or -not $returnedHomeEvent) {
          throw "Missing one or more required wait_satisfied checkpoints for refresh/home behavior"
        }

        $initial = $initialEvent.state
        $refreshed = $refreshedEvent.state
        $searched = $searchedEvent.state
        $returnedHome = $returnedHomeEvent.state

        if ($initial.currentActivity -ne "HakoSearch") {
          throw "Expected initial activity 'HakoSearch' but got '$($initial.currentActivity)'"
        }
        if ($initial.screen.buttonHints.btn4 -ne "Refresh") {
          throw "Expected initial btn4 hint 'Refresh' but got '$($initial.screen.buttonHints.btn4)'"
        }
        if ($initial.screen.headerSubtitle -ne "Home | LEFT search") {
          throw "Expected initial home subtitle but got '$($initial.screen.headerSubtitle)'"
        }

        if ($refreshed.screen.list.itemCount -le 0) {
          throw "Expected Refresh to load home feed items but itemCount was '$($refreshed.screen.list.itemCount)'"
        }
        if ($refreshed.screen.bodyPrimaryText -ne "Summary") {
          throw "Expected refreshed state bodyPrimaryText 'Summary' but got '$($refreshed.screen.bodyPrimaryText)'"
        }
        if ($refreshed.screen.buttonHints.btn4 -ne "Refresh") {
          throw "Expected refreshed btn4 hint 'Refresh' but got '$($refreshed.screen.buttonHints.btn4)'"
        }

        if ($searched.screen.headerSubtitle -notlike "re zero | Page 1") {
          throw "Expected search subtitle 're zero | Page 1' but got '$($searched.screen.headerSubtitle)'"
        }
        if ($searched.screen.buttonHints.btn4 -ne "Home") {
          throw "Expected search btn4 hint 'Home' but got '$($searched.screen.buttonHints.btn4)'"
        }
        if ($searched.screen.list.itemCount -ne 21) {
          throw "Expected searched itemCount 21 but got '$($searched.screen.list.itemCount)'"
        }

        if ($returnedHome.screen.headerSubtitle -ne "Home | LEFT search") {
          throw "Expected returned home subtitle but got '$($returnedHome.screen.headerSubtitle)'"
        }
        if ($returnedHome.screen.bodyPrimaryText -ne "Pause to load source...") {
          throw "Expected returned home bodyPrimaryText 'Pause to load source...' but got '$($returnedHome.screen.bodyPrimaryText)'"
        }
        if ($returnedHome.screen.buttonHints.btn4 -ne "Refresh") {
          throw "Expected returned home btn4 hint 'Refresh' but got '$($returnedHome.screen.buttonHints.btn4)'"
        }
        if ($returnedHome.screen.list.itemCount -ne 0) {
          throw "Expected returned home to clear results list but itemCount was '$($returnedHome.screen.list.itemCount)'"
        }
      }
    },
    @{
      Name = "story_library_many_items_navigation"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-TrackedSeriesFixture -Paths $Paths -Count 10
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "WAIT_LIST_ITEM Fixture Story 01 10000",
        "GET_STATE",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_MS 240",
        "TAP DOWN 180",
        "WAIT_HEADER Page 2/ 10000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        if ($Result.Events.Count -lt 2) {
          throw "Expected multiple state snapshots for stress case"
        }
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 2) {
          throw "Expected at least 2 state events but saw $($stateEvents.Count)"
        }

        $initial = $stateEvents[0].state
        $afterMove = $stateEvents[-1].state

        if ($initial.currentActivity -ne "TrackedSeries") {
          throw "Expected initial activity 'TrackedSeries' but got '$($initial.currentActivity)'"
        }
        if ($initial.screen.list.itemCount -ne 10) {
          throw "Expected 10 tracked stories but got '$($initial.screen.list.itemCount)'"
        }
        Assert-StateDiagnostics -State $initial -Label "Story Library initial state" -Expected @{
          trackedSeriesLoaded = $true
          trackedSeriesCount = 10
        }
        if ($initial.screen.list.selectedTitle -ne "Fixture Story 01") {
          throw "Expected first selected story to be 'Fixture Story 01' but got '$($initial.screen.list.selectedTitle)'"
        }
        if ($initial.screen.headerSubtitle -notmatch "1-7/10") {
          throw "Expected initial header subtitle to expose visible range 1-7/10, got '$($initial.screen.headerSubtitle)'"
        }
        if ($initial.screen.headerSubtitle -notmatch "Page 1/") {
          throw "Expected initial header subtitle to expose page 1, got '$($initial.screen.headerSubtitle)'"
        }

        if ($afterMove.screen.headerSubtitle -notmatch "8-10/10") {
          throw "Expected moved header subtitle to expose visible range 8-10/10, got '$($afterMove.screen.headerSubtitle)'"
        }
        Assert-StateDiagnostics -State $afterMove -Label "Story Library moved state" -Expected @{
          trackedSeriesLoaded = $true
          trackedSeriesCount = 10
        }
        if ($afterMove.screen.headerSubtitle -notmatch "Page 2/") {
          throw "Expected moved header subtitle to expose page 2, got '$($afterMove.screen.headerSubtitle)'"
        }
        if ($afterMove.screen.list.selectedTitle -eq $initial.screen.list.selectedTitle) {
          throw "Expected selection to move to another story"
        }
        if ($afterMove.screen.list.selectedVisibleIndex -lt 0) {
          throw "Expected moved selection to remain visible on page 2, got '$($afterMove.screen.list.selectedVisibleIndex)'"
        }
        if ($afterMove.screen.bodyPrimaryText -ne "Summary") {
          throw "Expected Story Library preview label 'Summary' but got '$($afterMove.screen.bodyPrimaryText)'"
        }
      }
    },
    @{
      Name = "story_library_filter_and_sort"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-TrackedSeriesFilterFixture -Paths $Paths
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "WAIT_LIST_ITEM Needs Update 10000",
        "GET_STATE",
        "HOLD LEFT 900",
        "WAIT_POPUP Filter: Reading 5000",
        "GET_STATE",
        "HOLD LEFT 900",
        "WAIT_POPUP Filter: Needs Update 5000",
        "GET_STATE",
        "HOLD LEFT 900",
        "WAIT_POPUP Filter: Downloaded 5000",
        "GET_STATE",
        "HOLD RIGHT 900",
        "WAIT_POPUP Sort: Title 5000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 5) {
          throw "Expected at least 5 state snapshots but saw $($stateEvents.Count)"
        }

        $initial = $stateEvents[0].state
        $reading = $stateEvents[1].state
        $needsUpdate = $stateEvents[2].state
        $downloaded = $stateEvents[3].state
        $sorted = $stateEvents[4].state

        if ($initial.screen.list.itemCount -ne 3) {
          throw "Expected initial library count 3 but got '$($initial.screen.list.itemCount)'"
        }
        if ($initial.screen.list.selectedTitle -ne "Needs Update") {
          throw "Expected status sort to surface 'Needs Update' first but got '$($initial.screen.list.selectedTitle)'"
        }

        if ($reading.screen.headerSubtitle -notmatch "^Reading 2/3") {
          throw "Expected Reading filter subtitle but got '$($reading.screen.headerSubtitle)'"
        }
        $readingTitles = @($reading.screen.list.visibleItems | ForEach-Object { $_.title })
        if ($reading.screen.list.itemCount -ne 2 -or $readingTitles -notcontains "Reading Current" -or $readingTitles -notcontains "Needs Update") {
          throw "Expected Reading filter to keep both reading stories visible but got '$($readingTitles -join ', ')'"
        }

        if ($needsUpdate.screen.headerSubtitle -notmatch "^Needs Update 1/3") {
          throw "Expected Needs Update filter subtitle but got '$($needsUpdate.screen.headerSubtitle)'"
        }
        if ($needsUpdate.screen.list.itemCount -ne 1 -or $needsUpdate.screen.list.selectedTitle -ne "Needs Update") {
          throw "Expected Needs Update filter to isolate 'Needs Update' but got '$($needsUpdate.screen.list.selectedTitle)'"
        }

        if ($downloaded.screen.headerSubtitle -notmatch "^Downloaded 1/3") {
          throw "Expected Downloaded filter subtitle but got '$($downloaded.screen.headerSubtitle)'"
        }
        if ($downloaded.screen.list.itemCount -ne 1 -or $downloaded.screen.list.selectedTitle -ne "Downloaded Library") {
          throw "Expected Downloaded filter to isolate 'Downloaded Library' but got '$($downloaded.screen.list.selectedTitle)'"
        }

        if ($sorted.screen.headerSubtitle -notmatch "Sort Title") {
          throw "Expected sort subtitle to switch to Title but got '$($sorted.screen.headerSubtitle)'"
        }
        if ($sorted.screen.list.selectedTitle -ne "Downloaded Library") {
          throw "Expected Title sort within Downloaded filter to keep 'Downloaded Library' selected but got '$($sorted.screen.list.selectedTitle)'"
        }
      }
    },
    @{
      Name = "story_library_persists_filter_sort"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-TrackedSeriesFilterFixture -Paths $Paths
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "WAIT_SELECTED_LIST_ITEM Needs Update 5000",
        "HOLD LEFT 900",
        "WAIT_POPUP Filter: Reading 5000",
        "HOLD RIGHT 900",
        "WAIT_POPUP Sort: Title 5000",
        "GET_STATE",
        "GO_BACK 10000",
        "WAIT_HEADER Online Library 10000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 2) {
          throw "Expected 2 state snapshots but saw $($stateEvents.Count)"
        }

        $configured = $stateEvents[0].state
        $reopened = $stateEvents[1].state

        if ($configured.screen.headerSubtitle -notmatch "^Reading 2/3") {
          throw "Expected configured filter subtitle to start with 'Reading 2/3' but got '$($configured.screen.headerSubtitle)'"
        }
        if ($configured.screen.headerSubtitle -notmatch "Sort Title") {
          throw "Expected configured sort subtitle to contain 'Sort Title' but got '$($configured.screen.headerSubtitle)'"
        }

        if ($reopened.screen.headerTitle -ne "Story Library") {
          throw "Expected reopened header 'Story Library' but got '$($reopened.screen.headerTitle)'"
        }
        if ($reopened.screen.headerSubtitle -notmatch "^Reading 2/3") {
          throw "Expected reopened filter subtitle to start with 'Reading 2/3' but got '$($reopened.screen.headerSubtitle)'"
        }
        if ($reopened.screen.headerSubtitle -notmatch "Sort Title") {
          throw "Expected reopened sort subtitle to contain 'Sort Title' but got '$($reopened.screen.headerSubtitle)'"
        }
        if ($reopened.screen.list.selectedTitle -ne "Needs Update") {
          throw "Expected reopened selection to stay on 'Needs Update' but got '$($reopened.screen.list.selectedTitle)'"
        }
      }
    },
    @{
      Name = "story_library_downloaded_item_opens_detail"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-TrackedSeriesDownloadedDetailFixture -Paths $Paths
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Story Library",
        "WAIT_HEADER Story Library 15000",
        "WAIT_SELECTED_LIST_ITEM Bach Luyen Thanh Tien 5000",
        "ACTIVATE_VISIBLE_LIST_ITEM Bach Luyen Thanh Tien",
        "WAIT_ACTIVITY HakoDetail 15000",
        "WAIT_HEADER Bach Luyen Thanh Tien 15000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 1) {
          throw "Expected at least 1 state snapshot but saw $($stateEvents.Count)"
        }

        $detailState = $stateEvents[0].state
        if ($detailState.currentActivity -ne "HakoDetail") {
          throw "Expected downloaded Story Library item to open HakoDetail but got '$($detailState.currentActivity)'"
        }
        if ($detailState.screen.headerTitle -ne "Bach Luyen Thanh Tien") {
          throw "Expected detail header 'Bach Luyen Thanh Tien' but got '$($detailState.screen.headerTitle)'"
        }
        if ($detailState.screen.buttonMenu.labels -notcontains "Browse Chapters") {
          throw "Expected detail actions to include 'Browse Chapters' but got '$(($detailState.screen.buttonMenu.labels -join ', '))'"
        }
      }
    },
    @{
      Name = "downloads_cancel_and_clear_done"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-DownloadJobsFixture -Paths $Paths -Mode mixed
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Downloads",
        "WAIT_HEADER Downloads 15000",
        "WAIT_LIST_ITEM Queued Download 10000",
        "GET_STATE",
        "TAP LEFT 180",
        "WAIT_POPUP Cancelled 5000",
        "GET_STATE",
        "TAP RIGHT 180",
        "WAIT_POPUP Cleared finished jobs 5000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 3) {
          throw "Expected 3 state snapshots but saw $($stateEvents.Count)"
        }
        $initial = $stateEvents[0].state
        $afterCancel = $stateEvents[1].state
        $afterClear = $stateEvents[2].state

        if ($initial.screen.list.itemCount -ne 3) {
          throw "Expected 3 download jobs initially but got '$($initial.screen.list.itemCount)'"
        }
        Assert-StateDiagnostics -State $initial -Label "Downloads initial state" -Expected @{
          totalJobCount = 3
          activeJobCount = 1
        }
        if ($initial.screen.list.selectedTitle -ne "Queued Download") {
          throw "Expected active queued job selected first but got '$($initial.screen.list.selectedTitle)'"
        }
        if ($initial.screen.headerSubtitle -notmatch "^1 active \| 3 total") {
          throw "Expected downloads header to expose active/total counts but got '$($initial.screen.headerSubtitle)'"
        }

        if ($afterCancel.screen.list.selectedSubtitle -notmatch "Cancelled") {
          throw "Expected selected job subtitle to show Cancelled but got '$($afterCancel.screen.list.selectedSubtitle)'"
        }
        Assert-StateDiagnostics -State $afterCancel -Label "Downloads after cancel state" -Expected @{
          totalJobCount = 3
          activeJobCount = 0
        }
        if ($afterCancel.screen.headerSubtitle -notmatch "^0 active \| 3 total") {
          throw "Expected active count to drop after cancel but got '$($afterCancel.screen.headerSubtitle)'"
        }

        if ($afterClear.screen.list.itemCount -ne 0) {
          throw "Expected clear done to remove all terminal jobs but got '$($afterClear.screen.list.itemCount)'"
        }
        Assert-StateDiagnostics -State $afterClear -Label "Downloads after clear state" -Expected @{
          totalJobCount = 0
          activeJobCount = 0
        }
        if ($afterClear.screen.headerSubtitle -ne "Queue background jobs") {
          throw "Expected empty downloads subtitle after clear but got '$($afterClear.screen.headerSubtitle)'"
        }
      }
    },
    @{
      Name = "downloads_retry_failed_job"
      ResetSession = $false
      Setup = {
        param($Paths)
        Reset-SmokeSessionData -Paths $Paths
        Write-DownloadJobsFixture -Paths $Paths -Mode failed_only
      }
      Commands = @(
        "WAIT_MENU_ITEM Online Library 20000",
        "ACTIVATE_VISIBLE_MENU_ITEM Online Library",
        "WAIT_HEADER Online Library 15000",
        "ACTIVATE_VISIBLE_LIST_ITEM Downloads",
        "WAIT_HEADER Downloads 15000",
        "WAIT_LIST_ITEM Retry Target 10000",
        "GET_STATE",
        "TAP CONFIRM 180",
        "WAIT_POPUP Retry queued 5000",
        "GET_STATE",
        "QUIT"
      )
      Assert = {
        param($Result)
        $stateEvents = @($Result.Events | Where-Object { $_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "state" })
        if ($stateEvents.Count -lt 2) {
          throw "Expected 2 state snapshots but saw $($stateEvents.Count)"
        }
        $initial = $stateEvents[0].state
        $afterRetry = $stateEvents[1].state

        if ($initial.screen.list.selectedSubtitle -notmatch "Failed") {
          throw "Expected initial failed subtitle but got '$($initial.screen.list.selectedSubtitle)'"
        }
        Assert-StateDiagnostics -State $initial -Label "Downloads retry initial state" -Expected @{
          totalJobCount = 1
          activeJobCount = 0
        }
        if ($afterRetry.screen.list.selectedSubtitle -notmatch "Queued") {
          throw "Expected retried job subtitle to show Queued but got '$($afterRetry.screen.list.selectedSubtitle)'"
        }
        Assert-StateDiagnostics -State $afterRetry -Label "Downloads retry queued state" -Expected @{
          totalJobCount = 1
          activeJobCount = 1
        }
        if ($afterRetry.screen.headerSubtitle -notmatch "^1 active \| 1 total") {
          throw "Expected retry to make job active again but got '$($afterRetry.screen.headerSubtitle)'"
        }
      }
    }
  )
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
    $event = ConvertFrom-SimctlLine -Line $trimmedLine
    if ($null -eq $event) {
      throw "Malformed SIMCTL payload line: $trimmedLine"
    }
    $events.Add($event)
  }
  return $events
}

function Invoke-SmokeCase {
  param(
    [hashtable]$Definition,
    [string]$ResolvedEmulatorPath,
    [int]$TimeoutMs,
    [int]$DelayCapMs
  )

  $sessionPaths = Get-SmokeSessionPaths -ResolvedEmulatorPath $ResolvedEmulatorPath
  $shouldResetSession = $true
  if ($Definition.ContainsKey("ResetSession")) {
    $shouldResetSession = [bool]$Definition.ResetSession
  }
  if ($shouldResetSession) {
    Reset-SmokeSessionData -Paths $sessionPaths
  }
  if ($Definition.ContainsKey("Setup") -and $null -ne $Definition.Setup) {
    & $Definition.Setup $sessionPaths
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
  if ($shouldResetSession) {
    $arguments += "--reset-session"
  }
  $psi.Arguments = $arguments -join " "

  $proc = [System.Diagnostics.Process]::new()
  $proc.StartInfo = $psi

  if (-not $proc.Start()) {
    throw "Failed to start emulator process"
  }

  $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
  $stderrTask = $proc.StandardError.ReadToEndAsync()

  foreach ($command in $Definition.Commands) {
    $proc.StandardInput.WriteLine($command)
  }
  $proc.StandardInput.Close()

  if (-not $proc.WaitForExit($TimeoutMs)) {
    try { $proc.Kill() } catch {}
    throw "Timed out after ${TimeoutMs}ms"
  }

  $stdoutText = $stdoutTask.GetAwaiter().GetResult()
  $stderrText = $stderrTask.GetAwaiter().GetResult()
  $events = Parse-SimctlEvents -StdoutText $stdoutText

  $result = [ordered]@{
    Name = $Definition.Name
    ExitCode = $proc.ExitCode
    Stdout = $stdoutText
    Stderr = $stderrText
    Events = $events
    LastState = $null
    LastScreen = $null
  }

  foreach ($event in $events) {
    if ($null -eq $event -or $event.PSObject.Properties.Name -notcontains "event") {
      continue
    }
    if ($event.event -eq "state") {
      $result.LastState = $event.state
      if ($event.state.PSObject.Properties.Name -contains "screen" -and $null -ne $event.state.screen) {
        $result.LastScreen = $event.state.screen
      }
      continue
    }
    if ($event.event -eq "screen") {
      $result.LastScreen = $event.screen
      continue
    }
    if ($event.PSObject.Properties.Name -contains "state" -and $null -ne $event.state) {
      $result.LastState = $event.state
      if ($event.state.PSObject.Properties.Name -contains "screen" -and $null -ne $event.state.screen) {
        $result.LastScreen = $event.state.screen
      }
    }
  }

  if ($proc.ExitCode -ne 0) {
    throw "Emulator exited with code $($proc.ExitCode). stderr: $stderrText"
  }

  $failedEvents = @($events | Where-Object {
      ($_ -and $_.PSObject.Properties.Name -contains "ok" -and -not $_.ok) -or
      ($_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "wait_timeout") -or
      ($_ -and $_.PSObject.Properties.Name -contains "event" -and $_.event -eq "action_failed")
    })
  if ($failedEvents.Count -gt 0) {
    $firstFailure = $failedEvents[0]
    $failureJson = $firstFailure | ConvertTo-Json -Depth 16 -Compress
    $failureState = $null
    if ($firstFailure.PSObject.Properties.Name -contains "state") {
      $failureState = $firstFailure.state
    }
    throw "SIMCTL reported failure: $failureJson | $(Format-SimctlStateSummary -State $failureState)"
  }

  & $Definition.Assert $result
  return [pscustomobject]$result
}

$sessionMutex = Acquire-EmulatorSessionLock
try {
  $resolvedEmulatorPath = Resolve-EmulatorPath -RequestedPath $EmulatorPath
  $allCases = @(Get-SmokeCases)

  if ($Case -eq "all") {
    $selectedCases = $allCases
  } else {
    $selectedCases = @($allCases | Where-Object { $_.Name -eq $Case })
    if ($selectedCases.Count -eq 0) {
      $available = ($allCases | ForEach-Object { $_.Name }) -join ", "
      throw "Unknown case '$Case'. Available: $available"
    }
  }

  $failures = New-Object System.Collections.Generic.List[string]
  $startedAt = Get-Date

  foreach ($definition in $selectedCases) {
    $caseStartedAt = Get-Date
    try {
      $null = Invoke-SmokeCase -Definition $definition -ResolvedEmulatorPath $resolvedEmulatorPath -TimeoutMs $TimeoutMs `
        -DelayCapMs $DelayCapMs
      $elapsedMs = [int]((Get-Date) - $caseStartedAt).TotalMilliseconds
      Write-Host ("PASS {0} ({1} ms)" -f $definition.Name, $elapsedMs)
    } catch {
      $elapsedMs = [int]((Get-Date) - $caseStartedAt).TotalMilliseconds
      $message = "FAIL $($definition.Name) ($elapsedMs ms): $($_.Exception.Message)"
      $failures.Add($message)
      Write-Host $message
    }
  }

  $totalMs = [int]((Get-Date) - $startedAt).TotalMilliseconds
  Write-Host ("Completed {0} case(s) in {1} ms" -f $selectedCases.Count, $totalMs)

  if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    exit 1
  }

  exit 0
} finally {
  Release-EmulatorSessionLock -Mutex $sessionMutex
}
