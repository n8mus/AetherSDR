#pragma once

// The SDRplay API v3, loaded at run time.
//
// libsdrplay_api is proprietary and must never ship with a release (fork
// rule 5). Linking it would let an AppImage or installer bundle it, so the
// backend never links it: it opens the operator's installed copy with
// dlopen() and resolves the few entry points it uses. A machine without the
// API simply has no RSP2 and the backend runs CAT-only.
//
// The header (sdrplay_api.h) comes from the operator's API install at build
// time and is never vendored into this repository.

#include <sdrplay_api.h>

#include <QString>

namespace AetherSDR::orion {

struct SdrPlayApi {
    decltype(&sdrplay_api_Open) open = nullptr;
    decltype(&sdrplay_api_Close) close = nullptr;
    decltype(&sdrplay_api_ApiVersion) apiVersion = nullptr;
    decltype(&sdrplay_api_LockDeviceApi) lockDeviceApi = nullptr;
    decltype(&sdrplay_api_UnlockDeviceApi) unlockDeviceApi = nullptr;
    decltype(&sdrplay_api_GetDevices) getDevices = nullptr;
    decltype(&sdrplay_api_SelectDevice) selectDevice = nullptr;
    decltype(&sdrplay_api_ReleaseDevice) releaseDevice = nullptr;
    decltype(&sdrplay_api_GetErrorString) getErrorString = nullptr;
    decltype(&sdrplay_api_GetDeviceParams) getDeviceParams = nullptr;
    decltype(&sdrplay_api_Init) init = nullptr;
    decltype(&sdrplay_api_Uninit) uninit = nullptr;
    decltype(&sdrplay_api_Update) update = nullptr;

    // Loads the library once per process. Returns nullptr, with the reason
    // in *error, when it is not installed or an entry point is missing.
    static const SdrPlayApi* load(QString* error);

    [[nodiscard]] QString errorText(sdrplay_api_ErrT e) const;
};

}  // namespace AetherSDR::orion
