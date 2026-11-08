# make_dumps_def5a.ps1 - dumps for the DEF5a chat (old repo, ST7789)
# Run: powershell -ExecutionPolicy Bypass -File .\make_dumps_def5a.ps1
$repo = "C:\Users\HOME\Desktop\esp32s3-audio-controller-main"
Set-Location $repo
$out = "_dumps"
$utf8 = New-Object System.Text.UTF8Encoding($false)

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

$gi = ".gitignore"
if (-not (Test-Path $gi)) { [IO.File]::WriteAllText((Join-Path $repo $gi), "_dumps/`r`n", $utf8) }
elseif (-not (Select-String -Path $gi -Pattern '^\s*_dumps/?\s*$' -Quiet)) { Add-Content -Path $gi -Value "`r`n_dumps/" -Encoding UTF8 }

# --- Probe: is heap_caps_walk present in PlatformIO packages ---
$probe = New-Object System.Text.StringBuilder
$pkgs = Join-Path $env:USERPROFILE ".platformio\packages"
[void]$probe.AppendLine("pkgs = $pkgs")
try {
  $hdrs = Get-ChildItem -Path $pkgs -Recurse -Filter "esp_heap_caps.h" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "esp32s3" }
  foreach ($h in $hdrs) {
    [void]$probe.AppendLine("HEADER: " + $h.FullName)
    Select-String -Path $h.FullName -Pattern "heap_caps_walk|heap_caps_dump|walker_cb_t|failed_alloc" | ForEach-Object { [void]$probe.AppendLine("  " + $_.Line.Trim()) }
  }
  if (-not $hdrs) { [void]$probe.AppendLine("HEADER: not found (esp_heap_caps.h, esp32s3)") }
  $libs = Get-ChildItem -Path $pkgs -Recurse -Filter "libheap.a" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match "esp32s3" }
  $nm = Get-ChildItem -Path $pkgs -Recurse -Filter "xtensa-esp32s3-elf-nm.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $nm) { $nm = Get-ChildItem -Path $pkgs -Recurse -Filter "xtensa-esp-elf-nm.exe" -ErrorAction SilentlyContinue | Select-Object -First 1 }
  foreach ($l in $libs) {
    [void]$probe.AppendLine("LIB: " + $l.FullName)
    if ($nm) {
      [void]$probe.AppendLine("NM: " + $nm.FullName)
      & $nm.FullName $l.FullName 2>&1 | Select-String -Pattern "heap_caps_walk|multi_heap_walk|heap_caps_dump" | ForEach-Object { [void]$probe.AppendLine("  " + $_.Line.Trim()) }
    } else { [void]$probe.AppendLine("NM: nm executable not found") }
  }
  if (-not $libs) { [void]$probe.AppendLine("LIB: libheap.a not found (esp32s3)") }
} catch { [void]$probe.AppendLine("PROBE ERROR: " + $_.Exception.Message) }
[IO.File]::WriteAllText((Join-Path (Join-Path $repo $out) "heap_api_probe.txt"), $probe.ToString(), $utf8)
Write-Host ("heap_api_probe.txt: " + [Math]::Round($probe.Length/1KB,1) + " KB")

# --- File groups ---
$groups = [ordered]@{
  "dump_1" = @("src\usb\usb_memdiag.h","src\usb\usb_memdiag.cpp","src\usb\usb_mode.h","src\usb\usb_mode.cpp","src\core\ssl_workaround.h","src\core\ssl_workaround.cpp")
  "dump_2" = @("src\usb\usb_play_diag.h","src\usb\usb_play_diag.cpp","src\usb\usb_msc_mount.h","src\usb\usb_msc_mount.cpp","src\main.cpp")
  "dump_3" = @("src\audio\audio_player.h","src\audio\audio_player.cpp")
  "dump_4" = @("src\config\features.h","src\config\usb_host_config.h","src\config\usb_*_config.h","src\config\file_player_config.h","hwcheck_USB13b.md")
}

foreach ($g in $groups.Keys) {
  $sb = New-Object System.Text.StringBuilder
  $seen = @{}
  foreach ($entry in $groups[$g]) {
    $files = @()
    if ($entry -like "*``**") { $files = @(Get-ChildItem -Path $entry -File -ErrorAction SilentlyContinue) }
    elseif (Test-Path -LiteralPath $entry) { $files = @(Get-Item -LiteralPath $entry) }
    if ($files.Count -eq 0) { Write-Warning "Missing file: $entry"; continue }
    foreach ($f in $files) {
      if ($seen.ContainsKey($f.FullName)) { continue }
      $seen[$f.FullName] = $true
      $rel = $f.FullName.Substring($repo.Length).TrimStart('\')
      [void]$sb.AppendLine("===== FILE: .\$rel =====")
      [void]$sb.AppendLine([IO.File]::ReadAllText($f.FullName, [Text.Encoding]::UTF8))
    }
  }
  $path = Join-Path (Join-Path $repo $out) ($g + ".txt")
  [IO.File]::WriteAllText($path, $sb.ToString(), $utf8)
  Write-Host ("{0}: {1} KB" -f $g, [Math]::Round((Get-Item $path).Length/1KB,1))
}
Write-Host "Done. Attach all files from the folder $out (dump_1..dump_4, heap_api_probe.txt) together with PROMPT_DEF5a_heapmap.md."
