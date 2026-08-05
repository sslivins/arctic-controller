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
 * @brief Translate a keyed, library-sourced string to the current language.
 *
 * The macon library is the single source of truth for English text (parameter
 * names, detail paragraphs, enum meanings). The controller holds ONLY the
 * non-English translations, keyed by a stable msg_id supplied by the library.
 * For English — or for any key without a translation — the library-provided
 * English fallback is returned unchanged, so nothing prose ever has to be
 * duplicated in the controller.
 *
 * @param key              Stable i18n key (e.g. "ap.freq_ratio_k1.name").
 *                         May be NULL (English-only string) -> returns fallback.
 * @param english_fallback The library's English source-of-truth string. Returned
 *                         when the current language is English or no translation
 *                         exists for `key`. May be NULL if the caller has none.
 * @return Localized string, or `english_fallback` when untranslated.
 */
const char* i18n_get_key(const char* key, const char* english_fallback);

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

/**
 * @brief Look up a string ID by its English text
 * @param english_text The English string to find
 * @return The string_id_t if found, or STR_COUNT if not found
 */
string_id_t i18n_find_by_english(const char* english_text);

/**
 * @brief Translate an English string to the current language
 * @param english_text The English string to translate
 * @return The localized string, or the original english_text if no match found
 */
const char* i18n_translate(const char* english_text);

/**
 * @brief Get the English text for a localized string
 * @param localized_text The text in any language to look up
 * @return The English version, or NULL if not found in any i18n table
 */
const char* i18n_get_english(const char* localized_text);

#ifdef __cplusplus
}
#endif
