#pragma once

#include <windows.h>

#include "devices.h"

namespace vrbattery {

std::vector<DeviceStatus> PollSteamVr();

struct QuestAdbConfig {
    std::wstring executable;
    std::wstring endpoint;
    bool operator==(const QuestAdbConfig& other) const {
        return executable == other.executable && endpoint == other.endpoint;
    }
    bool Enabled() const { return !executable.empty() && !endpoint.empty(); }
};

QuestAdbConfig LoadQuestAdbConfig();
bool SaveQuestAdbConfig(const QuestAdbConfig& config);
std::wstring NormalizeAdbEndpoint(const std::wstring& address);

class QuestAdbPoller {
public:
    std::vector<DeviceStatus> Poll(const QuestAdbConfig& config, HANDLE stop = nullptr);

private:
    QuestAdbConfig config_;
    ULONGLONG nextConnect_ = 0;
};

}  // namespace vrbattery
