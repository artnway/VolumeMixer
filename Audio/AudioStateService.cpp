#include "pch.h"
#include <initguid.h>
#include "AudioStateService.h"
#include "PolicyConfigAdapter.h"
#include <audioclient.h>
#include <appmodel.h>
#include <functiondiscoverykeys_devpkey.h>
#include <filesystem>
#include <cmath>
#include <cwctype>

namespace
{
    constexpr LONG MeterIntervalMs = 20;
    constexpr GUID MasterWriteContext{0x14816d80, 0x223b, 0x4f2e, {0x9b, 0x68, 0xc1, 0xe6, 0xcc, 0x46, 0x34, 0x02}};
    constexpr GUID ApplicationWriteContext{0xc53ed4f4, 0x06fb, 0x4f80, {0x8c, 0x79, 0x86, 0x43, 0xfb, 0x7a, 0xf4, 0xa8}};
    template<typename Interface>
    class ComCallback : public Interface
    {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
        {
            if (!object)
            {
                return E_POINTER;
            }
            if (iid == IID_IUnknown || iid == __uuidof(Interface))
            {
                AddRef();
                *object = static_cast<Interface*>(this);
                return S_OK;
            }
            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return InterlockedIncrement(&m_referenceCount);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            auto const count = InterlockedDecrement(&m_referenceCount);
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

    protected:
        virtual ~ComCallback() = default;

    private:
        ULONG m_referenceCount{ 1 };
    };

    std::wstring DeviceId(IMMDevice* device)
    {
        LPWSTR value{};
        if (!device || FAILED(device->GetId(&value)))
        {
            return {};
        }
        std::wstring result(value);
        CoTaskMemFree(value);
        return result;
    }

    std::wstring DeviceStringProperty(IMMDevice* device, PROPERTYKEY const& key)
    {
        winrt::com_ptr<IPropertyStore> properties;
        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring result;
        if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put())))
        {
            if (SUCCEEDED(properties->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
            {
                result = value.pwszVal;
            }
        }
        PropVariantClear(&value);
        return result;
    }

    std::wstring DeviceName(IMMDevice* device)
    {
        return DeviceStringProperty(device, PKEY_Device_FriendlyName);
    }

    std::wstring DeviceIconPath(IMMDevice* device)
    {
        return DeviceStringProperty(device, PKEY_DeviceClass_IconPath);
    }

    bool IsHeadphoneEndpoint(IMMDevice& device)
    {
        winrt::com_ptr<IPropertyStore> properties;
        if (FAILED(device.OpenPropertyStore(STGM_READ, properties.put()))) return false;
        PROPVARIANT value{};
        bool const headphones = SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &value))
            && value.vt == VT_UI4 && (value.uintVal == Headphones || value.uintVal == Headset);
        PropVariantClear(&value);
        return headphones;
    }

    std::wstring SessionString(HRESULT(STDMETHODCALLTYPE IAudioSessionControl2::*method)(LPWSTR*), IAudioSessionControl2* control)
    {
        LPWSTR value{};
        if (!control || FAILED((control->*method)(&value)) || !value)
        {
            return {};
        }
        std::wstring result(value);
        CoTaskMemFree(value);
        return result;
    }

    void ReadProcessInfo(DWORD processId, bool systemSounds, std::wstring& path, std::wstring& name, std::wstring& aumid)
    {
        if (systemSounds)
        {
            name = L"System Sounds";
            return;
        }
        auto const process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process)
        {
            name = L"Application";
            return;
        }
        wchar_t buffer[MAX_PATH]{};
        DWORD size = ARRAYSIZE(buffer);
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        {
            path.assign(buffer, size);
            name = std::filesystem::path(path).stem().wstring();
        }
        UINT32 length = ARRAYSIZE(buffer);
        if (GetApplicationUserModelId(process, &length, buffer) == ERROR_SUCCESS)
        {
            aumid = buffer;
        }
        CloseHandle(process);
        if (name.empty())
        {
            name = L"Application";
        }
    }

    unsigned Percent(double value)
    {
        return static_cast<unsigned>(std::clamp(std::lround(value * 100.0), 0L, 100L));
    }

    ERole PrimaryRole(unsigned roles)
    {
        return (roles & 1) ? eConsole : (roles & 2) ? eMultimedia : eCommunications;
    }

    bool AudioServiceUnavailable(HRESULT status)
    {
        return status == AUDCLNT_E_SERVICE_NOT_RUNNING || status == RPC_E_DISCONNECTED
            || status == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE);
    }
}

