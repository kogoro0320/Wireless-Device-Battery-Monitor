#include "vr_battery.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace vrbattery {
namespace {

std::string_view Trim(std::string_view text) {
    constexpr char whitespace[] = " \t\r\n";
    size_t first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}

template<class Handle>
void ForLines(std::string_view text, Handle handle) {
    while (!text.empty()) {
        size_t end = text.find('\n');
        handle(Trim(text.substr(0, end)));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
}

std::string_view Field(std::string_view line, std::string_view name) {
    size_t colon = line.find(':');
    if (colon == std::string_view::npos || Trim(line.substr(0, colon)) != name) return {};
    return Trim(line.substr(colon + 1));
}

int Number(std::string_view text) {
    if (text.empty()) return -1;
    int number = -1;
    auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? number : -1;
}

bool Inactive(std::string_view status) {
    const auto state = Lower(status);
    return state == "disabled" || state == "disconnected" || state == "asleep" ||
           state == "sleeping" || state == "standby" || state == "connected_standby" || state == "off";
}

}  // namespace

int FromOpenVr(float fraction) {
    if (!std::isfinite(fraction) || fraction < 0.f || fraction > 1.f) return -1;
    return static_cast<int>(std::lround(fraction * 100.f));
}

DeviceStatus ParseQuestHeadset(std::string_view output) {
    int level = -1, scale = -1, status = -1;
    bool powered = false;
    ForLines(output, [&](std::string_view line) {
        if (auto value = Field(line, "level"); !value.empty()) level = Number(value);
        if (auto value = Field(line, "scale"); !value.empty()) scale = Number(value);
        if (auto value = Field(line, "status"); !value.empty()) status = Number(value);
        for (auto source : {"AC powered", "USB powered", "Wireless powered", "Dock powered"})
            powered |= Field(line, source) == "true";
    });

    DeviceStatus device;
    device.key = QuestHeadsetKey;
    device.name = L"Meta Quest 3";
    device.via = L"Quest ADB";
    device.kind = DeviceKind::VrHeadset;
    if (scale > 0 && level >= 0 && level <= scale)
        device.percent = static_cast<int>(std::lround(100.0 * level / scale));
    device.charging = status == 2;  // Android BatteryManager.BATTERY_STATUS_CHARGING
    device.full = status == 5 && powered;
    if (device.percent >= 0) device.graceSec = 1;  // OnResults retains it for two refresh intervals.
    return device;
}

std::vector<DeviceStatus> ParseTouchPlus(std::string_view output, const std::wstring& headsetSerial) {
    if (headsetSerial.empty()) return {};
    std::array<int, 2> levels{-1, -1};
    std::array<bool, 2> seen{};
    ForLines(output, [&](std::string_view line) {
        if (line.substr(0, 14) != "Paired device:") return;
        std::string_view type, battery, status, externalStatus;
        while (!line.empty()) {
            size_t next = line.find(',');
            auto field = Trim(line.substr(0, next));
            if (auto value = Field(field, "Type"); !value.empty()) type = value;
            if (auto value = Field(field, "Battery"); !value.empty()) battery = value;
            if (auto value = Field(field, "Status"); !value.empty()) status = value;
            if (auto value = Field(field, "ExternalStatus"); !value.empty()) externalStatus = value;
            if (next == std::string_view::npos) break;
            line.remove_prefix(next + 1);
        }
        const auto side = Lower(type);
        if (side != "left" && side != "right") return;
        const size_t index = side == "right";
        if (seen[index]) { levels[index] = -1; return; }
        seen[index] = true;
        if (Inactive(status) || Inactive(externalStatus) || battery.empty() || battery.back() != '%') return;
        int level = Number(Trim(battery.substr(0, battery.size() - 1)));
        if (level >= 0 && level <= 100) levels[index] = level;
    });

    std::vector<DeviceStatus> devices;
    for (size_t i = 0; i < levels.size(); ++i) {
        if (levels[i] < 0) continue;
        auto& device = devices.emplace_back();
        device.key = L"vr:quest3:" + headsetSerial + (i == 0 ? L":left" : L":right");
        device.name = i == 0 ? L"Touch Plus Left" : L"Touch Plus Right";
        device.via = L"Quest ADB";
        device.kind = DeviceKind::VrController;
        device.percent = levels[i];
        device.graceSec = 1;
    }
    return devices;
}

bool IsQuest3(std::string_view model) {
    const auto name = Lower(Trim(model));
    return name == "quest 3" || name == "quest3" || name == "meta quest 3" || name == "meta quest3" ||
           name == "oculus quest 3" || name == "oculus quest3" || name == "eureka";
}

const wchar_t* ViveTrackerModel(std::string_view driver, std::string_view manufacturer,
                                std::string_view model, std::string_view renderModel) {
    if (Lower(driver) != "lighthouse" || Lower(manufacturer).find("htc") == std::string::npos) return nullptr;
    auto name = Lower(model);
    auto render = Lower(renderModel);
    if (name.find("vive tracker 3.0") != std::string::npos || render.find("vr_tracker_vive_3_0") != std::string::npos)
        return L"VIVE Tracker 3.0";
    if (name.find("vive tracker pro") != std::string::npos || name.find("vive tracker (2018)") != std::string::npos ||
        name.find("vive tracker 2.0") != std::string::npos || render.find("vr_tracker_vive_2_0") != std::string::npos)
        return L"VIVE Tracker 2.0";
    return nullptr;
}

void AppendQuestAdb(std::vector<DeviceStatus>& devices, std::vector<DeviceStatus> adb) {
    const bool steamHeadset = std::any_of(devices.begin(), devices.end(), [](const DeviceStatus& device) {
        return device.kind == DeviceKind::VrHeadset && device.percent >= 0;
    });
    for (auto& device : adb) {
        if (!steamHeadset || device.kind != DeviceKind::VrHeadset) devices.push_back(std::move(device));
    }
}

}  // namespace vrbattery
