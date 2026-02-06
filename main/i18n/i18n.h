/*
 * Arctic Heat Pump Controller
 * Internationalization (i18n) API
 */
#pragma once

#include "strings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supported languages
 */
typedef enum {
    LANG_ENGLISH = 0,
    LANG_FRENCH,
    LANG_SPANISH,
    LANG_COUNT
} language_t;

/**
 * @brief Initialize the i18n system
 *        Loads saved language preference from NVS
 */
void i18n_init(void);

/**
 * @brief Get a localized string
 * @param id String identifier
 * @return Localized string (never NULL, returns English fallback if missing)
 */
const char* i18n_get(string_id_t id);

/**
 * @brief Get current language
 * @return Current language setting
 */
language_t i18n_get_language(void);

/**
 * @brief Set the language
 *        Saves to NVS for persistence
 * @param lang Language to set
 */
void i18n_set_language(language_t lang);

/**
 * @brief Get the display name for a language (in that language)
 * @param lang Language
 * @return Display name (e.g., "English", "Français", "Español")
 */
const char* i18n_get_language_name(language_t lang);

/**
 * @brief Get the display name for a language in current language
 * @param lang Language to get name for
 * @return Display name in current language
 */
const char* i18n_get_language_name_localized(language_t lang);

#ifdef __cplusplus
}
#endif
