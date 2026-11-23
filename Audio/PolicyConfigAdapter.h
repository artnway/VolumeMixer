#pragma once

struct IMMDeviceEnumerator;

class PolicyConfigAdapter final
{
public:
    static HRESULT SetDefaultPlaybackDevice(IMMDeviceEnumerator& devices, std::wstring const& deviceId, unsigned roles);
};
