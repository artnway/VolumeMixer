#include "pch.h"
#include "ApplicationController.h"
#include "../AppResources.h"
#include <windowsx.h>

namespace
{
    constexpr wchar_t HostClassName[] = L"VolumeMixer.ShellHost";
    constexpr wchar_t InstanceMutexName[] = L"Local\\SelfProject.VolumeMixer.SingleInstance";
    constexpr ULONGLONG WidgetLossGraceMs = 750;
    constexpr ULONGLONG WidgetRecoveryGraceMs = 2000;
}

ApplicationController::~ApplicationController()
{
    if (m_largeIcon)
    {
        DestroyIcon(m_largeIcon);
    }
    if (m_smallIcon)
    {
        DestroyIcon(m_smallIcon);
    }
    if (m_singleInstance)
    {
        ReleaseMutex(m_singleInstance);
        CloseHandle(m_singleInstance);
    }
}

bool ApplicationController::Initialize()
{
    m_singleInstance = CreateMutexW(nullptr, TRUE, InstanceMutexName);
    if (!m_singleInstance)
    {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (auto const existing = FindWindowW(HostClassName, nullptr))
        {
            PostMessageW(existing, ShowMixerMessage, 0, 0);
        }
        return false;
    }

    auto const module = GetModuleHandleW(nullptr);
    auto const icon = MAKEINTRESOURCEW(IDI_VOLUME_MIXER);
    m_largeIcon = static_cast<HICON>(LoadImageW(module, icon, IMAGE_ICON, 32, 32, 0));
    m_smallIcon = static_cast<HICON>(LoadImageW(module, icon, IMAGE_ICON, 16, 16, 0));

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &ApplicationController::HostWndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = HostClassName;
    windowClass.hIcon = m_largeIcon;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    m_host = CreateWindowExW(WS_EX_TOOLWINDOW, HostClassName, L"Volume Mixer", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, windowClass.hInstance, this);
    if (!m_host)
    {
        return false;
    }

    m_settings = m_settingsManager.Load();
    m_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    m_taskbar.Initialize(m_host);
    ReadTaskbarTheme();
    m_hotkeyError = m_hotkeys.Initialize(m_host, m_settings.hotkeys);
    ApplySettings();
    m_audio.Start(m_host, AudioStateMessage, m_settings.deviceRoleMask, m_settings.hiddenDevices);
    if (!m_settings.startMinimized)
    {
        ShowMixer(TrayAnchor());
    }
    return true;
}

void ApplicationController::Exit()
{
    if (m_exiting)
    {
        return;
    }
    m_exiting = true;
    m_hotkeys.Shutdown();
    m_taskbar.RequestStop();
    m_tray.Shutdown();
    if (m_settingsWindow)
    {
        m_settingsWindow->Hide();
    }
    if (m_mixer)
    {
        m_mixer->HideMixer();
    }
    m_audio.RequestStop();
    if ((m_audio.IsStopped() && m_taskbar.IsStopped()) || !SetTimer(m_host, ExitTimer, 40, nullptr))
    {
        FinishExit();
    }
}

void ApplicationController::FinishExit()
{
    KillTimer(m_host, ExitTimer);
    m_audio.Stop();
    m_taskbar.Stop();
    MSG pending{};
    while (PeekMessageW(&pending, m_host, AudioStateService::MasterStateMessage, AudioStateService::MasterStateMessage, PM_REMOVE))
    {
        delete reinterpret_cast<MasterVolumeState*>(pending.lParam);
    }
    while (PeekMessageW(&pending, m_host, AudioStateMessage, AudioStateMessage, PM_REMOVE))
    {
        delete reinterpret_cast<AudioSnapshot*>(pending.lParam);
    }
    if (m_settingsWindow)
    {
        m_settingsWindow->Destroy();
        m_settingsWindow = nullptr;
    }
    if (m_mixer)
    {
        m_mixer->Destroy();
        m_mixer = nullptr;
    }
    if (m_host)
    {
        DestroyWindow(m_host);
    }
    winrt::Microsoft::UI::Xaml::Application::Current().Exit();
}

