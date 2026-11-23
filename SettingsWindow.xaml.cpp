#include "pch.h"
#include "SettingsWindow.xaml.h"
#include "AppResources.h"
#include "System/DynamicIconRenderer.h"
#include "System/HotkeyManager.h"
#include <cmath>
#include <dwmapi.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.Globalization.NumberFormatting.h>
#include <winrt/Windows.UI.Core.h>
#if __has_include("SettingsWindow.g.cpp")
#include "SettingsWindow.g.cpp"
#endif

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Windowing;

namespace winrt::VolumeMixer::implementation
{
    namespace
    {
        FrameworkElement FindPickerPart(DependencyObject const& parent, std::wstring_view name)
        {
            if (auto const element = parent.try_as<FrameworkElement>(); element && element.Name() == name)
            {
                return element;
            }
            for (int i = 0, count = Media::VisualTreeHelper::GetChildrenCount(parent); i < count; ++i)
            {
                if (auto const found = FindPickerPart(Media::VisualTreeHelper::GetChild(parent, i), name))
                {
                    return found;
                }
            }
            return nullptr;
        }
    }

    SettingsWindow::SettingsWindow()
    {
        InitializeComponent();
        for (auto const label : HotkeyLabels)
        {
            Controls::Grid row;
            row.ColumnSpacing(12);
            Controls::ColumnDefinition titleColumn;
            titleColumn.Width({1, GridUnitType::Star});
            Controls::ColumnDefinition shortcutColumn;
            shortcutColumn.Width({164, GridUnitType::Pixel});
            row.ColumnDefinitions().Append(titleColumn);
            row.ColumnDefinitions().Append(shortcutColumn);
            Controls::TextBlock name;
            name.Text(label);
            name.VerticalAlignment(VerticalAlignment::Center);
            Controls::TextBox box;
            Automation::AutomationProperties::SetName(box, label);
            box.IsReadOnly(true);
            box.PlaceholderText(L"Unassigned");
            Controls::Grid::SetColumn(box, 1);
            box.GotFocus([](IInspectable const& sender, RoutedEventArgs const&)
            {
                auto const field = sender.as<Controls::TextBox>();
                field.PlaceholderText(L"Press shortcut");
                field.SelectAll();
            });
            box.LostFocus([](IInspectable const& sender, RoutedEventArgs const&)
            {
                sender.as<Controls::TextBox>().PlaceholderText(L"Unassigned");
            });
            box.TextChanged([this](auto const&, auto const&) { MarkDirty(); });
            box.PreviewKeyDown([](IInspectable const& sender, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args)
            {
                using Windows::System::VirtualKey;
                using Windows::UI::Core::CoreVirtualKeyStates;
                auto const down = [](VirtualKey key)
                {
                    auto const state = Microsoft::UI::Input::InputKeyboardSource::GetKeyStateForCurrentThread(key);
                    return (state & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
                };
                UINT const modifiers = (down(VirtualKey::Control) ? MOD_CONTROL : 0)
                    | (down(VirtualKey::Menu) ? MOD_ALT : 0) | (down(VirtualKey::Shift) ? MOD_SHIFT : 0)
                    | ((down(VirtualKey::LeftWindows) || down(VirtualKey::RightWindows)) ? MOD_WIN : 0);
                auto const key = static_cast<UINT>(args.OriginalKey());
                auto const binding = ::HotkeyManager::FormatBinding(modifiers, key);
                if (!binding.empty() || (!modifiers && (key == VK_DELETE || key == VK_BACK)))
                {
                    sender.as<Controls::TextBox>().Text(binding);
                    args.Handled(true);
                }
            });
            row.Children().Append(name);
            row.Children().Append(box);
            HotkeyPanel().Children().Append(row);
            m_hotkeyFields.push_back(box);
        }
        VolumeMixer::SettingsWindow thisWindow = *this;
        auto windowNative = thisWindow.as<IWindowNative>();
        HWND hwnd{};
        check_hresult(windowNative->get_WindowHandle(&hwnd));
        m_appWindow = AppWindow::GetFromWindowId(GetWindowIdFromWindow(hwnd));
        auto const dpi = GetDpiForWindow(hwnd);
        m_dpi = dpi;
        auto const module = GetModuleHandleW(nullptr);
        auto const icon = MAKEINTRESOURCEW(IDI_VOLUME_MIXER);
        auto const smallIcon = LoadImageW(module, icon, IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED);
        auto const largeIcon = LoadImageW(module, icon, IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED);
        if (smallIcon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        if (largeIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
        auto const setTitleBarTheme = [hwnd](FrameworkElement const& surface)
        {
            BOOL const dark = surface.ActualTheme() == ElementTheme::Dark;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        };
        SettingsSurface().ActualThemeChanged([this, setTitleBarTheme](FrameworkElement const& surface, IInspectable const&)
        {
            setTitleBarTheme(surface);
            UpdateGlassTintPicker();
        });
        setTitleBarTheme(SettingsSurface());
        m_appWindow.Resize({MulDiv(512, dpi, 96), MulDiv(428, dpi, 96)});
        m_appWindow.Changed([this, hwnd](auto const&, auto const&)
        {
            auto const newDpi = GetDpiForWindow(hwnd);
            if (newDpi != m_dpi)
            {
                m_dpi = newDpi;
                QueueFitToTab();
            }
        });
        auto const changed = [this](auto const&, auto const&) { MarkDirty(); };
        for (auto const& toggle : {StartupToggle(), StartMinimizedToggle(), HideOnFocusToggle(), RememberPositionToggle(),
            PinnedTopToggle(), TaskbarWheelToggle(), MutedVolumeToggle()}) toggle.Toggled(changed);
        AlwaysSpeakerToggle().Toggled([this](auto const&, auto const&)
        {
            UpdateTaskbarPreview();
            MarkDirty();
        });
        for (auto const& check : {MetersToggle(), ChannelPercentagesToggle(), ConsoleRole(), MultimediaRole(), CommunicationsRole()}) check.Click(changed);
        for (auto const& box : {WheelStepBox(), ChannelWidthBox(), FaderHeightBox()})
        {
            box.RegisterPropertyChangedCallback(Controls::NumberBox::TextProperty(), changed);
            box.ValueChanged(changed);
        }
        ThemeComboBox().SelectionChanged(changed);
        IntegrationComboBox().SelectionChanged(changed);
        GlassTintSwatch().Fill(m_tintBrush);
        m_appWindow.Closing([this](Microsoft::UI::Windowing::AppWindow const&, AppWindowClosingEventArgs const& args)
        {
            if (!m_destroying)
            {
                args.Cancel(true);
                m_appWindow.Hide();
            }
        });
    }

    bool SettingsWindow::CaptureRegisteredHotkey(UINT modifiers, UINT key)
    {
        if (GetForegroundWindow() != Microsoft::UI::GetWindowFromWindowId(m_appWindow.Id())) return false;
        auto const focused = Microsoft::UI::Xaml::Input::FocusManager::GetFocusedElement(SettingsSurface().XamlRoot())
            .try_as<Controls::TextBox>();
        if (std::ranges::find(m_hotkeyFields, focused) == m_hotkeyFields.end()) return false;

        auto const binding = ::HotkeyManager::FormatBinding(modifiers, key);
        if (binding.empty()) return false;
        focused.Text(binding);
        return true;
    }

    void SettingsWindow::SetSettings(::SettingsModel const& settings)
    {
        m_loadingSettings = true;
        m_settings = settings;
        m_savedSettings = settings;
        SettingsSurface().RequestedTheme(settings.theme == AppTheme::Light ? ElementTheme::Light
            : settings.theme == AppTheme::Dark ? ElementTheme::Dark : ElementTheme::Default);
        ThemeComboBox().SelectedIndex(settings.theme == AppTheme::Light ? 1 : settings.theme == AppTheme::Dark ? 2 : 0);
        StartupToggle().IsOn(settings.launchAtStartup);
        for (size_t i = 0; i < m_hotkeyFields.size(); ++i)
        {
            m_hotkeyFields[i].Text(settings.hotkeys[i]);
        }
        IntegrationComboBox().SelectedIndex(static_cast<int>(settings.integrationMode));
        TaskbarIndicatorComboBox().SelectedIndex(static_cast<int>(settings.taskbarIndicator));
        MetersToggle().IsChecked(settings.showAudioActivityMeters);
        ChannelPercentagesToggle().IsChecked(settings.showChannelPercentages);
        TaskbarWheelToggle().IsOn(settings.taskbarWheelEnabled);
        AlwaysSpeakerToggle().IsOn(settings.alwaysShowSpeaker);
        UpdateTaskbarPreview();
        MutedVolumeToggle().IsOn(settings.zeroWhenMuted);
        StartMinimizedToggle().IsOn(settings.startMinimized);
        HideOnFocusToggle().IsOn(settings.hideOnFocusLoss);
        PinnedTopToggle().IsOn(settings.pinnedAlwaysOnTop);
        RememberPositionToggle().IsOn(settings.rememberPopupPosition);
        WheelStepBox().Value(settings.wheelStep);
        ChannelWidthBox().Value(settings.channelWidth);
        FaderHeightBox().Value(settings.faderHeight);
        GlassOpacityBox().Value(settings.glassOpacity);
        UpdateGlassTintPicker();
        GlassFrostBox().Value(settings.glassFrost);
        MixerBackdropBox().SelectedIndex(static_cast<int>(settings.mixerBackdrop));
        ConsoleRole().IsChecked((settings.deviceRoleMask & 1) != 0);
        MultimediaRole().IsChecked((settings.deviceRoleMask & 2) != 0);
        CommunicationsRole().IsChecked((settings.deviceRoleMask & 4) != 0);
        RestoreHiddenButton().Visibility(settings.hiddenApplications.empty() ? Visibility::Collapsed : Visibility::Visible);
        UpdateDeviceControls();
        m_loadingSettings = false;
        ApplyButton().IsEnabled(false);
    }

    void SettingsWindow::ShowError(std::wstring const& error)
    {
        HotkeyError().Text(error);
        HotkeyError().Visibility(error.empty() ? Visibility::Collapsed : Visibility::Visible);
        QueueFitToTab();
    }

    void SettingsWindow::Show()
    {
        m_appWindow.Show();
        Activate();
        QueueFitToTab();
    }

    void SettingsWindow::QueueFitToTab()
    {
        if (!m_appWindow || !m_appWindow.IsVisible() || m_fitQueued) return;
        m_fitQueued = true;
        DispatcherQueue().TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak()]
        {
            auto const self = weak.get();
            if (!self) return;
            self->m_fitQueued = false;
            if (self->m_destroying || !self->m_appWindow.IsVisible()) return;
            auto const tab = self->SettingsTabs().SelectedItem().try_as<Controls::TabViewItem>();
            if (!tab) return;
            auto const viewport = tab.Content().as<Controls::ScrollViewer>();
            auto const content = viewport.Content().as<FrameworkElement>();
            auto const surface = self->SettingsSurface();
            auto const hwnd = Microsoft::UI::GetWindowFromWindowId(self->m_appWindow.Id());
            RECT bounds{}, client{};
            MONITORINFO monitor{sizeof(monitor)};
            if (!viewport.XamlRoot() || viewport.ActualWidth() <= 0 || IsZoomed(hwnd)
                || !GetWindowRect(hwnd, &bounds) || !GetClientRect(hwnd, &client)
                || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;

            content.Measure({static_cast<float>(viewport.ActualWidth()), std::numeric_limits<float>::infinity()});
            auto const header = viewport.TransformToVisual(surface).TransformPoint({0, 0}).Y;
            // Leave one spacing step for rounding between XAML layout and the native window.
            auto const desired = header + content.DesiredSize().Height + self->SettingsFooter().ActualHeight() + 4;
            auto const frame = bounds.bottom - bounds.top - client.bottom;
            auto const& work = monitor.rcWork;
            int const width = std::min(bounds.right - bounds.left, work.right - work.left);
            int const height = std::min<int>(work.bottom - work.top,
                frame + static_cast<int>(std::ceil(desired * self->m_dpi / 96)));
            if (height <= frame) return;
            RECT const fitted{std::clamp(bounds.left, work.left, work.right - width),
                std::clamp(bounds.top, work.top, work.bottom - height), 0, 0};
            if (bounds.left != fitted.left || bounds.top != fitted.top || bounds.right - bounds.left != width
                || std::abs(bounds.bottom - bounds.top - height) > 1)
            {
                SetWindowPos(hwnd, nullptr, fitted.left, fitted.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        });
    }

    void SettingsWindow::SettingsTabs_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        QueueFitToTab();
    }

    void SettingsWindow::SettingsPage_Loaded(IInspectable const&, RoutedEventArgs const&)
    {
        QueueFitToTab();
    }

    void SettingsWindow::MarkDirty()
    {
        if (!m_loadingSettings && ApplyButton()) ApplyButton().IsEnabled(true);
    }

    void SettingsWindow::SetDevices(std::vector<::AudioDeviceModel> const& devices)
    {
        if (m_devices == devices)
        {
            return;
        }
        auto const selected = SelectedDevice();
        m_devices = devices;
        auto const items = DeviceBox().Items();
        items.Clear();
        int index = devices.empty() ? -1 : 0;
        for (size_t i = 0; i < devices.size(); ++i)
        {
            auto const& device = devices[i];
            items.Append(box_value(device.name + (device.isDefault ? L" (Default)" : L"")));
            if (device.id == selected.c_str())
            {
                index = static_cast<int>(i);
            }
        }
        DeviceBox().SelectedIndex(index);
        UpdateTaskbarPreview();
        UpdateDeviceControls();
    }

    hstring SettingsWindow::SelectedDevice()
    {
        auto const index = DeviceBox().SelectedIndex();
        return index >= 0 && static_cast<size_t>(index) < m_devices.size() ? hstring(m_devices[index].id) : hstring{};
    }

    void SettingsWindow::UpdateDeviceControls()
    {
        auto const id = SelectedDevice();
        auto const favorite = std::ranges::find(m_settings.favoriteDevices, id.c_str());
        auto const hidden = std::ranges::find(m_settings.hiddenDevices, id.c_str());
        bool const isFavorite = favorite != m_settings.favoriteDevices.end();
        FavoriteDevice().IsEnabled(!id.empty());
        HiddenDevice().IsEnabled(!id.empty());
        FavoriteDevice().IsChecked(isFavorite);
        HiddenDevice().IsChecked(hidden != m_settings.hiddenDevices.end());
        MoveDeviceUp().IsEnabled(isFavorite && favorite != m_settings.favoriteDevices.begin());
        MoveDeviceDown().IsEnabled(isFavorite && std::next(favorite) != m_settings.favoriteDevices.end());
    }

    void SettingsWindow::DeviceBox_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        UpdateDeviceControls();
    }

    void SettingsWindow::FavoriteDevice_Click(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring const id(SelectedDevice());
        if (!id.empty() && !std::erase(m_settings.favoriteDevices, id))
        {
            m_settings.favoriteDevices.push_back(id);
        }
        MarkDirty();
        UpdateDeviceControls();
    }

    void SettingsWindow::HiddenDevice_Click(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring const id(SelectedDevice());
        if (!id.empty() && !std::erase(m_settings.hiddenDevices, id))
        {
            m_settings.hiddenDevices.push_back(id);
        }
        MarkDirty();
        UpdateDeviceControls();
    }

    void SettingsWindow::MoveFavorite(int step)
    {
        auto const id = SelectedDevice();
        auto& favorites = m_settings.favoriteDevices;
        auto const favorite = std::ranges::find(favorites, id.c_str());
        auto const adjacent = favorite - favorites.begin() + step;
        if (favorite != favorites.end() && adjacent >= 0 && static_cast<size_t>(adjacent) < favorites.size())
        {
            std::iter_swap(favorite, favorites.begin() + adjacent);
            MarkDirty();
        }
        UpdateDeviceControls();
    }

    void SettingsWindow::MoveDeviceUp_Click(IInspectable const&, RoutedEventArgs const&)
    {
        MoveFavorite(-1);
    }

    void SettingsWindow::MoveDeviceDown_Click(IInspectable const&, RoutedEventArgs const&)
    {
        MoveFavorite(1);
    }

    void SettingsWindow::Destroy()
    {
        m_destroying = true;
        Close();
    }

    bool SettingsWindow::Apply()
    {
        if (!ApplyButton().IsEnabled()) return true;

        auto const roles = (ConsoleRole().IsChecked().Value() ? 1u : 0u)
            | (MultimediaRole().IsChecked().Value() ? 2u : 0u)
            | (CommunicationsRole().IsChecked().Value() ? 4u : 0u);
        if (!roles)
        {
            ShowError(L"Select at least one playback role.");
            return false;
        }
        m_settings.deviceRoleMask = roles;
        m_settings.theme = ThemeComboBox().SelectedIndex() == 1 ? AppTheme::Light : ThemeComboBox().SelectedIndex() == 2 ? AppTheme::Dark : AppTheme::System;
        m_settings.launchAtStartup = StartupToggle().IsOn();
        for (size_t i = 0; i < m_hotkeyFields.size(); ++i)
        {
            m_settings.hotkeys[i] = m_hotkeyFields[i].Text();
        }
        m_settings.integrationMode = static_cast<IntegrationMode>(IntegrationComboBox().SelectedIndex());
        m_settings.taskbarIndicator = static_cast<TaskbarIndicator>(TaskbarIndicatorComboBox().SelectedIndex());
        m_settings.showAudioActivityMeters = MetersToggle().IsChecked().Value();
        m_settings.showChannelPercentages = ChannelPercentagesToggle().IsChecked().Value();
        m_settings.taskbarWheelEnabled = TaskbarWheelToggle().IsOn();
        m_settings.alwaysShowSpeaker = AlwaysSpeakerToggle().IsOn();
        m_settings.zeroWhenMuted = MutedVolumeToggle().IsOn();
        m_settings.startMinimized = StartMinimizedToggle().IsOn();
        m_settings.hideOnFocusLoss = HideOnFocusToggle().IsOn();
        m_settings.pinnedAlwaysOnTop = PinnedTopToggle().IsOn();
        m_settings.rememberPopupPosition = RememberPositionToggle().IsOn();
        if (!m_settings.rememberPopupPosition)
        {
            m_settings.popupPosition.reset();
        }
        auto const setNumber = [](int& setting, Controls::NumberBox const& box, int step = 1)
        {
            auto const parser = box.NumberFormatter().try_as<Windows::Globalization::NumberFormatting::INumberParser>();
            auto const parsed = parser ? parser.ParseDouble(box.Text()) : nullptr;
            auto const value = parsed ? parsed.Value() : box.Value();
            if (std::isfinite(value))
            {
                setting = static_cast<int>(std::lround(std::clamp(value, box.Minimum(), box.Maximum()) / step)) * step;
            }
        };
        setNumber(m_settings.wheelStep, WheelStepBox());
        setNumber(m_settings.channelWidth, ChannelWidthBox(), 4);
        setNumber(m_settings.faderHeight, FaderHeightBox(), 4);
        m_settings.glassOpacity = static_cast<int>(std::lround(GlassOpacityBox().Value()));
        m_settings.glassFrost = static_cast<int>(std::lround(GlassFrostBox().Value()));
        m_settings.mixerBackdrop = static_cast<MixerBackdrop>(MixerBackdropBox().SelectedIndex());
        if (m_saveHandler)
        {
            auto const error = m_saveHandler(m_settings, m_savedSettings);
            ShowError(error);
            if (!error.empty())
            {
                return false;
            }
        }
        SetSettings(m_settings);
        return true;
    }

    void SettingsWindow::SaveButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        if (Apply()) m_appWindow.Hide();
    }

    void SettingsWindow::ApplyButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        Apply();
    }

