#pragma once

#include "SettingsWindow.g.h"
#include "Settings/SettingsModel.h"
#include "Audio/AudioModels.h"
#include <functional>

namespace winrt::VolumeMixer::implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow>
    {
        SettingsWindow();

        void SetSettings(::SettingsModel const& settings);
        void SetDevices(std::vector<::AudioDeviceModel> const& devices);
        void SetSaveHandler(std::function<std::wstring(::SettingsModel&, ::SettingsModel const&)> handler) { m_saveHandler = std::move(handler); }
        void ShowError(std::wstring const& error);
        void Show();
        bool CaptureRegisteredHotkey(UINT modifiers, UINT key);
        bool IsVisible() const { return m_appWindow.IsVisible(); }
        void Destroy();
        void Hide() { m_appWindow.Hide(); }
        void SaveButton_Click(IInspectable const&, RoutedEventArgs const&);
        void ApplyButton_Click(IInspectable const&, RoutedEventArgs const&);
        void CancelButton_Click(IInspectable const&, RoutedEventArgs const&);
        void RestoreHiddenButton_Click(IInspectable const&, RoutedEventArgs const&);
        void DeviceBox_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&);
        void FavoriteDevice_Click(IInspectable const&, RoutedEventArgs const&);
        void HiddenDevice_Click(IInspectable const&, RoutedEventArgs const&);
        void MoveDeviceUp_Click(IInspectable const&, RoutedEventArgs const&);
        void MoveDeviceDown_Click(IInspectable const&, RoutedEventArgs const&);
        void SettingsTabs_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&);
        void SettingsPage_Loaded(IInspectable const&, RoutedEventArgs const&);
        void TaskbarIndicator_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&);
        void MixerBackdropBox_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&);
        void GlassSlider_ValueChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const&);
        void GlassTintPicker_Loaded(IInspectable const&, RoutedEventArgs const&);
        void GlassTintPicker_ColorChanged(Controls::ColorPicker const&, Controls::ColorChangedEventArgs const& args);
        void ResetGlassTint_Click(IInspectable const&, RoutedEventArgs const&);

    private:
        bool Apply();
        winrt::hstring SelectedDevice();
        void UpdateDeviceControls();
        void MoveFavorite(int step);
        void QueueFitToTab();
        void MarkDirty();
        void UpdateTaskbarPreview();
        void UpdateGlassTintPicker();
        Microsoft::UI::Windowing::AppWindow m_appWindow{ nullptr };
        ::SettingsModel m_settings;
        ::SettingsModel m_savedSettings;
        std::function<std::wstring(::SettingsModel&, ::SettingsModel const&)> m_saveHandler;
        std::vector<Controls::TextBox> m_hotkeyFields;
        std::vector<::AudioDeviceModel> m_devices;
        Media::SolidColorBrush m_tintBrush;
        UINT m_dpi{};
        bool m_destroying{ false };
        bool m_loadingSettings{};
        bool m_fitQueued{};
        bool m_settingGlassTint{};
    };
}

namespace winrt::VolumeMixer::factory_implementation
{
    struct SettingsWindow : SettingsWindowT<SettingsWindow, implementation::SettingsWindow>
    {
    };
}
