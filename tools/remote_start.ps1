# Starts the game for a remote-controlled test session: input comes from
# analysis\input_cmd.txt (see runtime/src/input/gamepad_sdl.cpp, LBP3_REMOTE_INPUT).
# Extra environment variables can be passed as "NAME=value" arguments, and arguments
# starting with "--" go to lbp3.exe:
#   .\tools\remote_start.ps1 LBP3_READBACKS=relaxed --break=eboot.bin+0x1234
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$EnvVars)

$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$env:LBP3_REMOTE_INPUT = "1"
$exeArgs = @("--run", "--skip-intro")
$exeArgs += @($EnvVars | Where-Object { $_ -like "--*" })
$EnvVars = @($EnvVars | Where-Object { $_ -and $_ -notlike "--*" })
foreach ($kv in $EnvVars) {
    $name, $value = $kv -split "=", 2
    Set-Item -Path "env:$name" -Value $value
}
$p = Start-Process -FilePath "$root\build\lbp3.exe" `
    -ArgumentList $exeArgs `
    -WorkingDirectory $root -RedirectStandardOutput "$root\analysis\remote_stdout.txt" `
    -RedirectStandardError "$root\analysis\remote_stderr.txt" -PassThru
$env:LBP3_REMOTE_INPUT = $null
foreach ($kv in $EnvVars) { Remove-Item -Path ("env:" + ($kv -split "=", 2)[0]) -ErrorAction SilentlyContinue }
Write-Host "started pid $($p.Id)"
