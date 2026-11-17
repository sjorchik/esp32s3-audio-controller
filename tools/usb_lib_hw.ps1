# Helpers for hwcheck_USB22a1.md. Usage (from the repo root):  . .\tools\usb_hw.ps1 ; . .\tools\usb_lib_hw.ps1
# Needs Get-Json / Call / $global:IP from tools\usb_hw.ps1. ASCII only on purpose (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI).

function LibGet($query) {
    # one GET /api/usb/library; returns the parsed body, or a string "HTTP <code> <body>" on non-200
    try {
        return (Get-Json ('/api/usb/library' + $query))
    } catch {
        if ($_.Exception.Response) { return "HTTP $($_.Exception.Response.StatusCode.value__) $($_.ErrorDetails.Message)" }
        return "ERR $($_.Exception.Message)"
    }
}

function LibWait($timeoutSec = 75) {
    # polls every 100 ms until state != building; prints the time from the FIRST GET (which starts the scan) to ready
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $first = $null
    while ($sw.Elapsed.TotalSeconds -lt $timeoutSec) {
        $l = LibGet '?limit=1'
        if ($l -is [string]) { return $l }
        if ($null -eq $first) { $first = $l.state }
        if ($l.state -ne 'building') {
            return "first=$first state=$($l.state) total=$($l.total) version=$($l.version) truncated=$($l.truncated) after $([int]$sw.ElapsedMilliseconds) ms"
        }
        Start-Sleep -Milliseconds 100
    }
    return "timeout after $timeoutSec s"
}

function LibAll {
    # walks all pages by nextOffset; checks contiguity of i, path composition and the page size
    $off = 0; $n = 0; $pages = 0; $bad = 0; $maxBytes = 0; $ver = 0
    while ($true) {
        $q = "?offset=$off&limit=100"
        if ($ver -ne 0) { $q += "&version=$ver" }
        try {
            $r = Invoke-WebRequest -Uri "http://$($global:IP)/api/usb/library$q" -UseBasicParsing -DisableKeepAlive -TimeoutSec 8
        } catch { return "page at offset $off failed: $($_.Exception.Message)" }
        $bytes = $r.RawContentLength
        if ($bytes -gt $maxBytes) { $maxBytes = $bytes }
        $l = ([System.Text.Encoding]::UTF8.GetString($r.RawContentStream.ToArray())) | ConvertFrom-Json
        if ($l.state -ne 'ready') { return "state=$($l.state) (not ready) total=$($l.total)" }
        $ver = $l.version
        foreach ($it in $l.items) {
            if ($it.i -ne $n) { $bad++ }
            $exp = $it.dir.TrimEnd('/') + '/' + $it.name
            if ($it.path -ne $exp) { $bad++ }
            $n++
        }
        $pages++
        if ($null -eq $l.nextOffset) { break }
        $off = $l.nextOffset
        if ($pages -gt 400) { return "too many pages" }
    }
    return "items=$n total=$($l.total) pages=$pages bad=$bad maxPageBytes=$maxBytes version=$ver truncated=$($l.truncated)"
}

function LibTime($count = 20) {
    # response time of one page (offset 0, limit 50) over $count requests: run this WHILE a track is playing
    $ms = @()
    for ($i = 0; $i -lt $count; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $l = LibGet '?offset=0&limit=50'
        $sw.Stop()
        if ($l -is [string]) { return "request $i failed: $l" }
        $ms += $sw.ElapsedMilliseconds
        Start-Sleep -Milliseconds 300
    }
    $s = $ms | Measure-Object -Minimum -Maximum -Average
    return "n=$count min=$($s.Minimum) avg=$([int]$s.Average) max=$($s.Maximum) ms (state=$($l.state))"
}
