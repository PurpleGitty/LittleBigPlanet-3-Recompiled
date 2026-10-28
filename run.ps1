# Builds, then runs the game for a while. Just before the time is up it prints
# a thread dump (where every game thread is waiting) and exits.
#   .\run.ps1                       # 20 s
#   .\run.ps1 -Seconds 60
#   .\run.ps1 -Break eboot.bin+0x9f878,eboot.bin+0xa310c   # log registers there
param([int]$Seconds = 20, [string[]]$Break = @())

$dir = $PSScriptRoot
Set-Location $dir
& cmd.exe /d /c "`"$dir\build.bat`"" 2>&1 | Select-String -Pattern "error|FAILED"
if ($LASTEXITCODE -ne 0) { Write-Host "build failed"; exit 1 }

$out = "$dir\analysis\run_stdout.txt"
$err = "$dir\analysis\run_stderr.txt"
$args = @("--run", "--dump-after=$Seconds", "--skip-intro")
$args += $Break | ForEach-Object { "--break=$_" }
$p = Start-Process -FilePath "$dir\build\lbp3.exe" -ArgumentList $args `
    -WorkingDirectory $dir -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if (-not $p.WaitForExit(($Seconds + 10) * 1000)) {
    Stop-Process -Id $p.Id -Force
    Write-Host "hung even past the thread dump (stopped)"
} else {
    Write-Host "exit code: $($p.ExitCode) (5 = ran until the thread dump)"
}
Get-Content $err | Select-Object -Last 50
