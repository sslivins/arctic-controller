# Regenerates the montserrat_<size>_misc fallback fonts. The *_latin fonts fall
# back to these, so any glyph added here renders in every UI font.
#   - Montserrat: dashes, bullet, ellipsis, arrows (matches the body text)
#   - Segoe UI Symbol (Windows): the hollow/filled circles used as indicators
# After adding a range, run: python scripts/check_glyphs.py
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$montserrat = "0x2013,0x2014,0x2022,0x2026,0x2190-0x2193"
$symbols    = "0x25CB,0x25CF"
foreach ($size in 16, 24, 32, 40) {
    npx --yes lv_font_conv@1.5.3 --no-compress --bpp 4 --size $size --format lvgl `
        --font Montserrat-Medium.ttf -r $montserrat `
        --font C:\Windows\Fonts\seguisym.ttf -r $symbols `
        -o "montserrat_${size}_misc.c"
    if ($LASTEXITCODE -ne 0) { throw "lv_font_conv failed for size $size" }
}
