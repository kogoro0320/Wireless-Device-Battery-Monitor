#pragma once

#include "devices.h"

#include <string_view>

namespace vrbattery {

constexpr wchar_t QuestHeadsetKey[] = L"vr:quest3:headset";

constexpr bool IsVrKind(DeviceKind kind) {
    return kind == DeviceKind::VrHeadset || kind == DeviceKind::VrController || kind == DeviceKind::VrTracker;
}

int FromOpenVr(float fraction);
DeviceStatus ParseQuestHeadset(std::string_view output);
std::vector<DeviceStatus> ParseTouchPlus(std::string_view output, const std::wstring& headsetSerial);

bool IsQuest3(std::string_view model);
const wchar_t* ViveTrackerModel(std::string_view driver, std::string_view manufacturer,
                                std::string_view model, std::string_view renderModel);

// Each input is independently refreshable. A fresh SteamVR headset reading takes priority.
void AppendQuestAdb(std::vector<DeviceStatus>& devices, std::vector<DeviceStatus> adb);

}  // namespace vrbattery
