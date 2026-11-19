#include "../src/vr_battery.h"
#include "../src/vr_sources.h"

#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    using namespace vrbattery;
    assert(FromOpenVr(0.f) == 0);
    assert(FromOpenVr(0.555f) == 56);
    assert(FromOpenVr(1.f) == 100);
    assert(FromOpenVr(-.01f) == -1 && FromOpenVr(1.01f) == -1);
    assert(FromOpenVr(std::numeric_limits<float>::quiet_NaN()) == -1);

    auto headset = ParseQuestHeadset("  USB powered: true\r\n  status: 2\r\n  level: 3\r\n  scale: 4\r\n");
    assert(headset.percent == 75 && headset.charging && !headset.full);
    auto full = ParseQuestHeadset("  Wireless powered: true\n  status: 5\n  level: 100\n  scale: 100\n");
    assert(full.percent == 100 && full.full && !full.charging);
    assert(ParseQuestHeadset("level: 0\nscale: 100\n").percent == 0);
    assert(ParseQuestHeadset("level: 101\nscale: 100\n").percent == -1);
    assert(ParseQuestHeadset("level: 40\n").percent == -1);

    const std::string left = "  Paired device: TEST-LEFT, Type:   Left, Battery:  0%, Status: Active, ExternalStatus: CONNECTED_ACTIVE\r\n";
    const std::string right = "Paired device: TEST-RIGHT, Type:  Right, Battery:  85%, Status: Active, ExternalStatus: CONNECTED_ACTIVE\n";
    const std::string asleep = "Paired device: TEST-RIGHT, Type: Right, Battery: 85%, ExternalStatus: DISABLED\n";
    auto devices = ParseTouchPlus(right + left, L"TEST-HEADSET");
    assert(devices.size() == 2);
    assert(devices[0].percent == 0 && devices[0].key == L"vr:quest3:TEST-HEADSET:left");
    assert(devices[1].percent == 85 && devices[1].key == L"vr:quest3:TEST-HEADSET:right");
    assert(ParseTouchPlus(asleep + left, L"TEST-HEADSET").size() == 1);
    assert(ParseTouchPlus(left + left + right, L"TEST-HEADSET").front().name == L"Touch Plus Right");
    assert(ParseTouchPlus("Paired device: TEST, Type: Left, Battery: 102%\n", L"TEST-HEADSET").empty());
    assert(ParseTouchPlus("Type: Left, Battery: 90%\n", L"TEST-HEADSET").empty());
    assert(ParseTouchPlus(left, L"").empty());

    assert(IsQuest3("eureka") && IsQuest3("Oculus Quest3"));
    assert(!IsQuest3("Meta Quest 3S") && !IsQuest3("Oculus Quest 2"));
    assert(std::wstring(ViveTrackerModel("lighthouse", "HTC", "Vive Tracker Pro MV", "")) == L"VIVE Tracker 2.0");
    assert(std::wstring(ViveTrackerModel("lighthouse", "HTC", "VIVE Tracker 3.0 MV", "")) == L"VIVE Tracker 3.0");
    assert(!ViveTrackerModel("VirtualDesktop", "HTC", "VIVE Tracker 3.0", "vr_tracker_vive_3_0"));
    assert(!ViveTrackerModel("lighthouse", "HTC", "Vive Tracker PVT", "vr_tracker_vive_1_0"));

    auto steamHeadset = headset;
    steamHeadset.percent = 33;
    steamHeadset.via = L"SteamVR";
    devices.push_back(headset);
    std::vector<DeviceStatus> merged{steamHeadset};
    AppendQuestAdb(merged, devices);
    assert(merged.size() == 3 && merged[0].percent == 33);
    merged.clear();
    AppendQuestAdb(merged, devices);
    assert(merged.size() == 3 && merged.back().via == L"Quest ADB");

    assert(NormalizeAdbEndpoint(L"192.0.2.10") == L"192.0.2.10:5555");
    assert(NormalizeAdbEndpoint(L"[2001:db8::2]:12345") == L"[2001:db8::2]:12345");
    assert(NormalizeAdbEndpoint(L"192.0.2.10:65536").empty());
    assert(NormalizeAdbEndpoint(L"192.0.2.10:").empty());
    assert(NormalizeAdbEndpoint(L"192.0.2.10:0").empty());
    assert(NormalizeAdbEndpoint(L"-a").empty());
    std::puts("VR battery tests passed (ADB readings, missing and asleep controllers, source priority, tracker models, and endpoints).");
}
