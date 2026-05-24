// Bambu Bridge — native cloud device-list (ship-2).
//
// Replaces the proprietary plugin's `get_user_print_info` step. Calls
// GET https://<host>/v1/iot-service/api/user/print?force=true with the
// bearer token from a CloudSessionData and parses the response into the
// same `CloudDevice` shape `parse_user_print_info` produces in the
// slicer.
//
// The raw JSON body is also returned so the caller can hand it directly
// to `DeviceManager::parse_user_print_info(body)` — that function takes
// a JSON string and populates `userMachineList` with full MachineObjects
// (with cert-aware fields the bridge doesn't try to recreate).

#ifndef SLIC3R_BAMBU_BRIDGE_CLOUD_DEVICE_LIST_HPP
#define SLIC3R_BAMBU_BRIDGE_CLOUD_DEVICE_LIST_HPP

#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {

struct CloudSessionData;  // CloudSession.hpp

// Per-printer record parsed out of the bind_list response. Matches the
// shape the slicer's `DeviceManager::parse_user_print_info` consumes.
struct CloudListPrinter {
    std::string dev_id;
    std::string name;
    std::string model;            // dev_model_name
    std::string access_code;      // dev_access_code
    std::string nozzle_diameter;  // string-form
    std::string print_status;
    bool        online = false;
};

struct CloudDeviceListResult {
    bool                          ok = false;
    long                          http_status = 0;
    std::string                   body;             // raw JSON, hand to parse_user_print_info
    std::vector<CloudListPrinter> printers;
    std::string                   error;
};

class CloudDeviceList {
public:
    // GET /v1/iot-service/api/user/print?force=true with Bearer token.
    // On 2xx, populates body + printers. Network/HTTP errors surface
    // via ok=false + error.
    CloudDeviceListResult fetch(const CloudSessionData &session);
};

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_CLOUD_DEVICE_LIST_HPP
