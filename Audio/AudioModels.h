#pragma once

#include <chrono>

struct AudioSessionModel
{
    std::wstring instanceId;
    std::wstring appId;
    std::wstring executablePath;
    std::wstring displayName;
    unsigned volume{ 0 };
    bool muted{ false };
};

struct AudioDeviceModel
{
    std::wstring id;
    std::wstring name;
    std::wstring iconPath;
    bool isDefault{ false };
    bool headphones{};
    bool operator==(AudioDeviceModel const&) const = default;
};

struct AudioMeterLevels
{
    float source{};
    float output{};
};

struct AudioPeakModel
{
    std::wstring appId;
    AudioMeterLevels levels;
};

struct AudioPeakSnapshot
{
    std::wstring deviceId;
    std::chrono::steady_clock::time_point sampledAt;
    AudioMeterLevels master;
    std::vector<AudioPeakModel> sessions;
};

struct MasterVolumeState
{
    bool deviceAvailable{ false };
    unsigned masterVolume{ 0 };
    bool masterMuted{ false };
    uint64_t masterWrite{};
};

struct AudioSnapshot : MasterVolumeState
{
    std::wstring deviceId;
    std::wstring deviceName;
    std::wstring deviceIconPath;
    HRESULT deviceSwitchResult{ S_OK };
    std::vector<AudioDeviceModel> devices;
    std::vector<AudioSessionModel> sessions;
};
