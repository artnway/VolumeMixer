#pragma once

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
#include "AudioModels.h"

class AudioDeviceCallback;
class AudioEndpointCallback;
class AudioSessionNotificationCallback;
class AudioSessionEventCallback;

class AudioStateService final
{
public:
    static constexpr UINT PeakStateMessage = WM_APP + 4;
    static constexpr UINT MasterStateMessage = WM_APP + 7;

    ~AudioStateService();
    void Start(HWND target, UINT stateMessage, unsigned roles, std::vector<std::wstring> hiddenDevices);
    void Stop();
    void RequestStop();
    bool IsStopped();
    void Refresh();
    void SetDevicePreferences(unsigned roles, std::vector<std::wstring> hiddenDevices);
    uint64_t SetMasterVolume(double value, std::wstring const& deviceId);
    void AdjustMasterVolume(double delta);
    void ToggleMasterMute();
    void SetApplicationVolume(std::wstring const& appId, double value, std::wstring const& deviceId);
    void SetApplicationMute(std::wstring const& appId, bool muted);
    void SwitchToDevice(std::wstring const& deviceId);
    void CycleDevice(std::vector<std::wstring> favorites, int steps = 1);
    void StartMetering();
    void StopMetering();
    bool TakeLatestPeaks(AudioPeakSnapshot& snapshot);

private:
    using Task = std::function<void()>;
    struct Session
    {
        winrt::com_ptr<IAudioSessionControl> control;
        winrt::com_ptr<IAudioSessionControl2> control2;
        winrt::com_ptr<ISimpleAudioVolume> volume;
        winrt::com_ptr<IAudioMeterInformation> meter;
        winrt::com_ptr<IAudioSessionEvents> events;
        std::wstring instanceId;
        std::wstring appId;
        std::wstring executablePath;
        std::wstring displayName;
        float meterGain{};
        unsigned displayedVolume{};
        bool muted{};
        size_t peakGroup{};
    };

    static LRESULT CALLBACK WorkerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleWorkerMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void WorkerMain();
    void PostTask(Task task);
    void InitializeAudio();
    void ShutdownAudio();
    void ReloadDevices();
    void ReloadDefaultDevice(bool forceReconnect = false);
    void DisconnectDefaultDevice();
    void RefreshSessions(IAudioSessionControl* pending = nullptr);
    void AddSession(std::vector<Session>& sessions, IAudioSessionControl* control);
    static void ReadSessionState(Session& session);
    void Publish(bool refreshAllSessions = true);
    MasterVolumeState ReadMasterVolume();
    void PublishMasterVolume();
    void PublishPeaks();
    void ClearPendingPeaks();
    void ApplyPendingApplicationVolumes(bool publish);
    void SignalDeviceChanged();
    void SignalVolumeChanged();
    void SignalSessionsChanged();
    void SignalSessionCreated(IAudioSessionControl* control, WPARAM generation);
    void SignalAudioRestart();
    void ChangeDefaultDevice(std::wstring const& id);

    static constexpr UINT TaskMessage = WM_APP + 20;
    static constexpr UINT DeviceChangedMessage = WM_APP + 21;
    static constexpr UINT VolumeChangedMessage = WM_APP + 22;
    static constexpr UINT SessionsChangedMessage = WM_APP + 23;
    static constexpr UINT SessionCreatedMessage = WM_APP + 24;
    static constexpr UINT StopMessage = WM_APP + 25;
    static constexpr UINT MeterMessage = WM_APP + 26;
    static constexpr UINT AudioRestartMessage = WM_APP + 27;
    static constexpr UINT MasterWriteMessage = WM_APP + 28;
    static constexpr UINT MasterChangedMessage = WM_APP + 29;
    static constexpr UINT ApplicationWriteMessage = WM_APP + 30;
    static constexpr UINT_PTR AudioRetryTimer = 2;
    static constexpr UINT_PTR ApplicationWriteTimer = 3;

    friend class ::AudioDeviceCallback;
    friend class ::AudioEndpointCallback;
    friend class ::AudioSessionNotificationCallback;
    friend class ::AudioSessionEventCallback;

    HWND m_target{ nullptr };
    UINT m_stateMessage{ 0 };
    std::atomic<HWND> m_workerWindow{ nullptr };
    HANDLE m_ready{ nullptr };
    HANDLE m_meterTimer{};
    std::thread m_worker;
    std::atomic_bool m_stopping{ true };
    std::atomic_bool m_deviceChangedPending{ false };
    std::atomic_bool m_volumeChangedPending{ false };
    std::atomic_bool m_masterChangedPending{ false };
    struct MasterWrite
    {
        uint64_t sequence;
        float value;
        std::wstring deviceId;
    };
    std::mutex m_masterWriteMutex;
    std::optional<MasterWrite> m_pendingMasterWrite;
    uint64_t m_masterWriteSequence{};
    uint64_t m_appliedMasterWrite{};
    struct ApplicationWrite
    {
        std::wstring deviceId;
        float value;
    };
    std::mutex m_applicationWriteMutex;
    std::unordered_map<std::wstring, ApplicationWrite> m_pendingApplicationWrites;
    std::atomic_bool m_meteringRequested{};
    AudioPeakSnapshot m_sampledPeaks;
    std::mutex m_peakMutex;
    AudioPeakSnapshot m_latestPeaks;
    bool m_peaksReady{};
    bool m_peakMessagePending{};

    winrt::com_ptr<IMMDeviceEnumerator> m_enumerator;
    winrt::com_ptr<IMMDevice> m_device;
    winrt::com_ptr<IAudioEndpointVolume> m_endpointVolume;
    winrt::com_ptr<IAudioSessionManager2> m_sessionManager;
    winrt::com_ptr<IAudioMeterInformation> m_masterMeter;
    float m_masterMeterGain{};
    winrt::com_ptr<IMMNotificationClient> m_deviceEvents;
    winrt::com_ptr<IAudioEndpointVolumeCallback> m_volumeEvents;
    winrt::com_ptr<IAudioSessionNotification> m_sessionEvents;
    WPARAM m_sessionGeneration{};
    std::vector<Session> m_sessions;
    std::vector<AudioDeviceModel> m_devices;
    std::vector<std::wstring> m_hiddenDevices;
    std::wstring m_deviceId;
    std::wstring m_deviceName;
    std::wstring m_deviceIconPath;
    HRESULT m_deviceSwitchResult{ S_OK };
    unsigned m_roles{ 7 };
    ERole m_primaryRole{ eConsole };
};
