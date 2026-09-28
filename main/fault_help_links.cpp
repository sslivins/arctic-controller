/*
 * Arctic Heat Pump Controller
 * Links from fault codes to Arctic's online troubleshooting articles.
 *
 * Arctic publishes one troubleshooting article per fault code on its Freshdesk
 * support site. This table lives in the controller rather than the arctic-macon
 * library because the library describes the Macon hardware, which other brands
 * also sell; the articles are Arctic's.
 *
 * The URLs leave out the article's title slug: Freshdesk finds articles by the
 * number alone, and the shorter URL gives a smaller, easier-to-scan QR code.
 *
 * tools/check_fault_help_links.py reads this table (one {"CODE", "URL"} entry
 * per line) and checks that every link still opens the right article. A
 * weekly workflow runs it so a link Arctic removes gets noticed.
 */

#include "fault_help_links.h"

#include <ctype.h>

namespace arctic {

namespace {

constexpr const char* SUPPORT_ARTICLE_LIST =
    "https://arcticheatpumps.freshdesk.com/support/solutions";

struct FaultHelpLink {
    const char* code;
    const char* url;
};

// clang-format off
constexpr FaultHelpLink FAULT_HELP_LINKS[] = {
    {"E01", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837781"},
    {"E05", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837782"},
    {"E09", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837783"},
    {"E13", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837785"},
    {"E18", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837786"},
    {"E19", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837787"},
    {"E20", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837788"},
    {"E21", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837789"},
    {"E22", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837790"},
    {"E27", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000982091"},
    {"E28", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959944"},
    {"EA",  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832846"},
    {"EB",  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832847"},
    {"EC",  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832850"},
    {"FA",  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832842"},
    {"P01", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000810195"},
    {"P02", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838"},
    {"P06", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832839"},
    {"P11", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832841"},
    {"P15", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959949"},
    {"P19", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959948"},
    {"P27", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959950"},
    {"PC",  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832851"},
    {"R02", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60001048972"},
    {"R10", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959947"},
    {"R11", "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959945"},
};
// clang-format on

bool same_code(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == *b;
}

const FaultHelpLink* find(const char* code) {
    if (!code) return nullptr;
    for (const auto& link : FAULT_HELP_LINKS) {
        if (same_code(link.code, code)) return &link;
    }
    return nullptr;
}

}  // namespace

const char* faultHelpUrl(const char* code) {
    const FaultHelpLink* link = find(code);
    return link ? link->url : SUPPORT_ARTICLE_LIST;
}

bool faultHasHelpArticle(const char* code) {
    return find(code) != nullptr;
}

}  // namespace arctic
