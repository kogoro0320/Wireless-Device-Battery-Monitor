#include <winsock2.h>
#include <ws2tcpip.h>

#include "vr_sources.h"
#include "vr_battery.h"
#include "platform_compat.h"

#include <array>
#include <initializer_list>
#include <memory>
#include <optional>

namespace vrbattery {
namespace {

constexpr wchar_t kSettings[] = L"Software\\WirelessBattery";
constexpr wchar_t kAdbConfig[] = L"QuestAdb";
constexpr size_t kMaxOutput = 256 * 1024;

struct Close { void operator()(HANDLE handle) const { CloseHandle(handle); } };
using Handle = std::unique_ptr<void, Close>;

bool ValidExecutable(const std::wstring& path) {
    const auto name = path.find_last_of(L"\\/");
    if (name == std::wstring::npos || path.find_first_of(L"\"\r\n") != std::wstring::npos ||
        _wcsicmp(path.c_str() + name + 1, L"adb.exe")) return false;
    const bool drivePath = path.size() >= 3 && path[1] == L':' && path[2] == L'\\';
    if (!drivePath && path.rfind(L"\\\\", 0) != 0) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<std::string> Run(const std::wstring& executable, std::initializer_list<std::wstring_view> args,
                               DWORD timeoutMs, HANDLE stop) {
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return std::nullopt;
    std::wstring command = L"\"" + executable + L"\"";
    for (auto arg : args) {
        command += L" \"";
        command.append(arg);
        command += L'"';
    }

    SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) return std::nullopt;
    Handle reader(readPipe), writer(writePipe);
    if (!SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0)) return std::nullopt;
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nul == INVALID_HANDLE_VALUE) return std::nullopt;
    Handle input(nul);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = writer.get();
    startup.StartupInfo.hStdError = writer.get();
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) return std::nullopt;
    std::vector<unsigned char> attributes(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return std::nullopt;
    HANDLE allowed[] = { input.get(), writer.get() };
    PROCESS_INFORMATION process{};
    bool started = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        allowed, sizeof(allowed), nullptr, nullptr) &&
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) return std::nullopt;
    CloseHandle(process.hThread);
    Handle child(process.hProcess);
    writer.reset();

    std::string output;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        if (GetTickCount64() >= deadline || (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)) break;
        DWORD available = 0;
        if (!PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr) && GetLastError() != ERROR_BROKEN_PIPE)
            break;
        if (available) {
            std::array<char, 4096> buffer;
            DWORD read = 0;
            if (!ReadFile(reader.get(), buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())), &read, nullptr))
                break;
            if (output.size() + read > kMaxOutput) break;
            output.append(buffer.data(), read);
            continue;
        }
        if (WaitForSingleObject(child.get(), 20) == WAIT_OBJECT_0) {
            // Drain bytes written between the pipe check and the process exit.
            if (PeekNamedPipe(reader.get(), nullptr, 0, nullptr, &available, nullptr) && available) continue;
            DWORD code = 1;
            if (GetExitCodeProcess(child.get(), &code) && code == 0) return output;
            return std::nullopt;
        }
    }
    TerminateProcess(child.get(), 1);
    return std::nullopt;
}

std::string_view ReadMarker(std::string_view text, std::string_view marker) {
    const size_t index = text.find(marker);
    if (index == std::string_view::npos) return {};
    text.remove_prefix(index + marker.size());
    return text.substr(0, text.find_first_of("\r\n"));
}

std::optional<std::vector<DeviceStatus>> ReadQuest(const QuestAdbConfig& config, HANDLE stop) {
    constexpr wchar_t command[] = L"printf 'BatteryHubModel='; getprop ro.product.device; "
        L"printf 'BatteryHubSerial='; getprop ro.serialno; "
        L"echo BatteryHubBattery; dumpsys -t 2 battery; "
        L"echo BatteryHubControllers; dumpsys -t 2 OVRRemoteService; echo BatteryHubEnd";
    const auto response = Run(config.executable, {L"-s", config.endpoint, L"shell", L"-T", command}, 6000, stop);
    if (!response) return std::nullopt;
    std::string_view output = *response;
    const auto model = ReadMarker(output, "BatteryHubModel=");
    const auto serial = ReadMarker(output, "BatteryHubSerial=");
    const size_t battery = output.find("BatteryHubBattery");
    const size_t controllers = output.find("BatteryHubControllers");
    const size_t end = output.find("BatteryHubEnd");
    if (model.empty() || !(battery < controllers && controllers < end && end < output.size())) return std::nullopt;
    if (!IsQuest3(model)) return std::vector<DeviceStatus>{};  // Endpoint is reachable, but it is not a Quest 3.

    auto devices = ParseTouchPlus(output.substr(controllers, end - controllers), Utf8ToWide(std::string(serial)));
    auto headset = ParseQuestHeadset(output.substr(battery, controllers - battery));
    if (headset.percent >= 0) devices.push_back(std::move(headset));
    return devices;
}

}  // namespace

std::wstring NormalizeAdbEndpoint(const std::wstring& value) {
    if (value.empty() || value.size() > 80) return {};
    std::wstring address = value, port = L"5555";
    if (value.front() == L'[') {
        const auto close = value.find(L']');
        if (close == std::wstring::npos) return {};
        address = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != L':') return {};
            port = value.substr(close + 2);
        }
        IN6_ADDR ipv6{};
        if (InetPtonW(AF_INET6, address.c_str(), &ipv6) != 1) return {};
        address = L"[" + address + L"]";
    } else {
        if (auto colon = value.find(L':'); colon != std::wstring::npos) {
            address = value.substr(0, colon);
            port = value.substr(colon + 1);
        }
        IN_ADDR ipv4{};
        if (InetPtonW(AF_INET, address.c_str(), &ipv4) != 1) return {};
    }
    if (port.empty() || port.size() > 5 || port.find_first_not_of(L"0123456789") != std::wstring::npos) return {};
    const unsigned long number = wcstoul(port.c_str(), nullptr, 10);
    if (!number || number > 65535) return {};
    return address + L":" + std::to_wstring(number);
}

QuestAdbConfig LoadQuestAdbConfig() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettings, kAdbConfig, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kSettings, kAdbConfig, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
        return {};
    value.resize(wcslen(value.c_str()));
    auto separator = value.find(L'\n');
    if (separator == std::wstring::npos) return {};
    return { value.substr(0, separator), NormalizeAdbEndpoint(value.substr(separator + 1)) };
}

bool SaveQuestAdbConfig(const QuestAdbConfig& config) {
    if (config.Enabled() && (!ValidExecutable(config.executable) || NormalizeAdbEndpoint(config.endpoint).empty())) return false;
    const std::wstring value = config.Enabled() ? config.executable + L'\n' + config.endpoint : L"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kSettings, kAdbConfig, REG_SZ,
                          value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

std::vector<DeviceStatus> QuestAdbPoller::Poll(const QuestAdbConfig& config, HANDLE stop) {
    if (!(config_ == config)) {
        config_ = config;
        nextConnect_ = 0;
    }
    if (!config.Enabled() || !ValidExecutable(config.executable)) return {};
    if (auto devices = ReadQuest(config, stop)) return std::move(*devices);
    if (GetTickCount64() < nextConnect_) return {};

    nextConnect_ = GetTickCount64() + 15 * 1000;
    if (!Run(config.executable, {L"connect", config.endpoint}, 2500, stop)) return {};
    if (auto devices = ReadQuest(config, stop)) return std::move(*devices);
    return {};
}

}  // namespace vrbattery
