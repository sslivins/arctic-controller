# Fails the build if IDF's own spi_flash isn't the v6.1 copy this override was
# taken from, so a newer IDF can't silently build against stale sources. See
# README.md. Line endings are normalised so the hash matches on any checkout.
# Run with `cmake -DIDF_PATH=<idf> -DARCTIC_PRINT_HASH=1 -P idf_spi_flash_guard.cmake`
# to print the hash.
set(ARCTIC_SPI_FLASH_BASE_SHA256
    "4b6e6e6423efac982c9b9163d1c25465520d92a7e4b43bb09eee8ad9186965f4")

function(arctic_idf_spi_flash_hash out)
    set(root "${IDF_PATH}/components/spi_flash")
    file(GLOB_RECURSE files RELATIVE "${root}" "${root}/*")
    list(FILTER files EXCLUDE REGEX "^test_apps/")
    list(SORT files)
    set(manifest "")
    foreach(f IN LISTS files)
        file(READ "${root}/${f}" content HEX)
        string(REPLACE "0d0a" "0a" content "${content}")
        string(SHA256 h "${content}")
        string(APPEND manifest "${f}:${h}\n")
    endforeach()
    string(SHA256 total "${manifest}")
    set(${out} "${total}" PARENT_SCOPE)
endfunction()

arctic_idf_spi_flash_hash(arctic_spi_flash_hash)
if(ARCTIC_PRINT_HASH)
    message("${arctic_spi_flash_hash}")
elseif(NOT arctic_spi_flash_hash STREQUAL ARCTIC_SPI_FLASH_BASE_SHA256)
    message(FATAL_ERROR
        "IDF's components/spi_flash differs from the ESP-IDF v6.1 copy that "
        "components/spi_flash overrides (hash ${arctic_spi_flash_hash}). Re-copy it "
        "from this IDF and re-apply the fix, or delete the override if this IDF "
        "has it. See components/spi_flash/README.md.")
endif()
