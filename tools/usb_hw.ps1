# Helpers for hwcheck_USB20a2.md. Usage (from the repo root):  . .\tools\usb_hw.ps1
# NOTE: names are UsbLs / UsbCd / UsbUp on purpose: 'ls' and 'cd' are built-in PowerShell aliases and would shadow functions.
# ASCII only on purpose (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI).
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$global:IP = "192.168.0.106"
$global:mode = '/api/usb/mode'
$global:opt = '/api/usb/options'

function Call($method, $path, $json) {
    for ($i = 1; $i -le 3; $i++) {
        try {
            $p = @{ Method = $method; Uri = "http://$($global:IP)$path"; DisableKeepAlive = $true; TimeoutSec = 8 }
            if ($json) { $p.ContentType = "application/json"; $p.Body = $json }
            $r = Invoke-RestMethod @p
            return "200 " + ($r | ConvertTo-Json -Compress -Depth 4)
        } catch {
            if ($_.Exception.Response) {
                return "$($_.Exception.Response.StatusCode.value__) $($_.ErrorDetails.Message)"
            }
            Start-Sleep 1
        }
    }
    return "NO RESPONSE (3 tries): $($_.Exception.Message)"
}

# GET as UTF-8 (the firmware sends no charset, so Invoke-RestMethod garbles non-ASCII names)
function Get-Json($path) {
    $r = Invoke-WebRequest -Uri "http://$($global:IP)$path" -UseBasicParsing -DisableKeepAlive -TimeoutSec 8
    $txt = [System.Text.Encoding]::UTF8.GetString($r.RawContentStream.ToArray())
    return ($txt | ConvertFrom-Json)
}

function QS {
    $s = Get-Json '/api/usb/status'
    $q = $s.queue
    "state=$($q.state) count=$($q.count) pos=$($q.pos) idx=$($q.index) scope=$($q.scope) trunc=$($q.truncated) building=$($q.building) opt=$($s.options.scope) path=$($s.nowPlaying.path)"
}

function UsbLs {
    $l = Get-Json '/api/usb/list?limit=100'
    "cwd=$($l.cwd) count=$($l.count) dirs=$($l.dirs) version=$($l.version) truncated=$($l.truncated)"
    foreach ($it in $l.items) {
        $k = '[F]'
        if ($it.dir) { $k = '[D]' }
        "{0,3} {1} {2}" -f $it.i, $k, $it.name
    }
}

function UsbCd($index) {
    $r = Call POST '/api/usb/cd' ("{`"index`":$index}")
    Start-Sleep 1
    return $r
}

function UsbUp {
    $r = Call POST '/api/usb/cd' '{"up":true}'
    Start-Sleep 1
    return $r
}

function PlayFirstFile {
    # first FILE of the current folder (folders come first in the list, so its index == dirs)
    $l = Get-Json '/api/usb/list?limit=50'
    if ($l.count -le $l.dirs) { return "no files in $($l.cwd)" }
    $idx = $l.dirs
    "play idx=${idx} (cwd=$($l.cwd) count=$($l.count) dirs=$($l.dirs) version=$($l.version)): " +
        (Call POST '/api/usb/play' ("{`"index`":$idx,`"version`":$($l.version)}"))
}

function UsbCycle($scope, $n) {
    # $n cycles: enter USB mode -> set scope -> play first file of the root -> wait -> stop -> exit.
    # Read the memory lines in the serial monitor: "worker: started ... psram_free=", "ram vs before begin(): d_free=".
    $r = Call POST $mode '{"action":"exit"}'
    Start-Sleep 8
    for ($c = 1; $c -le $n; $c++) {
        "=== scope=$scope cycle $c/$n"
        $r = Call POST $mode '{"action":"enter"}'
        Start-Sleep 9
        $r = Call POST $opt ("{`"scope`":`"$scope`"}")
        $r = PlayFirstFile
        Start-Sleep 6
        QS
        $r = Call POST '/api/usb/control' '{"action":"stop"}'
        Start-Sleep 1
        $r = Call POST $mode '{"action":"exit"}'
        Start-Sleep 8
    }
}
