# make_dumps_def5b.ps1 - dumps for the DEF5b chat (old repo, ST7789)
# Run: powershell -ExecutionPolicy Bypass -File .\make_dumps_def5b.ps1
$repo = "C:\Users\HOME\Desktop\esp32s3-audio-controller-main"
Set-Location $repo
$out = "_dumps"
$utf8 = New-Object System.Text.UTF8Encoding($false)

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Path $out | Out-Null

$gi = ".gitignore"
if (-not (Test-Path $gi)) { [IO.File]::WriteAllText((Join-Path $repo $gi), "_dumps/`r`n", $utf8) }
elseif (-not (Select-String -Path $gi -Pattern '^\s*_dumps/?\s*$' -Quiet)) { Add-Content -Path $gi -Value "`r`n_dumps/" -Encoding UTF8 }

# --- File groups ---
$groups = [ordered]@{
  "dump_1" = @("src\core\ssl_workaround.h","src\core\ssl_workaround.cpp","src\config\features.h","src\config\usb_host_config.h","src\config\usb_*_config.h","src\usb\usb_heapmap.h")
  "dump_2" = @("src\audio\audio_player.h","src\audio\audio_player.cpp")
  "dump_3" = @("src\core\app_controller.h","src\core\app_controller.cpp")
  "dump_4" = @("src\usb\usb_mode.h","src\usb\usb_mode.cpp","src\usb\usb_play_diag.cpp")
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
Write-Host "Done. Attach dump_1..dump_4 from the folder $out together with PROMPT_DEF5b_preflight.md."
