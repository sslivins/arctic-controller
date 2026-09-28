/*
 * Arctic Heat Pump Controller
 * Links from fault codes to Arctic's online troubleshooting articles.
 */

#pragma once

namespace arctic {

// Arctic's support-site article for a fault code (e.g. "P02"), or the support
// site's article list when Arctic has no article for that code. Never null.
// Matching ignores case, so "r02" finds the "R02" article.
const char* faultHelpUrl(const char* code);

// True when faultHelpUrl(code) is a page about that specific code.
bool faultHasHelpArticle(const char* code);

}  // namespace arctic
