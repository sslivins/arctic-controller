# Regenerates montserrat_220_digits.c: digits and '-' only, for the home
# screen's big tank temperature.
param([string]$OutDir = $PSScriptRoot)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$name = "montserrat_220_digits"
$out  = [System.IO.Path]::GetFullPath((Join-Path $OutDir "$name.c"))
# Convert in a scratch dir with relative paths so the "Opts:" header is stable.
$work = Join-Path ([System.IO.Path]::GetTempPath()) "regen_digits"
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item Montserrat-Medium.ttf $work
Push-Location $work
try {
    npx --yes lv_font_conv@1.5.3 --font Montserrat-Medium.ttf -r "0x2D,0x30-0x39" `
        --bpp 4 --size 220 --format lvgl -o "$name.c" --no-compress
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed" }
    $src = [System.IO.File]::ReadAllText((Join-Path $work "$name.c"))
} finally {
    Pop-Location
    Remove-Item -Recurse -Force $work
}
[System.IO.File]::WriteAllText($out, $src, (New-Object System.Text.UTF8Encoding $false))