class AudioDeviceCallback final : public ComCallback<IMMNotificationClient>
{
public:
    explicit AudioDeviceCallback(AudioStateService* owner) : m_owner(owner) {}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { m_owner->SignalDeviceChanged(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { m_owner->SignalDeviceChanged(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { m_owner->SignalDeviceChanged(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR) override
    {
        if (flow == eRender)
        {
            m_owner->SignalDeviceChanged();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { m_owner->SignalDeviceChanged(); return S_OK; }
private:
    AudioStateService* m_owner;
};

class AudioEndpointCallback final : public ComCallback<IAudioEndpointVolumeCallback>
{
public:
    explicit AudioEndpointCallback(AudioStateService* owner) : m_owner(owner) {}
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA notification) override
    {
        if (notification && notification->guidEventContext != MasterWriteContext && !m_owner->m_stopping
            && !m_owner->m_masterChangedPending.exchange(true))
        {
            if (!PostMessageW(m_owner->m_workerWindow, AudioStateService::MasterChangedMessage, 0, 0))
            {
                m_owner->m_masterChangedPending = false;
            }
        }
        return S_OK;
    }
private:
    AudioStateService* m_owner;
};

class AudioSessionNotificationCallback final : public ComCallback<IAudioSessionNotification>
{
public:
    AudioSessionNotificationCallback(AudioStateService* owner, WPARAM generation) : m_owner(owner), m_generation(generation) {}
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) override
    {
        m_owner->SignalSessionCreated(session, m_generation);
        return S_OK;
    }
private:
    AudioStateService* m_owner;
    WPARAM const m_generation;
};

class AudioSessionEventCallback final : public ComCallback<IAudioSessionEvents>
{
public:
    explicit AudioSessionEventCallback(AudioStateService* owner) : m_owner(owner) {}
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { m_owner->SignalSessionsChanged(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID context) override
    {
        if (!context || *context != ApplicationWriteContext) m_owner->SignalVolumeChanged();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { m_owner->SignalSessionsChanged(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState state) override
    {
        if (state == AudioSessionStateExpired)
        {
            m_owner->SignalSessionsChanged();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override
    {
        if (reason == DisconnectReasonServerShutdown)
        {
            m_owner->SignalAudioRestart();
        }
        else
        {
            m_owner->SignalSessionsChanged();
        }
        return S_OK;
    }
private:
    AudioStateService* m_owner;
};

AudioStateService::~AudioStateService()
{
    Stop();
}

void AudioStateService::Start(HWND target, UINT stateMessage, unsigned roles, std::vector<std::wstring> hiddenDevices)
{
    if (m_worker.joinable())
    {
        return;
    }
    m_target = target;
    m_stateMessage = stateMessage;
    m_roles = roles;
    m_primaryRole = PrimaryRole(roles);
    m_hiddenDevices = std::move(hiddenDevices);
    m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_ready)
    {
        return;
    }
    m_stopping = false;
    m_worker = std::thread(&AudioStateService::WorkerMain, this);
    WaitForSingleObject(m_ready, INFINITE);
    PostTask([this] { InitializeAudio(); });
}

void AudioStateService::RequestStop()
{
    if (!m_worker.joinable() || m_stopping.exchange(true))
    {
        return;
    }
    if (auto const window = m_workerWindow.load())
    {
        PostMessageW(window, StopMessage, 0, 0);
    }
}

bool AudioStateService::IsStopped()
{
    return !m_worker.joinable() || WaitForSingleObject(m_worker.native_handle(), 0) == WAIT_OBJECT_0;
}

void AudioStateService::Stop()
{
    if (!m_worker.joinable())
    {
        return;
    }
    RequestStop();
    m_worker.join();
    m_meteringRequested = false;
    m_deviceChangedPending = false;
    {
        std::lock_guard lock(m_peakMutex);
        m_peaksReady = false;
        m_peakMessagePending = false;
    }
    if (m_ready)
    {
        CloseHandle(m_ready);
        m_ready = nullptr;
    }
}

void AudioStateService::Refresh()
{
    PostTask([this]
    {
        ReloadDefaultDevice(true);
        Publish();
    });
}

void AudioStateService::SetDevicePreferences(unsigned roles, std::vector<std::wstring> hiddenDevices)
{
    PostTask([this, roles, hiddenDevices = std::move(hiddenDevices)]() mutable
    {
        m_hiddenDevices = std::move(hiddenDevices);
        if (m_roles != roles)
        {
            m_roles = roles;
            m_primaryRole = PrimaryRole(roles);
            m_deviceSwitchResult = S_OK;
            ReloadDefaultDevice();
            Publish();
        }
    });
}

uint64_t AudioStateService::SetMasterVolume(double value, std::wstring const& deviceId)
{
    std::lock_guard lock(m_masterWriteMutex);
    if (m_stopping || !m_workerWindow)
    {
        return 0;
    }
    bool const scheduled = m_pendingMasterWrite.has_value();
    m_pendingMasterWrite = MasterWrite{++m_masterWriteSequence, static_cast<float>(std::clamp(value, 0.0, 1.0)), deviceId};
    if (!scheduled && !PostMessageW(m_workerWindow, MasterWriteMessage, 0, 0))
    {
        m_pendingMasterWrite.reset();
        return 0;
    }
    return m_masterWriteSequence;
}

void AudioStateService::AdjustMasterVolume(double delta)
{
    PostTask([this, delta]
    {
        float volume{};
        if (m_endpointVolume && SUCCEEDED(m_endpointVolume->GetMasterVolumeLevelScalar(&volume)))
        {
            m_endpointVolume->SetMasterVolumeLevelScalar(static_cast<float>(std::clamp(volume + delta, 0.0, 1.0)), &MasterWriteContext);
            PublishMasterVolume();
        }
    });
}

void AudioStateService::ToggleMasterMute()
{
    PostTask([this]
    {
        BOOL muted{};
        if (m_endpointVolume && SUCCEEDED(m_endpointVolume->GetMute(&muted)))
        {
            m_endpointVolume->SetMute(!muted, &MasterWriteContext);
            PublishMasterVolume();
        }
    });
}

void AudioStateService::SetApplicationVolume(std::wstring const& appId, double value, std::wstring const& deviceId)
{
    std::lock_guard lock(m_applicationWriteMutex);
    auto const window = m_workerWindow.load();
    if (m_stopping || !window) return;
    bool const first = m_pendingApplicationWrites.empty();
    m_pendingApplicationWrites.insert_or_assign(appId,
        ApplicationWrite{deviceId, static_cast<float>(std::clamp(value, 0.0, 1.0))});
    if (first && !PostMessageW(window, ApplicationWriteMessage, 0, 0))
    {
        m_pendingApplicationWrites.clear();
    }
}

void AudioStateService::ApplyPendingApplicationVolumes(bool publish)
{
    decltype(m_pendingApplicationWrites) writes;
    {
        std::lock_guard lock(m_applicationWriteMutex);
        writes.swap(m_pendingApplicationWrites);
    }
    if (writes.empty()) return;
    bool matched{};
    for (auto& session : m_sessions)
    {
        auto const write = writes.find(session.appId);
        if (write != writes.end() && write->second.deviceId == m_deviceId && session.volume)
        {
            matched = true;
            session.volume->SetMasterVolume(write->second.value, &ApplicationWriteContext);
            if (publish) ReadSessionState(session);
        }
    }
    if (publish && matched) Publish(false);
}

void AudioStateService::SetApplicationMute(std::wstring const& appId, bool muted)
{
    PostTask([this, appId, muted]
    {
        bool matched{};
        for (auto& session : m_sessions)
        {
            if (session.appId == appId && session.volume)
            {
                session.volume->SetMute(muted, &ApplicationWriteContext);
                ReadSessionState(session);
                matched = true;
            }
        }
        if (matched) Publish(false);
    });
}

void AudioStateService::SwitchToDevice(std::wstring const& deviceId)
{
    PostTask([this, deviceId]
    {
        auto const device = std::find_if(m_devices.begin(), m_devices.end(), [&](AudioDeviceModel const& item) { return item.id == deviceId; });
        if (device != m_devices.end() && std::ranges::find(m_hiddenDevices, deviceId) == m_hiddenDevices.end())
        {
            ChangeDefaultDevice(device->id);
        }
    });
}

void AudioStateService::CycleDevice(std::vector<std::wstring> favorites, int steps)
{
    PostTask([this, favorites = std::move(favorites), steps]
    {
        std::vector<std::wstring> available;
        if (favorites.empty())
        {
            for (auto const& device : m_devices)
            {
                if (std::ranges::find(m_hiddenDevices, device.id) == m_hiddenDevices.end())
                {
                    available.push_back(device.id);
                }
            }
        }
        else
        {
            for (auto const& favorite : favorites)
            {
                if (std::ranges::find(m_devices, favorite, &AudioDeviceModel::id) != m_devices.end()
                    && std::ranges::find(m_hiddenDevices, favorite) == m_hiddenDevices.end())
                {
                    available.push_back(favorite);
                }
            }
        }
        if (available.empty() || !steps)
        {
            return;
        }
        auto const current = std::find(available.begin(), available.end(), m_deviceId);
        int const count = static_cast<int>(available.size());
        int const index = current == available.end() ? (steps > 0 ? -1 : 0) : static_cast<int>(current - available.begin());
        auto const& next = available[(index + steps % count + count) % count];
        if (next == m_deviceId)
        {
            return;
        }
        ChangeDefaultDevice(next);
    });
}

void AudioStateService::ChangeDefaultDevice(std::wstring const& id)
{
    m_deviceSwitchResult = m_enumerator
        ? PolicyConfigAdapter::SetDefaultPlaybackDevice(*m_enumerator, id, m_roles)
        : E_UNEXPECTED;
    ReloadDefaultDevice();
    Publish();
}

void AudioStateService::StartMetering()
{
    if (m_stopping || !m_workerWindow || m_meteringRequested.exchange(true)) return;
    PostTask([this]
    {
        if (!m_meteringRequested) return;
        LARGE_INTEGER due{};
        due.QuadPart = -MeterIntervalMs * 10'000LL;
        if (!m_meterTimer || !SetWaitableTimer(m_meterTimer, &due, MeterIntervalMs, nullptr, nullptr, FALSE))
        {
            SetTimer(m_workerWindow, MeterMessage, MeterIntervalMs, nullptr);
        }
        PublishPeaks();
    });
}

void AudioStateService::StopMetering()
{
    if (!m_meteringRequested.exchange(false)) return;
    ClearPendingPeaks();
    PostTask([this]
    {
        if (m_meterTimer) CancelWaitableTimer(m_meterTimer);
        KillTimer(m_workerWindow, MeterMessage);
    });
}

bool AudioStateService::TakeLatestPeaks(AudioPeakSnapshot& snapshot)
{
    std::lock_guard lock(m_peakMutex);
    m_peakMessagePending = false;
    if (!std::exchange(m_peaksReady, false)) return false;
    std::swap(snapshot, m_latestPeaks);
    return true;
}

void AudioStateService::ClearPendingPeaks()
{
    std::lock_guard lock(m_peakMutex);
    m_peaksReady = false;
}

LRESULT CALLBACK AudioStateService::WorkerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto const create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto const self = reinterpret_cast<AudioStateService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleWorkerMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT AudioStateService::HandleWorkerMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case TaskMessage:
    {
        std::unique_ptr<Task> task(reinterpret_cast<Task*>(lParam));
        (*task)();
        return 0;
    }
    case DeviceChangedMessage:
        m_deviceChangedPending = false;
        ReloadDefaultDevice();
        Publish();
        return 0;
    case VolumeChangedMessage:
        m_volumeChangedPending = false;
        Publish();
        return 0;
    case MasterWriteMessage:
    {
        std::optional<MasterWrite> write;
        {
            std::lock_guard lock(m_masterWriteMutex);
            write.swap(m_pendingMasterWrite);
        }
        if (write)
        {
            if (m_endpointVolume && write->deviceId == m_deviceId)
            {
                m_endpointVolume->SetMasterVolumeLevelScalar(write->value, &MasterWriteContext);
            }
            m_appliedMasterWrite = write->sequence;
            PublishMasterVolume();
        }
        return 0;
    }
    case MasterChangedMessage:
        m_masterChangedPending = false;
        PublishMasterVolume();
        return 0;
    case ApplicationWriteMessage:
        if (!SetTimer(hwnd, ApplicationWriteTimer, 24, nullptr)) ApplyPendingApplicationVolumes(true);
        return 0;
    case SessionsChangedMessage:
        RefreshSessions();
        Publish();
        return 0;
    case SessionCreatedMessage:
    {
        winrt::com_ptr<IAudioSessionControl> control;
        control.attach(reinterpret_cast<IAudioSessionControl*>(lParam));
        if (wParam == m_sessionGeneration && m_sessionManager)
        {
            RefreshSessions(control.get());
            Publish();
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == ApplicationWriteTimer)
        {
            KillTimer(hwnd, ApplicationWriteTimer);
            ApplyPendingApplicationVolumes(true);
            return 0;
        }
        if (wParam == AudioRetryTimer)
        {
            KillTimer(hwnd, AudioRetryTimer);
            ShutdownAudio();
            InitializeAudio();
            return 0;
        }
        if (wParam == MeterMessage)
        {
            PublishPeaks();
            return 0;
        }
        break;
    case AudioRestartMessage:
        if (!SetTimer(hwnd, AudioRetryTimer, 1000, nullptr))
        {
            ReloadDefaultDevice(true);
            Publish();
        }
        return 0;
    case StopMessage:
    {
        ApplyPendingApplicationVolumes(false);
        ShutdownAudio();
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, TaskMessage, SessionCreatedMessage, PM_REMOVE))
        {
            if (pending.message == TaskMessage)
            {
                delete reinterpret_cast<Task*>(pending.lParam);
            }
            if (pending.message == SessionCreatedMessage)
            {
                reinterpret_cast<IAudioSessionControl*>(pending.lParam)->Release();
            }
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        m_workerWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void AudioStateService::WorkerMain()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &AudioStateService::WorkerWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"VolumeMixer.AudioWorker";
    RegisterClassW(&windowClass);
    m_workerWindow = CreateWindowExW(0, windowClass.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, this);
    m_meterTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    SetEvent(m_ready);

    DWORD const handleCount = m_meterTimer ? 1 : 0;
    for (;;)
    {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (m_stopping)
        {
            if (auto const window = m_workerWindow.load())
            {
                HandleWorkerMessage(window, StopMessage, 0, 0);
            }
            break;
        }
        auto const ready = MsgWaitForMultipleObjectsEx(handleCount, &m_meterTimer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ready == WAIT_FAILED) break;
        if (handleCount && ready == WAIT_OBJECT_0) PublishPeaks();
    }
    if (m_meterTimer)
    {
        CloseHandle(m_meterTimer);
        m_meterTimer = nullptr;
    }
    ShutdownAudio();
    CoUninitialize();
}

void AudioStateService::PostTask(Task task)
{
    if (m_stopping || !m_workerWindow)
    {
        return;
    }
    auto* allocated = new Task(std::move(task));
    if (!PostMessageW(m_workerWindow, TaskMessage, 0, reinterpret_cast<LPARAM>(allocated)))
    {
        delete allocated;
    }
}

void AudioStateService::InitializeAudio()
{
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_enumerator.put()))))
    {
        SetTimer(m_workerWindow, AudioRetryTimer, 1500, nullptr);
        Publish();
        return;
    }
    m_deviceEvents = winrt::com_ptr<IMMNotificationClient>(new AudioDeviceCallback(this), winrt::take_ownership_from_abi);
    m_enumerator->RegisterEndpointNotificationCallback(m_deviceEvents.get());
    ReloadDefaultDevice();
    Publish();
}

void AudioStateService::ShutdownAudio()
{
    DisconnectDefaultDevice();
    if (m_enumerator && m_deviceEvents)
    {
        m_enumerator->UnregisterEndpointNotificationCallback(m_deviceEvents.get());
    }
    m_deviceEvents = nullptr;
    m_enumerator = nullptr;
    m_devices.clear();
}

void AudioStateService::DisconnectDefaultDevice()
{
    ++m_sessionGeneration;
    ClearPendingPeaks();
    m_sampledPeaks.sessions.clear();
    for (auto& session : m_sessions)
    {
        if (session.control && session.events)
        {
            session.control->UnregisterAudioSessionNotification(session.events.get());
        }
    }
    m_sessions.clear();
    if (m_sessionManager && m_sessionEvents)
    {
        m_sessionManager->UnregisterSessionNotification(m_sessionEvents.get());
    }
    if (m_endpointVolume && m_volumeEvents)
    {
        m_endpointVolume->UnregisterControlChangeNotify(m_volumeEvents.get());
    }
    m_sessionEvents = nullptr;
    m_volumeEvents = nullptr;
    m_sessionManager = nullptr;
    m_endpointVolume = nullptr;
    m_masterMeter = nullptr;
    m_device = nullptr;
    m_deviceId.clear();
    m_deviceName.clear();
    m_deviceIconPath.clear();
}

void AudioStateService::ReloadDevices()
{
    m_devices.clear();
    if (!m_enumerator)
    {
        return;
    }
    winrt::com_ptr<IMMDeviceCollection> devices;
    if (FAILED(m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.put())) || !devices)
    {
        return;
    }
    UINT count{};
    if (FAILED(devices->GetCount(&count)))
    {
        return;
    }
    for (UINT index = 0; index < count; ++index)
    {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(devices->Item(index, device.put())) || !device)
        {
            continue;
        }
        AudioDeviceModel model;
        model.id = DeviceId(device.get());
        model.name = DeviceName(device.get());
        model.iconPath = DeviceIconPath(device.get());
        model.headphones = IsHeadphoneEndpoint(*device);
        if (!model.id.empty())
        {
            m_devices.push_back(std::move(model));
        }
    }
}

void AudioStateService::ReloadDefaultDevice(bool forceReconnect)
{
    ReloadDevices();
    winrt::com_ptr<IMMDevice> device;
    auto const status = m_enumerator ? m_enumerator->GetDefaultAudioEndpoint(eRender, m_primaryRole, device.put()) : E_UNEXPECTED;
    if (AudioServiceUnavailable(status))
    {
        SignalAudioRestart();
    }
    auto id = SUCCEEDED(status) ? DeviceId(device.get()) : std::wstring{};
    if (id.empty())
    {
        if (m_device) DisconnectDefaultDevice();
        return;
    }

    auto const metadata = std::ranges::find(m_devices, id, &AudioDeviceModel::id);
    auto name = metadata != m_devices.end() ? metadata->name : DeviceName(device.get());
    auto icon = metadata != m_devices.end() ? metadata->iconPath : DeviceIconPath(device.get());
    if (!forceReconnect && id == m_deviceId && m_volumeEvents && m_sessionEvents)
    {
        if (!m_masterMeter) device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr, m_masterMeter.put_void());
        m_deviceName = std::move(name);
        m_deviceIconPath = std::move(icon);
        return;
    }
    if (m_device) DisconnectDefaultDevice();
    m_device = std::move(device);
    m_deviceId = std::move(id);
    m_deviceName = std::move(name);
    m_deviceIconPath = std::move(icon);
    auto const endpointStatus = m_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, m_endpointVolume.put_void());
    if (AudioServiceUnavailable(endpointStatus))
    {
        SignalAudioRestart();
    }
    if (SUCCEEDED(endpointStatus))
    {
        m_volumeEvents = winrt::com_ptr<IAudioEndpointVolumeCallback>(new AudioEndpointCallback(this), winrt::take_ownership_from_abi);
        if (FAILED(m_endpointVolume->RegisterControlChangeNotify(m_volumeEvents.get()))) m_volumeEvents = nullptr;
    }
    m_device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr, m_masterMeter.put_void());
    if (SUCCEEDED(m_device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, m_sessionManager.put_void())))
    {
        m_sessionEvents = winrt::com_ptr<IAudioSessionNotification>(
            new AudioSessionNotificationCallback(this, m_sessionGeneration), winrt::take_ownership_from_abi);
        if (FAILED(m_sessionManager->RegisterSessionNotification(m_sessionEvents.get()))) m_sessionEvents = nullptr;
        RefreshSessions();
    }
}

