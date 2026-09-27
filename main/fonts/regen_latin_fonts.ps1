# Regenerates the montserrat_<size>_latin UI fonts: Latin-1 (accents), Greek
# Delta, curly quotes and the FontAwesome icons the UI uses. Each font falls
# back to montserrat_<size>_misc (see regen_misc_fonts.ps1), which lv_font_conv
# can't express, so the fallback is patched in after conversion.
# A new size also needs: regen_misc_fonts.ps1, main/CMakeLists.txt, fonts.h.
# Example: .\regen_latin_fonts.ps1 -Sizes 20
param([int[]]$Sizes = @(16, 20, 24, 32, 40), [string]$OutDir = $PSScriptRoot)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$text  = "0x20-0xFF,0x394,0x2018-0x201F"
$icons = "61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61475,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62189,62212,62810,63426,63650"
foreach ($size in $Sizes) {
    $name = "montserrat_${size}_latin"
    $out  = [System.IO.Path]::GetFullPath((Join-Path $OutDir "$name.c"))
    # Convert in a scratch dir with relative paths so the "Opts:" header is stable.
    $work = Join-Path ([System.IO.Path]::GetTempPath()) "regen_latin_$size"
    New-Item -ItemType Directory -Force $work | Out-Null
    Copy-Item Montserrat-Medium.ttf, FontAwesome.woff $work
    Push-Location $work
    try {
        npx --yes lv_font_conv@1.5.3 --font Montserrat-Medium.ttf -r $text `
            --font FontAwesome.woff -r $icons `
            --bpp 4 --size $size --format lvgl -o "$name.c" --no-compress
        if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed for size $size" }
        $src = [System.IO.File]::ReadAllText((Join-Path $work "$name.c"))
    } finally {
        Pop-Location
        Remove-Item -Recurse -Force $work
    }

    $nl  = if ($src.Contains("`r`n")) { "`r`n" } else { "`n" }
    $guard = "#if MONTSERRAT_${size}_LATIN$nl"
    $fb    = "    .fallback = NULL,$nl"
    if (-not $src.Contains($guard) -or -not $src.Contains($fb)) { throw "unexpected lv_font_conv output for $name" }
    $src = $src.Replace($guard, "$guard$nl/* Misc glyphs fallback font (geometric shapes, etc.) */${nl}extern const lv_font_t montserrat_${size}_misc;$nl")
    $src = $src.Replace($fb, "    .fallback = &montserrat_${size}_misc,$nl")
    [System.IO.File]::WriteAllText($out, $src, (New-Object System.Text.UTF8Encoding $false))
}
