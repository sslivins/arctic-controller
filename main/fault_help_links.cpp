/*
 * Arctic Heat Pump Controller
 * Links from faults to Arctic's online troubleshooting articles.
 *
 * Arctic publishes one troubleshooting article per fault on its Freshdesk
 * support site. This table lives in the controller rather than the arctic-macon
 * library because the library describes the Macon hardware, which other brands
 * also sell; the articles are Arctic's. It is keyed by the library's opaque
 * MaconFaultId so the controller never names an OEM fault code itself.
 *
 * The URLs leave out the article's title slug: Freshdesk finds articles by the
 * number alone, and the shorter URL gives a smaller, easier-to-scan QR code.
 *
 * tools/check_fault_help_links.py reads this table (one
 * {MaconFaultId::Name, "URL"} entry per line) and checks that every link still
 * opens the article for that fault's code. A weekly workflow runs it so a link
 * Arctic removes or renumbers gets noticed.
 */

#include "fault_help_links.h"

#include "macon_faults.h"

namespace arctic {

namespace {

constexpr const char* SUPPORT_ARTICLE_LIST =
    "https://arcticheatpumps.freshdesk.com/support/solutions";

struct FaultHelpLink {
    MaconFaultId id;
    const char* url;
};

// clang-format off
constexpr FaultHelpLink FAULT_HELP_LINKS[] = {
    {MaconFaultId::DischargeSensor,          "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837781"},
    {MaconFaultId::CoilSensor,               "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837782"},
    {MaconFaultId::SuctionSensor,            "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837783"},
    {MaconFaultId::CoolCoilSensor,           "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837785"},
    {MaconFaultId::OutletWaterSensor,        "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837786"},
    {MaconFaultId::InletWaterSensor,         "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837787"},
    {MaconFaultId::ControllerCommunication,  "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837789"},
    {MaconFaultId::AmbientSensor,            "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000837790"},
    {MaconFaultId::DriverCommunication,      "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000982091"},
    {MaconFaultId::EepromError,              "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959944"},
    {MaconFaultId::DcFanMotor,               "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832842"},
    {MaconFaultId::WaterFlowProtection,      "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000810195"},
    {MaconFaultId::HighPressureProtection,   "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832838"},
    {MaconFaultId::LowPressureProtection,    "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832839"},
    {MaconFaultId::HighDischargeTemp,        "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832841"},
    {MaconFaultId::TempDifferenceTooLarge,   "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959949"},
    {MaconFaultId::AcCurrentProtection,      "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959948"},
    {MaconFaultId::CoilOverheat,             "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959950"},
    {MaconFaultId::AmbientOutOfRange,        "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000832851"},
    {MaconFaultId::CompressorStartFailure,   "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60001048972"},
    {MaconFaultId::AcVoltageProtection,      "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959947"},
    {MaconFaultId::DcBusVoltageProtection,   "https://arcticheatpumps.freshdesk.com/support/solutions/articles/60000959945"},
};
// clang-format on

const FaultHelpLink* find(const char* code) {
    if (!code) return nullptr;
    MaconFaultId id = macon_fault_id_from_code(code);
    if (id == MaconFaultId::Unknown) return nullptr;
    for (const auto& link : FAULT_HELP_LINKS) {
        if (link.id == id) return &link;
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