void AudioStateService::RefreshSessions(IAudioSessionControl* pending)
{
    std::vector<Session> refreshed;
    if (m_sessionManager)
    {
        winrt::com_ptr<IAudioSessionEnumerator> enumerator;
        if (SUCCEEDED(m_sessionManager->GetSessionEnumerator(enumerator.put())))
        {
            int count{};
            enumerator->GetCount(&count);
            for (int index = 0; index < count; ++index)
            {
                winrt::com_ptr<IAudioSessionControl> control;
                if (FAILED(enumerator->GetSession(index, control.put())))
                {
                    continue;
                }
                auto const control2 = control.try_as<IAudioSessionControl2>();
                AudioSessionState activity{};
                if (FAILED(control->GetState(&activity)) || activity == AudioSessionStateExpired)
                {
                    continue;
                }
                auto const id = SessionString(&IAudioSessionControl2::GetSessionInstanceIdentifier, control2.get());
                auto existing = std::find_if(m_sessions.begin(), m_sessions.end(), [&](Session& item) { return item.instanceId == id; });
                if (existing != m_sessions.end())
                {
                    refreshed.push_back(std::move(*existing));
                }
                else
                {
                    AddSession(refreshed, control.get());
                }
            }
        }
    }
    if (pending)
    {
        winrt::com_ptr<IAudioSessionControl2> control2;
        pending->QueryInterface(IID_PPV_ARGS(control2.put()));
        auto const id = SessionString(&IAudioSessionControl2::GetSessionInstanceIdentifier, control2.get());
        auto const exists = std::any_of(refreshed.begin(), refreshed.end(), [&](Session const& item) { return item.instanceId == id; });
        if (!exists)
        {
            AddSession(refreshed, pending);
        }
    }
    for (auto& session : m_sessions)
    {
        if (session.control && session.events)
        {
            session.control->UnregisterAudioSessionNotification(session.events.get());
        }
    }
    std::unordered_map<std::wstring_view, size_t> groups;
    m_sampledPeaks.sessions.clear();
    for (auto& session : refreshed)
    {
        auto const [group, added] = groups.try_emplace(session.appId, groups.size());
        session.peakGroup = group->second;
        if (added) m_sampledPeaks.sessions.push_back({.appId = session.appId});
    }
    ClearPendingPeaks();
    m_sessions = std::move(refreshed);
}

