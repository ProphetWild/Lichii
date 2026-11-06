# One Ocarina trial: launch with extra env vars, auto-load the save, watch frames for a while.
#   powershell -File tuned\oot-trial.ps1 -Label perf-on -Env @{EDEN_NX2_PERF_OFF='method'} -Watch 120
param(
    [string]$Label = 'trial',
    [hashtable]$Env = @{},
    [int]$Watch = 120,
    [int]$Interval = 10
)
$ErrorActionPreference = 'Continue'
# Native Windows Python (MSYS python rejects C:/ paths); $env:OOT_PYTHON overrides it.
$py = if ($env:OOT_PYTHON) { $env:OOT_PYTHON } else { (Get-Command python.exe -ErrorAction Stop).Source }
$root = Split-Path -Parent $PSScriptRoot
Stop-Process -Name lichii -Force -ErrorAction SilentlyContinue
Start-Sleep 2
$env:EDEN_HDR_OUTPUT = '1'
foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }
cmd /c "$root\tuned\run-oot.cmd"
& $py -I "$root\tools\nx2\oot_autoload.py" --presses 3 --title-wait 20 | Out-Null
$out = & $py -I "$root\tools\nx2\oot_frame_check.py" --duration $Watch --interval $Interval
$result = ($out | Select-String 'RESULT').Line
$fps = ($out | Select-String 'fps ([0-9.]+)' -AllMatches | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
$median = if ($fps) { ($fps | Sort-Object)[[int]($fps.Count / 2)] } else { 0 }
"[$Label] $result, median fps $median, samples $($fps.Count)"
$out | Select-String 'flat 0\.[6-9]|flat 1\.0' | ForEach-Object { "    $($_.Line)" }
$shot = Join-Path $root ("build\debug\trial-" + ($Label -replace '[^A-Za-z0-9_-]', '_') + ".png")
& $py -I "$root\tools\nx2\debug_mcp.py" --call '{"method":"screenshot","params":{}}' --out $shot | Out-Null
Stop-Process -Name lichii -Force -ErrorAction SilentlyContinue
