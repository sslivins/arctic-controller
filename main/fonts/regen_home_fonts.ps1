# Regenerates the home screen's big tank-temperature fonts:
#   montserrat_220_digits.c  digits and '-' for the number
#   montserrat_80_unit.c     degree sign, C and F for the unit beside it
param([string]$OutDir = $PSScriptRoot)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Convert-Font([string]$name, [string]$range, [int]$size) {
    $out  = [System.IO.Path]::GetFullPath((Join-Path $OutDir "$name.c"))
    # Convert in a scratch dir with relative paths so the "Opts:" header is stable.
    $work = Join-Path ([System.IO.Path]::GetTempPath()) "regen_$name"
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item Montserrat-Medium.ttf $work
    Push-Location $work
    try {
        npx --yes lv_font_conv@1.5.3 --font Montserrat-Medium.ttf -r $range `
            --bpp 4 --size $size --format lvgl -o "$name.c" --no-compress
        if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed" }
        $src = [System.IO.File]::ReadAllText((Join-Path $work "$name.c"))
    } finally {
        Pop-Location
        Remove-Item -Recurse -Force $work
    }
    [System.IO.File]::WriteAllText($out, $src, (New-Object System.Text.UTF8Encoding $false))
}

Convert-Font "montserrat_220_digits" "0x2D,0x30-0x39" 220
Convert-Font "montserrat_80_unit" "0x43,0x46,0xB0" 80