void AudioStateService::AddSession(std::vector<Session>& sessions, IAudioSessionControl* control)
{
    Session session;
    session.control.copy_from(control);
    session.control2 = session.control.try_as<IAudioSessionControl2>();
    session.volume = session.control.try_as<ISimpleAudioVolume>();
    session.meter = session.control.try_as<IAudioMeterInformation>();
    if (!session.control2 || !session.volume)
    {
        return;
    }
    session.instanceId = SessionString(&IAudioSessionControl2::GetSessionInstanceIdentifier, session.control2.get());
    auto const sessionId = SessionString(&IAudioSessionControl2::GetSessionIdentifier, session.control2.get());
    DWORD processId{};
    session.control2->GetProcessId(&processId);
    bool const systemSounds = session.control2->IsSystemSoundsSession() == S_OK;
    std::wstring aumid;
    ReadProcessInfo(processId, systemSounds, session.executablePath, session.displayName, aumid);
    session.appId = systemSounds ? L"system:sounds" : L"session:" + sessionId;
    if (!aumid.empty())
    {
        session.appId = L"aumid:" + aumid;
    }
    else if (!session.executablePath.empty() && !systemSounds)
    {
        session.appId = L"path:" + session.executablePath;
        std::transform(session.appId.begin() + 5, session.appId.end(), session.appId.begin() + 5,
            [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    }
    else if (sessionId.empty() && !systemSounds)
    {
        session.appId = L"instance:" + session.instanceId;
    }
    auto displayName = SessionString(&IAudioSessionControl2::GetDisplayName, session.control2.get());
    if (!displayName.empty() && displayName.front() != L'@')
    {
        session.displayName = std::move(displayName);
    }
    session.events = winrt::com_ptr<IAudioSessionEvents>(new AudioSessionEventCallback(this), winrt::take_ownership_from_abi);
    session.control->RegisterAudioSessionNotification(session.events.get());
    sessions.push_back(std::move(session));
}

MasterVolumeState AudioStateService::ReadMasterVolume()
{
    MasterVolumeState state{};
    state.masterWrite = m_appliedMasterWrite;
    m_masterMeterGain = 0;
    if (m_endpointVolume)
    {
        float volume{};
        BOOL muted{};
        auto const volumeStatus = m_endpointVolume->GetMasterVolumeLevelScalar(&volume);
        auto const muteStatus = m_endpointVolume->GetMute(&muted);
        state.deviceAvailable = SUCCEEDED(volumeStatus) && SUCCEEDED(muteStatus);
        state.masterVolume = Percent(volume);
        state.masterMuted = muted != FALSE;
        float decibels{};
        if (state.deviceAvailable && !state.masterMuted && volume > 0
            && SUCCEEDED(m_endpointVolume->GetMasterVolumeLevel(&decibels)))
        {
            m_masterMeterGain = std::pow(10.0f, decibels / 20.0f);
        }
        if (AudioServiceUnavailable(volumeStatus) || AudioServiceUnavailable(muteStatus))
        {
            SignalAudioRestart();
        }
    }
    return state;
}

void AudioStateService::PublishMasterVolume()
{
    auto state = std::make_unique<MasterVolumeState>(ReadMasterVolume());
    if (PostMessageW(m_target, MasterStateMessage, 0, reinterpret_cast<LPARAM>(state.get())))
    {
        state.release();
    }
}

void AudioStateService::ReadSessionState(Session& session)
{
    float volume{};
    BOOL muted{};
    session.displayedVolume = session.volume && SUCCEEDED(session.volume->GetMasterVolume(&volume))
        ? Percent(volume) : 0;
    session.muted = session.volume && SUCCEEDED(session.volume->GetMute(&muted)) && muted != FALSE;
    session.meterGain = session.muted ? 0 : volume;
}

void AudioStateService::Publish(bool refreshAllSessions)
{
    auto snapshot = std::make_unique<AudioSnapshot>();
    static_cast<MasterVolumeState&>(*snapshot) = ReadMasterVolume();
    snapshot->deviceId = m_deviceId;
    snapshot->deviceName = m_deviceName;
    snapshot->deviceIconPath = m_deviceIconPath;
    snapshot->deviceSwitchResult = m_deviceSwitchResult;
    snapshot->devices = m_devices;
    for (auto& device : snapshot->devices)
    {
        device.isDefault = device.id == m_deviceId;
    }
    snapshot->sessions.reserve(m_sessions.size());
    for (auto& session : m_sessions)
    {
        if (refreshAllSessions) ReadSessionState(session);
        AudioSessionModel state;
        state.instanceId = session.instanceId;
        state.appId = session.appId;
        state.executablePath = session.executablePath;
        state.displayName = session.displayName;
        state.volume = session.displayedVolume;
        state.muted = session.muted;
        snapshot->sessions.push_back(std::move(state));
    }
    if (PostMessageW(m_target, m_stateMessage, 0, reinterpret_cast<LPARAM>(snapshot.get())))
    {
        snapshot.release();
    }
}

void AudioStateService::PublishPeaks()
{
    if (!m_meteringRequested) return;
    auto& snapshot = m_sampledPeaks;
    snapshot.sampledAt = std::chrono::steady_clock::now();
    snapshot.deviceId = m_deviceId;
    snapshot.master = {};
    float masterPeak{};
    if (m_masterMeter && SUCCEEDED(m_masterMeter->GetPeakValue(&masterPeak)))
    {
        snapshot.master = {masterPeak, masterPeak * m_masterMeterGain};
    }
    for (auto& group : snapshot.sessions) group.levels = {};
    for (auto const& session : m_sessions)
    {
        float peak{};
        if (session.meter && SUCCEEDED(session.meter->GetPeakValue(&peak)))
        {
            auto& levels = snapshot.sessions[session.peakGroup].levels;
            levels.source = std::max(levels.source, peak);
            levels.output = std::max(levels.output, peak * session.meterGain);
        }
    }
    std::lock_guard lock(m_peakMutex);
    if (!m_meteringRequested) return;
    m_latestPeaks = snapshot;
    m_peaksReady = true;
    if (!m_peakMessagePending)
    {
        m_peakMessagePending = PostMessageW(m_target, PeakStateMessage, 0, 0) != FALSE;
    }
}

void AudioStateService::SignalDeviceChanged()
{
    if (!m_stopping && !m_deviceChangedPending.exchange(true))
    {
        if (!PostMessageW(m_workerWindow, DeviceChangedMessage, 0, 0))
        {
            m_deviceChangedPending = false;
        }
    }
}

void AudioStateService::SignalVolumeChanged()
{
    if (!m_stopping && !m_volumeChangedPending.exchange(true))
    {
        if (!PostMessageW(m_workerWindow, VolumeChangedMessage, 0, 0))
        {
            m_volumeChangedPending = false;
        }
    }
}

void AudioStateService::SignalSessionsChanged()
{
    if (!m_stopping)
    {
        PostMessageW(m_workerWindow, SessionsChangedMessage, 0, 0);
    }
}

void AudioStateService::SignalAudioRestart()
{
    if (!m_stopping)
    {
        PostMessageW(m_workerWindow, AudioRestartMessage, 0, 0);
    }
}

void AudioStateService::SignalSessionCreated(IAudioSessionControl* control, WPARAM generation)
{
    if (m_stopping || !control)
    {
        return;
    }
    control->AddRef();
    if (!PostMessageW(m_workerWindow, SessionCreatedMessage, generation, reinterpret_cast<LPARAM>(control)))
    {
        control->Release();
    }
}
