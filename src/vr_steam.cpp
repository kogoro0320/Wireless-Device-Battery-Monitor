#include "vr_sources.h"
#include "vr_battery.h"
#include "platform_compat.h"

// Valve's flat ABI works with both MinGW and the MSVC-built runtime.
#pragma push_macro("__WIN32")
#undef __WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "../vendor/openvr/openvr_capi.h"
#pragma GCC diagnostic pop
#pragma pop_macro("__WIN32")

#include <algorithm>
#include <array>
#include <cstring>

namespace vrbattery {
namespace {

HMODULE LoadOpenVr() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return nullptr;
    executable.resize(length);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return nullptr;
    const std::wstring library = executable.substr(0, separator + 1) + L"openvr_api.dll";
    return LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

template<class Function>
Function LoadFunction(HMODULE module, const char* name) {
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    Function function;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

struct OpenVr {
    using Init = uint32_t (*)(EVRInitError*, EVRApplicationType, const char*);
    using Shutdown = void (*)();
    using GetInterface = intptr_t (*)(const char*, EVRInitError*);

    HMODULE module = LoadOpenVr();
    Init init = LoadFunction<Init>(module, "VR_InitInternal2");
    Shutdown shutdown = LoadFunction<Shutdown>(module, "VR_ShutdownInternal");
    GetInterface getInterface = LoadFunction<GetInterface>(module, "VR_GetGenericInterface");
};

std::string ReadString(const VR_IVRSystem_FnTable& vr, uint32_t index, ETrackedDeviceProperty prop) {
    std::array<char, 512> value{};
    ETrackedPropertyError error = ETrackedPropertyError_TrackedProp_UnknownProperty;
    uint32_t size = vr.GetStringTrackedDeviceProperty(index, prop, value.data(), value.size(), &error);
    if (error != ETrackedPropertyError_TrackedProp_Success || !size || size > value.size() || value[size - 1] != '\0') return {};
    return std::string(value.data());
}

}  // namespace

std::vector<DeviceStatus> PollSteamVr() {
    // Keep Valve's library loaded while any worker could still be returning from a runtime call.
    static const OpenVr sdk;
    if (!sdk.init || !sdk.shutdown || !sdk.getInterface) return {};

    EVRInitError error = EVRInitError_VRInitError_None;
    sdk.init(&error, EVRApplicationType_VRApplication_Background, nullptr);
    if (error != EVRInitError_VRInitError_None) return {};
    struct Session { OpenVr::Shutdown close; ~Session() { close(); } } session{ sdk.shutdown };
    const std::string interfaceName = std::string("FnTable:") + IVRSystem_Version;
    const auto* vr = reinterpret_cast<const VR_IVRSystem_FnTable*>(sdk.getInterface(interfaceName.c_str(), &error));
    if (!vr || error != EVRInitError_VRInitError_None) return {};

    std::vector<DeviceStatus> devices;
    for (uint32_t index = 0; index < k_unMaxTrackedDeviceCount; ++index) {
        if (!vr->IsTrackedDeviceConnected(index)) continue;
        const auto kind = vr->GetTrackedDeviceClass(index);
        if (kind != ETrackedDeviceClass_TrackedDeviceClass_HMD && kind != ETrackedDeviceClass_TrackedDeviceClass_GenericTracker &&
            kind != ETrackedDeviceClass_TrackedDeviceClass_Controller)
            continue;

        const auto model = ReadString(*vr, index, ETrackedDeviceProperty_Prop_ModelNumber_String);
        DeviceStatus device;
        if (kind == ETrackedDeviceClass_TrackedDeviceClass_HMD) {
            if (!IsQuest3(model)) continue;
            device.key = QuestHeadsetKey;
            device.name = L"Meta Quest 3";
            device.kind = DeviceKind::VrHeadset;
        } else {
            const auto driver = ReadString(*vr, index, ETrackedDeviceProperty_Prop_TrackingSystemName_String);
            const auto manufacturer = ReadString(*vr, index, ETrackedDeviceProperty_Prop_ManufacturerName_String);
            const auto render = ReadString(*vr, index, ETrackedDeviceProperty_Prop_RenderModelName_String);
            const auto* name = ViveTrackerModel(driver, manufacturer, model, render);
            if (!name) continue;
            const auto serial = Utf8ToWide(ReadString(*vr, index, ETrackedDeviceProperty_Prop_SerialNumber_String));
            if (serial.empty()) continue;  // A stable identity is required before saving user settings.
            device.key = L"vr:lighthouse:" + serial;
            device.name = name;
            device.detail = serial.substr(serial.size() > 4 ? serial.size() - 4 : 0);
            device.kind = DeviceKind::VrTracker;
        }

        ETrackedPropertyError propertyError = ETrackedPropertyError_TrackedProp_UnknownProperty;
        const float fraction = vr->GetFloatTrackedDeviceProperty(index, ETrackedDeviceProperty_Prop_DeviceBatteryPercentage_Float,
                                                                  &propertyError);
        if (propertyError != ETrackedPropertyError_TrackedProp_Success) continue;
        device.percent = FromOpenVr(fraction);
        if (device.percent < 0) continue;
        const bool charging = vr->GetBoolTrackedDeviceProperty(index, ETrackedDeviceProperty_Prop_DeviceIsCharging_Bool,
                                                               &propertyError);
        device.charging = propertyError == ETrackedPropertyError_TrackedProp_Success && charging;
        device.full = device.charging && device.percent == 100;
        device.via = L"SteamVR";
        device.graceSec = 1;
        if (std::none_of(devices.begin(), devices.end(), [&](const auto& existing) { return existing.key == device.key; }))
            devices.push_back(std::move(device));
    }
    return devices;
}

}  // namespace vrbattery
