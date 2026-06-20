// Pull-based OTA from a public GitHub release. The device queries the repo's
// latest release, and if its tag differs from FIRMWARE_VERSION, downloads the
// firmware asset and self-flashes (rebooting on success). All connections are
// device-initiated outbound, so this works through IoT-VLAN isolation that
// blocks inbound/connect-back push-OTA.
#pragma once

#include <stdint.h>

namespace ota {

// True if an update check is due (immediately after boot, then every
// OTA_CHECK_INTERVAL_MS). Gate the call on WiFi being up.
bool check_due(uint32_t now_ms);

// Query GitHub, and if a different release is published, download + flash it.
// Reboots into the new image on success; returns on no-update or failure.
void checkAndUpdate();

}  // namespace ota