    void SettingsWindow::CancelButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_appWindow.Hide();
    }

    void SettingsWindow::RestoreHiddenButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.hiddenApplications.clear();
        RestoreHiddenButton().Visibility(Visibility::Collapsed);
        MarkDirty();
        QueueFitToTab();
    }

    void SettingsWindow::MixerBackdropBox_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        if (auto const frost = FrostRow())
        {
            auto const visibility = MixerBackdropBox().SelectedIndex() == static_cast<int>(MixerBackdrop::LiquidGlass)
                ? Visibility::Visible : Visibility::Collapsed;
            frost.Visibility(visibility);
            if (auto const note = GlassCaptureNote()) note.Visibility(visibility);
        }
        MarkDirty();
        QueueFitToTab();
    }

    void SettingsWindow::TaskbarIndicator_SelectionChanged(IInspectable const&, Controls::SelectionChangedEventArgs const&)
    {
        UpdateTaskbarPreview();
        MarkDirty();
    }

    void SettingsWindow::UpdateTaskbarPreview()
    {
        auto const icon = TaskbarSpeakerPreview();
        auto const value = TaskbarNumberPreview();
        auto const speaker = AlwaysSpeakerToggle();
        if (!icon || !value || !speaker) return;

        bool const number = TaskbarIndicatorComboBox().SelectedIndex() == static_cast<int>(TaskbarIndicator::VolumePercentage);
        speaker.IsEnabled(!number);
        auto const device = std::ranges::find(m_devices, true, &::AudioDeviceModel::isDefault);
        bool const headphones = !speaker.IsOn() && device != m_devices.end() && device->headphones;
        icon.Glyph(std::wstring(1, headphones ? L'\uE7F6' : ::DynamicIconRenderer::SpeakerGlyph(100, false)));
        icon.Visibility(number ? Visibility::Collapsed : Visibility::Visible);
        value.Visibility(number ? Visibility::Visible : Visibility::Collapsed);
    }

    void SettingsWindow::GlassSlider_ValueChanged(IInspectable const& sender, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
    {
        auto const label = sender == GlassOpacityBox() ? OpacityValue() : FrostValue();
        if (label) label.Text(std::to_wstring(static_cast<int>(std::lround(args.NewValue()))) + L"%");
        MarkDirty();
    }

    void SettingsWindow::GlassTintPicker_Loaded(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto const picker = sender.as<Controls::ColorPicker>();
        picker.ApplyTemplate();
        auto const root = FindPickerPart(picker, L"RootGrid").try_as<Controls::Grid>();
        auto const spectrum = FindPickerPart(picker, L"ColorSpectrum").try_as<Controls::Primitives::ColorSpectrum>();
        auto const spectrumGrid = FindPickerPart(picker, L"ColorSpectrumGrid").try_as<Controls::Grid>();
        auto const entry = FindPickerPart(picker, L"TextEntryGrid").try_as<Controls::Grid>();
        auto const model = FindPickerPart(picker, L"ColorRepresentationComboBox").try_as<Controls::ComboBox>();
        auto const hex = FindPickerPart(picker, L"HexTextBox").try_as<Controls::TextBox>();
        auto const rgb = FindPickerPart(picker, L"RgbPanel").try_as<Controls::Grid>();
        auto const hsv = FindPickerPart(picker, L"HsvPanel").try_as<Controls::Grid>();
        constexpr double spectrumWidth = 272;
        constexpr double spectrumHeight = 152;
        if (!root || !spectrum || !spectrumGrid || !entry || !model || !hex || !rgb || !hsv
            || spectrum.Height() == spectrumHeight || entry.ColumnDefinitions().Size() != 3
            || entry.RowDefinitions().Size() < 2 || model.Parent() != entry || hex.Parent() != entry) return;

        struct Channel
        {
            std::wstring_view name;
            Controls::TextBox box{nullptr};
            Controls::TextBlock caption{nullptr};
        };
        auto const findChannels = [](Controls::Grid const& panel, std::array<Channel, 3>& channels)
        {
            for (auto& channel : channels)
            {
                channel.box = FindPickerPart(panel, std::wstring(channel.name) + L"TextBox").try_as<Controls::TextBox>();
                channel.caption = FindPickerPart(panel, std::wstring(channel.name) + L"Label").try_as<Controls::TextBlock>();
                if (!channel.box || !channel.caption || channel.box.Parent() != panel || channel.caption.Parent() != panel)
                {
                    return false;
                }
            }
            return true;
        };
        std::array rgbChannels{Channel{L"Red"}, Channel{L"Green"}, Channel{L"Blue"}};
        std::array hsvChannels{Channel{L"Hue"}, Channel{L"Saturation"}, Channel{L"Value"}};
        if (!findChannels(rgb, rgbChannels) || !findChannels(hsv, hsvChannels)) return;

        // Reflow the native editor parts so WinUI continues to own color conversion and input validation.
        root.MinWidth(0);
        root.MaxWidth(272);
        root.Padding(Thickness{0});
        spectrum.MinWidth(0);
        spectrum.MinHeight(0);
        spectrumGrid.Width(spectrumWidth);
        spectrum.Width(spectrumHeight);
        spectrum.Height(spectrumHeight);
        spectrum.HorizontalAlignment(HorizontalAlignment::Center);
        Media::ScaleTransform widen;
        widen.ScaleX(spectrumWidth / spectrumHeight);
        spectrum.RenderTransformOrigin({0.5f, 0.5f});
        spectrum.RenderTransform(widen);
        if (auto const marker = FindPickerPart(spectrum, L"SelectionEllipsePanel"))
        {
            Media::ScaleTransform restoreCircle;
            restoreCircle.ScaleX(spectrumHeight / spectrumWidth);
            marker.RenderTransformOrigin({0.5f, 0.5f});
            marker.RenderTransform(restoreCircle);
        }
        spectrumGrid.Margin(Thickness{0, 0, 0, 8});
        model.Width(84);
        entry.ColumnDefinitions().GetAt(0).Width({84, GridUnitType::Pixel});
        entry.ColumnDefinitions().GetAt(1).Width({8, GridUnitType::Pixel});
        entry.ColumnDefinitions().GetAt(2).Width({1, GridUnitType::Star});
        hex.Width(std::numeric_limits<double>::quiet_NaN());
        hex.HorizontalAlignment(HorizontalAlignment::Stretch);
        hex.Margin(Thickness{28, 0, 0, 0});
        Automation::AutomationProperties::SetName(hex, L"HEX color");
        Shapes::Rectangle swatch;
        swatch.Width(20);
        swatch.Height(20);
        swatch.RadiusX(4);
        swatch.RadiusY(4);
        swatch.Fill(m_tintBrush);
        swatch.Stroke(GlassTintSwatch().Stroke());
        swatch.StrokeThickness(1);
        swatch.HorizontalAlignment(HorizontalAlignment::Left);
        swatch.VerticalAlignment(VerticalAlignment::Center);
        Controls::Grid::SetColumn(swatch, 2);
        entry.Children().Append(swatch);

        auto const layoutChannels = [](Controls::Grid const& panel, std::array<Channel, 3> const& channels)
        {
            panel.RowDefinitions().Clear();
            panel.ColumnDefinitions().Clear();
            panel.ColumnSpacing(4);
            panel.Margin(Thickness{0, 8, 0, 0});
            for (int i = 0; i < 3; ++i)
            {
                Controls::ColumnDefinition captionColumn;
                captionColumn.Width({16, GridUnitType::Pixel});
                panel.ColumnDefinitions().Append(captionColumn);
                Controls::ColumnDefinition inputColumn;
                panel.ColumnDefinitions().Append(inputColumn);
                auto const& [name, box, caption] = channels[i];
                Automation::AutomationProperties::SetName(box, hstring(name));
                box.Width(std::numeric_limits<double>::quiet_NaN());
                box.MinWidth(0);
                box.Margin(Thickness{0, 0, i < 2 ? 4.0 : 0.0, 0});
                caption.Text(hstring(name.substr(0, 1)));
                caption.Margin(Thickness{0});
                Controls::Grid::SetRow(caption, 0);
                Controls::Grid::SetColumn(caption, i * 2);
                Controls::Grid::SetRow(box, 0);
                Controls::Grid::SetColumn(box, i * 2 + 1);
            }
        };
        layoutChannels(rgb, rgbChannels);
        layoutChannels(hsv, hsvChannels);
    }

    void SettingsWindow::UpdateGlassTintPicker()
    {
        auto const tint = m_settings.GlassTint(SettingsSurface().ActualTheme() == ElementTheme::Light);
        Windows::UI::Color const color{255, GetRValue(tint), GetGValue(tint), GetBValue(tint)};
        m_settingGlassTint = true;
        GlassTintPicker().Color(color);
        m_settingGlassTint = false;
        m_tintBrush.Color(color);
    }

    void SettingsWindow::GlassTintPicker_ColorChanged(Controls::ColorPicker const&, Controls::ColorChangedEventArgs const& args)
    {
        if (m_settingGlassTint || !GlassTintSwatch()) return;
        auto const color = args.NewColor();
        m_settings.glassTint = RGB(color.R, color.G, color.B);
        m_tintBrush.Color(color);
        MarkDirty();
    }

    void SettingsWindow::ResetGlassTint_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_settings.glassTint.reset();
        UpdateGlassTintPicker();
        MarkDirty();
        GlassTintFlyout().Hide();
    }
}

