#include "core/backends/orion/SdrPlayApi.h"

#include <dlfcn.h>

#include <mutex>

namespace AetherSDR::orion {

namespace {

template <typename Fn>
bool resolve(void* handle, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(::dlsym(handle, name));
    return out != nullptr;
}

}  // namespace

const SdrPlayApi* SdrPlayApi::load(QString* error)
{
    static std::once_flag once;
    static SdrPlayApi api;
    static bool ok = false;
    static QString reason;

    std::call_once(once, [] {
        // The versioned name first: the plain .so link is a development
        // package convenience that not every install provides.
        void* handle = nullptr;
        for (const char* name : {"libsdrplay_api.so.3", "libsdrplay_api.so"}) {
            handle = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (handle) {
                break;
            }
        }
        if (!handle) {
            reason = QStringLiteral("the SDRplay API is not installed (libsdrplay_api not found)");
            return;
        }
        const bool all =
            resolve(handle, "sdrplay_api_Open", api.open)
            && resolve(handle, "sdrplay_api_Close", api.close)
            && resolve(handle, "sdrplay_api_ApiVersion", api.apiVersion)
            && resolve(handle, "sdrplay_api_LockDeviceApi", api.lockDeviceApi)
            && resolve(handle, "sdrplay_api_UnlockDeviceApi", api.unlockDeviceApi)
            && resolve(handle, "sdrplay_api_GetDevices", api.getDevices)
            && resolve(handle, "sdrplay_api_SelectDevice", api.selectDevice)
            && resolve(handle, "sdrplay_api_ReleaseDevice", api.releaseDevice)
            && resolve(handle, "sdrplay_api_GetErrorString", api.getErrorString)
            && resolve(handle, "sdrplay_api_GetDeviceParams", api.getDeviceParams)
            && resolve(handle, "sdrplay_api_Init", api.init)
            && resolve(handle, "sdrplay_api_Uninit", api.uninit)
            && resolve(handle, "sdrplay_api_Update", api.update);
        if (!all) {
            reason = QStringLiteral("the installed SDRplay API is missing an entry point (need v3)");
            ::dlclose(handle);
            return;
        }
        // The handle stays open for the life of the process: the API's own
        // threads may still be winding down when a backend is destroyed.
        ok = true;
    });

    if (!ok) {
        if (error) {
            *error = reason;
        }
        return nullptr;
    }
    return &api;
}

QString SdrPlayApi::errorText(sdrplay_api_ErrT e) const
{
    const char* text = getErrorString ? getErrorString(e) : nullptr;
    return text ? QString::fromLatin1(text) : QStringLiteral("error %1").arg(static_cast<int>(e));
}

}  // namespace AetherSDR::orion