LRESULT CALLBACK ApplicationController::HostWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto const create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto const self = reinterpret_cast<ApplicationController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ApplicationController::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (m_exiting)
    {
        switch (message)
        {
        case WM_TIMER:
            if (wParam == ExitTimer && m_audio.IsStopped() && m_taskbar.IsStopped())
            {
                FinishExit();
            }
            return 0;
        case AudioStateMessage:
            delete reinterpret_cast<AudioSnapshot*>(lParam);
            return 0;
        case AudioStateService::MasterStateMessage:
            delete reinterpret_cast<MasterVolumeState*>(lParam);
            return 0;
        case WM_CLOSE:
            return 0;
        case WM_DESTROY:
        case WM_NCDESTROY:
            break;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }
    if (m_taskbarCreatedMessage && message == m_taskbarCreatedMessage)
    {
        ReadTaskbarTheme();
        m_taskbar.RecheckLayout();
        if (m_trayWanted)
        {
            m_tray.Recreate();
        }
        RefreshWidgetLayout(true);
        return 0;
    }

    switch (message)
    {
    case ShowMixerMessage:
        ShowMixer(TrayAnchor());
        return 0;
    case WM_TIMER:
        if (wParam == TrayIconManager::RetryTimer)
        {
            if (m_trayWanted) m_tray.EnsureRegistered();
            return 0;
        }
        if (wParam != TaskbarWidgetManager::LayoutTimer)
        {
            break;
        }
        [[fallthrough]];
    case TaskbarWidgetManager::LayoutMessage:
        RefreshWidgetLayout();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        RefreshWidgetLayout(true);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ReadTaskbarTheme();
        if (m_mixer)
        {
            m_mixer->RefreshColors();
        }
        RefreshWidgetLayout(true);
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC)
        {
            ReadTaskbarTheme();
            m_taskbar.RecheckLayout();
            RefreshWidgetLayout(true);
            m_audio.Refresh();
        }
        return TRUE;
    case TaskbarWidgetManager::InputMessage:
        switch (static_cast<TaskbarWidgetManager::Input>(wParam))
        {
        case TaskbarWidgetManager::Input::ToggleMixer:
            ToggleMixer({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            break;
        case TaskbarWidgetManager::Input::Menu:
            ShowTrayMenu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            break;
        case TaskbarWidgetManager::Input::Mute:
            m_audio.ToggleMasterMute();
            break;
        case TaskbarWidgetManager::Input::Volume:
            if (m_settings.taskbarWheelEnabled)
            {
                m_audio.AdjustMasterVolume(static_cast<int>(lParam) * m_settings.wheelStep / 100.0);
            }
            break;
        case TaskbarWidgetManager::Input::Device:
            if (m_settings.taskbarWheelEnabled)
            {
                m_audio.CycleDevice(m_settings.favoriteDevices, static_cast<int>(lParam));
            }
            break;
        }
        return 0;
    case AudioStateMessage:
    {
        std::unique_ptr<AudioSnapshot> snapshot(reinterpret_cast<AudioSnapshot*>(lParam));
        if (!snapshot)
        {
            return 0;
        }
        UpdateIntegration(*snapshot);
        if (m_mixer && IsWindowVisible(m_mixer->Handle()))
        {
            m_mixer->ApplySnapshot(*snapshot);
        }
        else
        {
            m_pendingMixerSnapshot = std::move(snapshot);
        }
        return 0;
    }
    case AudioStateService::PeakStateMessage:
    {
        if (m_audio.TakeLatestPeaks(m_peaks) && m_mixer && IsWindowVisible(m_mixer->Handle()))
        {
            m_mixer->ApplyPeaks(m_peaks);
        }
        return 0;
    }
    case AudioStateService::MasterStateMessage:
    {
        std::unique_ptr<MasterVolumeState> state(reinterpret_cast<MasterVolumeState*>(lParam));
        if (state)
        {
            if (m_mixer && IsWindowVisible(m_mixer->Handle()))
            {
                m_mixer->ApplyMasterVolume(*state);
            }
            if (UpdateMasterState(*state)) RefreshIntegration();
        }
        return 0;
    }
    case TrayIconManager::CallbackMessage:
    {
        auto const notification = LOWORD(lParam);
        POINT const anchor{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
        if (notification == NIN_SELECT || notification == NIN_KEYSELECT || notification == WM_LBUTTONUP)
        {
            ToggleMixer(anchor);
        }
        else if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP)
        {
            POINT cursor{};
            GetCursorPos(&cursor);
            ShowTrayMenu(cursor);
        }
        return 0;
    }
    case WM_HOTKEY:
        if (m_settingsWindow && m_settingsWindow->CaptureRegisteredHotkey(LOWORD(lParam), HIWORD(lParam)))
        {
            return 0;
        }
        switch (static_cast<HotkeyAction>(wParam))
        {
        case HotkeyAction::Next:
            m_audio.CycleDevice(m_settings.favoriteDevices);
            return 0;
        case HotkeyAction::Previous:
            m_audio.CycleDevice(m_settings.favoriteDevices, -1);
            return 0;
        case HotkeyAction::Mixer:
            ToggleMixer(TrayAnchor());
            return 0;
        case HotkeyAction::Mute:
            m_audio.ToggleMasterMute();
            return 0;
        case HotkeyAction::Favorite1:
        case HotkeyAction::Favorite2:
        case HotkeyAction::Favorite3:
        {
            auto const number = wParam - static_cast<WPARAM>(HotkeyAction::Favorite1);
            if (number < m_settings.favoriteDevices.size())
            {
                m_audio.SwitchToDevice(m_settings.favoriteDevices[number]);
            }
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == MenuOpen)
        {
            ShowMixer(TrayAnchor());
            return 0;
        }
        if (LOWORD(wParam) == MenuSettings)
        {
            ShowSettings();
            return 0;
        }
        if (LOWORD(wParam) == MenuMute)
        {
            m_audio.ToggleMasterMute();
            return 0;
        }
        if (LOWORD(wParam) == MenuExit)
        {
            Exit();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (!m_exiting)
        {
            PostQuitMessage(0);
        }
        return 0;
    case WM_NCDESTROY:
        m_host = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ApplicationController::ToggleMixer(POINT anchor)
{
    if (m_mixer && IsWindowVisible(m_mixer->Handle()))
    {
        m_mixer->HideMixer();
    }
    else
    {
        ShowMixer(anchor);
    }
}

void ApplicationController::ShowMixer(POINT anchor)
{
    if (!m_mixer)
    {
        m_mixer = winrt::make_self<winrt::VolumeMixer::implementation::MainWindow>();
        m_mixer->SetAudioService(&m_audio);
        m_mixer->SetSettingsHandler([this] { ShowSettings(); });
        m_mixer->SetSettingsChangedHandler([this](SettingsModel const& settings)
        {
            m_settings = settings;
            return m_settingsManager.Save(settings);
        });
        m_mixer->ApplySettings(m_settings);
    }
    if (m_pendingMixerSnapshot)
    {
        static_cast<MasterVolumeState&>(*m_pendingMixerSnapshot) = m_masterState;
        m_mixer->ApplySnapshot(*m_pendingMixerSnapshot);
        m_pendingMixerSnapshot.reset();
    }
    else
    {
        m_mixer->ApplyMasterVolume(m_masterState);
    }
    m_mixer->ShowAt(anchor);
}

void ApplicationController::ShowTrayMenu(POINT anchor)
{
    auto const menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    auto devices = m_devices;
    std::erase_if(devices, [&](auto const& device) { return std::ranges::find(m_settings.hiddenDevices, device.id) != m_settings.hiddenDevices.end(); });
    for (size_t index = 0; index < devices.size(); ++index)
    {
        auto const& device = devices[index];
        auto name = device.name;
        for (size_t amp{}; (amp = name.find(L'&', amp)) != std::wstring::npos; amp += 2)
        {
            name.insert(amp, 1, L'&');
        }
        AppendMenuW(menu, MF_STRING | (device.isDefault ? MF_CHECKED : 0), MenuDevice + index, name.c_str());
    }
    if (!devices.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuOpen, L"Open Mixer");
    AppendMenuW(menu, MF_STRING | (m_masterState.deviceAvailable ? 0 : MF_GRAYED),
        MenuMute, m_masterState.masterMuted ? L"Unmute" : L"Mute");
    AppendMenuW(menu, MF_STRING, MenuSettings, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MenuExit, L"Exit");
    SetForegroundWindow(m_host);
    auto const command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, anchor.x, anchor.y, 0, m_host, nullptr);
    DestroyMenu(menu);
    if (command >= MenuDevice && static_cast<size_t>(command - MenuDevice) < devices.size())
    {
        m_audio.SwitchToDevice(devices[command - MenuDevice].id);
    }
    else if (command == MenuOpen)
    {
        ShowMixer(anchor);
    }
    else if (command)
    {
        SendMessageW(m_host, WM_COMMAND, command, 0);
    }
}

void ApplicationController::ShowSettings()
{
    if (m_settingsWindow && m_settingsWindow->IsVisible())
    {
        m_settingsWindow->Show();
        return;
    }
    if (!m_settingsWindow)
    {
        m_settingsWindow = winrt::make_self<winrt::VolumeMixer::implementation::SettingsWindow>();
        m_settingsWindow->SetSaveHandler([this](SettingsModel& settings, SettingsModel const& saved) -> std::wstring
        {
            bool const hotkeysChanged = settings.hotkeys != m_settings.hotkeys;
            if (hotkeysChanged)
            {
                auto const error = m_hotkeys.Initialize(m_host, settings.hotkeys);
                if (!error.empty())
                {
                    m_hotkeyError = m_hotkeys.Initialize(m_host, m_settings.hotkeys);
                    return error;
                }
                m_hotkeyError.clear();
            }
            auto updated = settings;
            updated.pinnedApplications = m_settings.pinnedApplications;
            updated.hiddenApplications = m_settings.hiddenApplications;
            if (settings.hiddenApplications.empty() && !saved.hiddenApplications.empty())
            {
                std::erase_if(updated.hiddenApplications, [&](auto const& id) {
                    return std::ranges::find(saved.hiddenApplications, id) != saved.hiddenApplications.end();
                });
            }
            // Settings and the mixer can both edit favorites. Preserve independent additions and removals.
            if (settings.favoriteDevices == saved.favoriteDevices)
            {
                updated.favoriteDevices = m_settings.favoriteDevices;
            }
            else
            {
                auto const contains = [](auto const& list, auto const& id) {
                    return std::ranges::find(list, id) != list.end();
                };
                std::erase_if(updated.favoriteDevices, [&](auto const& id) {
                    return contains(saved.favoriteDevices, id) && !contains(m_settings.favoriteDevices, id);
                });
                for (auto const& id : m_settings.favoriteDevices)
                {
                    if (!contains(saved.favoriteDevices, id) && !contains(updated.favoriteDevices, id))
                    {
                        updated.favoriteDevices.push_back(id);
                    }
                }
            }
            updated.popupSize = m_settings.popupSize;
            if (updated.popupSize && updated.faderHeight != m_settings.faderHeight)
            {
                updated.popupSize->cy = winrt::VolumeMixer::implementation::MainWindow::HeightForTracks(updated);
            }
            if (updated.rememberPopupPosition) updated.popupPosition = m_settings.popupPosition;
            auto const save = [&]
            {
                return m_settingsManager.Save(updated);
            };
            using SaveResult = StartupManager::SaveResult;
            auto const result = updated.launchAtStartup != m_settings.launchAtStartup
                ? m_startup.SaveWithChange(updated.launchAtStartup, save)
                : (save() ? SaveResult::Saved : SaveResult::SettingsSaveFailed);
            if (result != SaveResult::Saved)
            {
                if (hotkeysChanged)
                {
                    m_hotkeyError = m_hotkeys.Initialize(m_host, m_settings.hotkeys);
                }
                if (result == SaveResult::StartupChangeFailed) return L"Could not change launch at sign-in.";
                if (result == SaveResult::StartupRestoreFailed) return L"Could not save settings or restore the previous launch at sign-in entry.";
                return L"Could not save settings.";
            }
            m_settings = std::move(updated);
            m_audio.SetDevicePreferences(settings.deviceRoleMask, settings.hiddenDevices);
            ApplySettings();
            settings = m_settings;
            return {};
        });
    }
    m_settingsWindow->SetSettings(m_settings);
    m_settingsWindow->SetDevices(m_devices);
    m_settingsWindow->ShowError(m_hotkeyError);
    m_settingsWindow->Show();
}

void ApplicationController::ApplySettings()
{
    if (m_mixer)
    {
        m_mixer->ApplySettings(m_settings);
    }
    m_taskbar.SetEnabled(m_settings.integrationMode != IntegrationMode::TrayOnly);
    RefreshWidgetLayout(true);
}

void ApplicationController::UpdateIntegration(AudioSnapshot const& snapshot)
{
    auto const device = std::ranges::find(snapshot.devices, true, &AudioDeviceModel::isDefault);
    bool const headphones = device != snapshot.devices.end() && device->headphones;
    bool const identityChanged = !m_hasAudioState || m_deviceName != snapshot.deviceName || m_headphones != headphones;
    m_hasAudioState = true;
    if (identityChanged) m_deviceName = snapshot.deviceName;
    m_headphones = headphones;
    bool const masterChanged = UpdateMasterState(snapshot);
    if (m_devices != snapshot.devices)
    {
        m_devices = snapshot.devices;
        if (m_settingsWindow) m_settingsWindow->SetDevices(m_devices);
    }
    if (masterChanged || identityChanged) RefreshIntegration();
}

bool ApplicationController::UpdateMasterState(MasterVolumeState const& state)
{
    bool const changed = m_masterState.deviceAvailable != state.deviceAvailable
        || m_masterState.masterVolume != state.masterVolume || m_masterState.masterMuted != state.masterMuted;
    m_masterState = state;
    return changed;
}

void ApplicationController::ReadTaskbarTheme()
{
    DWORD light{};
    DWORD size = sizeof(light);
    m_lightTaskbar = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS && light != 0;
    m_taskbar.SetLightTheme(m_lightTaskbar);
}

void ApplicationController::RefreshWidgetLayout(bool refreshIntegration)
{
    bool const widgetVisible = m_taskbar.ApplyLayout();
    auto const now = GetTickCount64();
    if (widgetVisible) m_widgetMissingSince = 0;
    else if (m_widgetVisibleSince) m_widgetMissingSince = now;
    m_widgetVisibleSince = widgetVisible ? (m_widgetVisibleSince ? m_widgetVisibleSince : now) : 0;
    // Allow Explorer to settle briefly before switching an established widget to the tray.
    bool const widgetUnavailable = !widgetVisible
        && (m_trayWanted || !m_widgetMissingSince || now - m_widgetMissingSince >= WidgetLossGraceMs);
    bool const wantTray = m_settings.integrationMode != IntegrationMode::TaskbarOnly || widgetUnavailable
        || (widgetVisible && m_trayWanted && now - m_widgetVisibleSince < WidgetRecoveryGraceMs);
    if (wantTray == m_trayWanted)
    {
        if (refreshIntegration) RefreshIntegration();
        return;
    }
    m_trayWanted = wantTray;
    if (wantTray)
    {
        SetTimer(m_host, TrayIconManager::RetryTimer, 5000, nullptr);
    }
    else
    {
        KillTimer(m_host, TrayIconManager::RetryTimer);
        m_tray.Shutdown();
    }
    RefreshIntegration(wantTray);
}

void ApplicationController::RefreshIntegration(bool initializeTray)
{
    auto const tooltip = m_hasAudioState && m_masterState.deviceAvailable
        ? L"Volume Mixer — " + m_deviceName + L" — " + std::to_wstring(m_masterState.masterVolume) + (m_masterState.masterMuted ? L"% (Muted)" : L"%")
        : L"Volume Mixer — No playback device";
    if (m_trayWanted)
    {
        auto const size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(m_host));
        auto const trayIcon = m_iconRenderer.Get(m_smallIcon, m_masterState.masterVolume, m_masterState.masterMuted, size, m_lightTaskbar);
        if (initializeTray) m_tray.Initialize(m_host, trayIcon, tooltip);
        else m_tray.Update(trayIcon, tooltip);
    }
    m_taskbar.Update(m_masterState.deviceAvailable, m_masterState.masterVolume, m_masterState.masterMuted,
        !m_settings.alwaysShowSpeaker && m_headphones,
        m_settings.taskbarIndicator == TaskbarIndicator::VolumePercentage, m_settings.zeroWhenMuted, tooltip);
}

POINT ApplicationController::TrayAnchor() const
{
    RECT rect{};
    if (m_taskbar.TryGetRect(rect) || (m_trayWanted && m_tray.TryGetIconRect(rect)))
    {
        return { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
    }
    POINT point{};
    GetCursorPos(&point);
    return point;
}
