#pragma once

#include "MainWindow.xaml.h"
#include "SettingsWindow.xaml.h"
#include "Settings/SettingsManager.h"
#include "System/StartupManager.h"
#include "System/TrayIconManager.h"
#include "System/TaskbarWidgetManager.h"
#include "System/HotkeyManager.h"
#include "System/DynamicIconRenderer.h"
#include "Audio/AudioStateService.h"

class ApplicationController final
{
public:
    ~ApplicationController();
    bool Initialize();
    void Exit();

private:
    static LRESULT CALLBACK HostWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void FinishExit();
    void ToggleMixer(POINT anchor);
    void ShowMixer(POINT anchor);
    void ShowTrayMenu(POINT anchor);
    void ShowSettings();
    void ApplySettings();
    void UpdateIntegration(AudioSnapshot const& snapshot);
    bool UpdateMasterState(MasterVolumeState const& state);
    void ReadTaskbarTheme();
    void RefreshIntegration(bool initializeTray = false);
    void RefreshWidgetLayout(bool refreshIntegration = false);
    POINT TrayAnchor() const;

    static constexpr UINT ShowMixerMessage = WM_APP + 2;
    static constexpr UINT AudioStateMessage = WM_APP + 3;
    static constexpr UINT_PTR ExitTimer = 102;
    static constexpr UINT MenuOpen = 1001;
    static constexpr UINT MenuExit = 1002;
    static constexpr UINT MenuSettings = 1003;
    static constexpr UINT MenuMute = 1004;
    static constexpr UINT MenuDevice = 1100;

    HWND m_host{ nullptr };
    HANDLE m_singleInstance{ nullptr };
    UINT m_taskbarCreatedMessage{ 0 };
    bool m_exiting{ false };
    HICON m_largeIcon{ nullptr };
    HICON m_smallIcon{ nullptr };
    bool m_hasAudioState{ false };
    bool m_headphones{};
    bool m_trayWanted{ false };
    bool m_lightTaskbar{};
    ULONGLONG m_widgetVisibleSince{};
    std::wstring m_deviceName;
    std::wstring m_hotkeyError;
    std::vector<AudioDeviceModel> m_devices;
    winrt::com_ptr<winrt::VolumeMixer::implementation::MainWindow> m_mixer;
    winrt::com_ptr<winrt::VolumeMixer::implementation::SettingsWindow> m_settingsWindow;
    SettingsManager m_settingsManager;
    SettingsModel m_settings;
    StartupManager m_startup;
    TrayIconManager m_tray;
    TaskbarWidgetManager m_taskbar;
    HotkeyManager m_hotkeys;
    DynamicIconRenderer m_iconRenderer;
    AudioStateService m_audio;
    AudioPeakSnapshot m_peaks;
    MasterVolumeState m_masterState;
    std::unique_ptr<AudioSnapshot> m_pendingMixerSnapshot;
};
