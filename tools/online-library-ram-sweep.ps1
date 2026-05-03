param(
  [string]$EmulatorPath = "",
  [int]$TimeoutMs = 180000,
  [int]$DelayCapMs = 15000,
  [int]$BaselineHeapFreeBytes = 79828,
  [int]$MinHeapFreeBytes = 40960,
  [int]$CoarseStepBytes = 4096,
  [int]$RetryCount = 2
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-SmokeCase {
  param(
    [string]$CaseName,
    [int]$HeapFreeBytes
  )

  $scriptPath = Join-Path $PSScriptRoot "online-library-smoke.ps1"
  $arguments = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $scriptPath,
    "-Case", $CaseName,
    "-TimeoutMs", "$TimeoutMs",
    "-DelayCapMs", "$DelayCapMs",
    "-HeapFreeBytes", "$HeapFreeBytes"
  )
  if (-not [string]::IsNullOrWhiteSpace($EmulatorPath)) {
    $arguments += @("-EmulatorPath", $EmulatorPath)
  }

  $attempt = 0
  $text = ""
  $exitCode = 1
  $passed = $false
  do {
    $attempt++
    $output = & powershell @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String)
    $passed = $exitCode -eq 0 -and $text -match ("PASS " + [regex]::Escape($CaseName))
    if ($passed) {
      break
    }
    Start-Sleep -Milliseconds 750
  } while ($attempt -lt [Math]::Max(1, $RetryCount))

  [pscustomobject]@{
    Passed = $passed
    ExitCode = $exitCode
    Output = $text
    AttemptCount = $attempt
  }
}

function Find-MinPassingHeap {
  param([string]$CaseName)

  $bestPass = $BaselineHeapFreeBytes
  $firstFail = $null
  $current = $BaselineHeapFreeBytes

  while ($current -ge $MinHeapFreeBytes) {
    Write-Host ("[{0}] coarse check @ {1} bytes" -f $CaseName, $current)
    $result = Invoke-SmokeCase -CaseName $CaseName -HeapFreeBytes $current
    if ($result.Passed) {
      $bestPass = $current
      $current -= $CoarseStepBytes
      continue
    }
    $firstFail = $current
    break
  }

  if ($null -eq $firstFail) {
    return [pscustomobject]@{
      CaseName = $CaseName
      LowestPassingHeap = $bestPass
      FirstFailHeap = $null
      MarginFromBaseline = $BaselineHeapFreeBytes - $bestPass
      Notes = "No failure down to floor"
    }
  }

  $lowFail = $firstFail
  $highPass = $bestPass
  while (($highPass - $lowFail) -gt 512) {
    $mid = [int]([math]::Floor(($highPass + $lowFail) / 2.0))
    Write-Host ("[{0}] refine check @ {1} bytes" -f $CaseName, $mid)
    $result = Invoke-SmokeCase -CaseName $CaseName -HeapFreeBytes $mid
    if ($result.Passed) {
      $highPass = $mid
    } else {
      $lowFail = $mid
    }
  }

  [pscustomobject]@{
    CaseName = $CaseName
    LowestPassingHeap = $highPass
    FirstFailHeap = $lowFail
    MarginFromBaseline = $BaselineHeapFreeBytes - $highPass
    Notes = "Binary refined"
  }
}

$cases = @(
  "sources_list_has_truyen_full",
  "hako_search_right_refresh_and_home",
  "truyen_full_search_to_detail",
  "truyen_full_browse_chapters",
  "browse_chapters_latest_page",
  "hako_download_queue_visible"
)

$results = New-Object System.Collections.Generic.List[object]
foreach ($caseName in $cases) {
  $results.Add((Find-MinPassingHeap -CaseName $caseName))
}

$results | Format-Table CaseName,LowestPassingHeap,FirstFailHeap,MarginFromBaseline,Notes -AutoSize
