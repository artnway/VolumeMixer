#include "pch.h"
#include "SettingsManager.h"
#include <shlobj.h>
#include <cmath>
#include <fstream>
#include <winrt/Windows.Data.Json.h>

using winrt::Windows::Data::Json::JsonObject;

namespace
{
    using namespace winrt::Windows::Data::Json;

    IJsonValue ReadValue(JsonObject const& object, winrt::hstring const& name, JsonValueType type)
    {
        auto const value = object.GetNamedValue(name, nullptr);
        return value && value.ValueType() == type ? value : nullptr;
    }

    double ReadNumber(JsonObject const& object, winrt::hstring const& name,
        double fallback = std::numeric_limits<double>::quiet_NaN())
    {
        auto const value = ReadValue(object, name, JsonValueType::Number);
        auto const number = value ? value.GetNumber() : fallback;
        return std::isfinite(number) ? number : fallback;
    }

    bool ReadBoolean(JsonObject const& object, winrt::hstring const& name, bool fallback)
    {
        auto const value = ReadValue(object, name, JsonValueType::Boolean);
        return value ? value.GetBoolean() : fallback;
    }

    winrt::hstring ReadString(JsonObject const& object, winrt::hstring const& name, winrt::hstring const& fallback = {})
    {
        auto const value = ReadValue(object, name, JsonValueType::String);
        return value ? value.GetString() : fallback;
    }

    JsonArray ReadArray(JsonObject const& object, winrt::hstring const& name)
    {
        auto const value = ReadValue(object, name, JsonValueType::Array);
        return value ? value.GetArray() : nullptr;
    }

    JsonObject ReadObject(JsonObject const& object, winrt::hstring const& name)
    {
        auto const value = ReadValue(object, name, JsonValueType::Object);
        return value ? value.GetObject() : nullptr;
    }
}

std::filesystem::path SettingsManager::FilePath() const
{
    PWSTR localAppData{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)))
    {
        return {};
    }
    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    return path / L"VolumeMixer" / L"settings.json";
}

SettingsModel SettingsManager::Load() const
{
    SettingsModel settings;
    auto const path = FilePath();
    if (path.empty())
    {
        return settings;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return settings;
    }
    std::string const text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    try
    {
        auto const json = JsonObject::Parse(winrt::to_hstring(text));
        auto const theme = ReadString(json, L"theme", L"System");
        settings.theme = theme == L"Light" ? AppTheme::Light : theme == L"Dark" ? AppTheme::Dark : AppTheme::System;
        settings.launchAtStartup = ReadBoolean(json, L"launchAtStartup", false);
        auto const legacyHotkey = ReadString(json, L"switchHotkey", L"Ctrl+Alt+V");
        settings.hotkeys[0] = legacyHotkey == L"Disabled" ? L"" : legacyHotkey.c_str();
        if (auto const hotkeys = ReadArray(json, L"hotkeys"))
        {
            for (uint32_t i = 0; i < hotkeys.Size() && i < settings.hotkeys.size(); ++i)
            {
                auto const binding = hotkeys.GetAt(i);
                if (binding.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                {
                    settings.hotkeys[i] = binding.GetString();
                }
            }
        }
        auto const integration = ReadString(json, L"integrationMode", L"TaskbarAndTray");
        settings.integrationMode = integration == L"TrayOnly" ? IntegrationMode::TrayOnly : integration == L"TaskbarOnly" ? IntegrationMode::TaskbarOnly : IntegrationMode::TaskbarAndTray;
        auto const taskbar = ReadString(json, L"taskbarIndicator", L"VolumePercentage");
        settings.taskbarIndicator = taskbar == L"IconOnly" ? TaskbarIndicator::IconOnly : TaskbarIndicator::VolumePercentage;
        settings.showAudioActivityMeters = ReadBoolean(json, L"showAudioActivityMeters", true);
        settings.showChannelPercentages = ReadBoolean(json, L"showChannelPercentages", false);
        settings.taskbarWheelEnabled = ReadBoolean(json, L"taskbarWheelEnabled", true);
        settings.alwaysShowSpeaker = ReadBoolean(json, L"alwaysShowSpeaker", ReadBoolean(json, L"alwaysShowHeadphones", false));
        settings.zeroWhenMuted = ReadBoolean(json, L"zeroWhenMuted", false);
        settings.startMinimized = ReadBoolean(json, L"startMinimized", false);
        settings.hideOnFocusLoss = ReadBoolean(json, L"hideOnFocusLoss", true);
        settings.pinnedAlwaysOnTop = ReadBoolean(json, L"pinnedAlwaysOnTop", false);
        settings.rememberPopupPosition = ReadBoolean(json, L"rememberPopupPosition", true);
        if (auto const position = ReadObject(json, L"popupPosition"); settings.rememberPopupPosition && position)
        {
            auto const x = ReadNumber(position, L"x");
            auto const y = ReadNumber(position, L"y");
            if (std::isfinite(x) && std::isfinite(y))
            {
                settings.popupPosition = POINT{static_cast<LONG>(std::clamp(x, -100000., 100000.)),
                    static_cast<LONG>(std::clamp(y, -100000., 100000.))};
            }
        }
        if (auto const size = ReadObject(json, L"popupSize"))
        {
            auto const width = ReadNumber(size, L"width");
            auto const height = ReadNumber(size, L"height");
            if (std::isfinite(width) && std::isfinite(height))
            {
                settings.popupSize = SIZE{static_cast<LONG>(std::clamp(width, 88., 8192.)),
                    static_cast<LONG>(std::clamp(height, 208., 8192.))};
            }
        }
        settings.wheelStep = static_cast<int>(std::clamp(ReadNumber(json, L"wheelStep", 2), 1.0, 20.0));
        settings.channelWidth = (static_cast<int>(std::clamp(ReadNumber(json, L"channelWidth", 68), 56.0, 72.0)) + 2) / 4 * 4;
        settings.faderHeight = (static_cast<int>(std::clamp(ReadNumber(json, L"faderHeight", 144), 128.0, 160.0)) + 2) / 4 * 4;
        settings.glassOpacity = static_cast<int>(std::clamp(ReadNumber(json, L"glassOpacity", 46), 0.0, 100.0));
        auto const legacyFrost = std::clamp(ReadNumber(json, L"glassFrost", 20), 0.0, 25.0);
        settings.glassFrost = static_cast<int>(std::clamp(ReadNumber(json, L"glassFrostPercent", legacyFrost * 4), 0.0, 100.0));
        auto const tint = ReadNumber(json, L"glassTint", -1);
        if (tint >= 0 && tint <= 0xffffff)
        {
            settings.glassTint = static_cast<COLORREF>(tint);
        }
        auto const legacyBackdrop = ReadBoolean(json, L"liveAcrylic", false) ? L"Acrylic" : L"Mica";
        auto const backdrop = ReadString(json, L"mixerBackdrop", legacyBackdrop);
        settings.mixerBackdrop = backdrop == L"LiquidGlass" ? MixerBackdrop::LiquidGlass
            : backdrop == L"ClearGlass" ? MixerBackdrop::ClearGlass
            : backdrop == L"Acrylic" ? MixerBackdrop::Acrylic : MixerBackdrop::Mica;
        settings.deviceRoleMask = static_cast<unsigned>(std::clamp(ReadNumber(json, L"deviceRoleMask", 7), 1.0, 7.0));
        if (auto const favorites = ReadArray(json, L"favoriteDevices"))
        {
            for (auto const& value : favorites)
            {
                if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) continue;
                std::wstring id(value.GetString());
                if (!id.empty() && std::find(settings.favoriteDevices.begin(), settings.favoriteDevices.end(), id) == settings.favoriteDevices.end())
                {
                    settings.favoriteDevices.push_back(std::move(id));
                }
            }
        }
        if (auto const hiddenDevices = ReadArray(json, L"hiddenDevices"))
        {
            for (auto const& value : hiddenDevices)
            {
                if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                {
                    settings.hiddenDevices.emplace_back(value.GetString());
                }
            }
        }
        auto const pinned = ReadArray(json, L"pinnedApplications");
        if (pinned)
        {
            for (auto const& value : pinned)
            {
                if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) continue;
                auto const entry = value.GetObject();
                PinnedApplication application;
                application.id = ReadString(entry, L"id").c_str();
                application.displayName = ReadString(entry, L"displayName", L"Application").c_str();
                application.executablePath = ReadString(entry, L"executablePath", L"").c_str();
                application.order = static_cast<int>(std::clamp(ReadNumber(entry, L"order", 0),
                    static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max())));
                if (!application.id.empty())
                {
                    settings.pinnedApplications.push_back(std::move(application));
                }
            }
        }
        auto const hidden = ReadArray(json, L"hiddenApplications");
        if (hidden)
        {
            for (auto const& value : hidden)
            {
                if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
                {
                    settings.hiddenApplications.emplace_back(value.GetString());
                }
            }
        }
    }
    catch (...)
    {
        return SettingsModel{};
    }
    return settings;
}

bool SettingsManager::Save(SettingsModel const& settings) const try
{
    auto const path = FilePath();
    if (path.empty())
    {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        return false;
    }

    JsonObject json;
    json.SetNamedValue(L"theme", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
        settings.theme == AppTheme::Light ? L"Light" : settings.theme == AppTheme::Dark ? L"Dark" : L"System"));
    json.SetNamedValue(L"launchAtStartup", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.launchAtStartup));
    winrt::Windows::Data::Json::JsonArray hotkeys;
    for (auto const& hotkey : settings.hotkeys)
    {
        hotkeys.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(hotkey));
    }
    json.SetNamedValue(L"hotkeys", hotkeys);
    json.SetNamedValue(L"integrationMode", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
        settings.integrationMode == IntegrationMode::TrayOnly ? L"TrayOnly" : settings.integrationMode == IntegrationMode::TaskbarOnly ? L"TaskbarOnly" : L"TaskbarAndTray"));
    json.SetNamedValue(L"taskbarIndicator", winrt::Windows::Data::Json::JsonValue::CreateStringValue(
        settings.taskbarIndicator == TaskbarIndicator::IconOnly ? L"IconOnly" : L"VolumePercentage"));
    json.SetNamedValue(L"showAudioActivityMeters", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.showAudioActivityMeters));
    json.SetNamedValue(L"showChannelPercentages", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.showChannelPercentages));
    json.SetNamedValue(L"taskbarWheelEnabled", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.taskbarWheelEnabled));
    json.SetNamedValue(L"alwaysShowSpeaker", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.alwaysShowSpeaker));
    json.SetNamedValue(L"zeroWhenMuted", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.zeroWhenMuted));
    json.SetNamedValue(L"startMinimized", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.startMinimized));
    json.SetNamedValue(L"hideOnFocusLoss", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.hideOnFocusLoss));
    json.SetNamedValue(L"pinnedAlwaysOnTop", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.pinnedAlwaysOnTop));
    json.SetNamedValue(L"rememberPopupPosition", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(settings.rememberPopupPosition));
    if (settings.rememberPopupPosition && settings.popupPosition)
    {
        JsonObject position;
        position.SetNamedValue(L"x", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.popupPosition->x));
        position.SetNamedValue(L"y", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.popupPosition->y));
        json.SetNamedValue(L"popupPosition", position);
    }
    if (settings.popupSize)
    {
        JsonObject size;
        size.SetNamedValue(L"width", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.popupSize->cx));
        size.SetNamedValue(L"height", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.popupSize->cy));
        json.SetNamedValue(L"popupSize", size);
    }
    json.SetNamedValue(L"wheelStep", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.wheelStep));
    json.SetNamedValue(L"channelWidth", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.channelWidth));
    json.SetNamedValue(L"faderHeight", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.faderHeight));
    json.SetNamedValue(L"glassOpacity", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.glassOpacity));
    json.SetNamedValue(L"glassFrostPercent", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.glassFrost));
    if (settings.glassTint)
    {
        json.SetNamedValue(L"glassTint", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(*settings.glassTint));
    }
    auto const backdrop = settings.mixerBackdrop == MixerBackdrop::LiquidGlass ? L"LiquidGlass"
        : settings.mixerBackdrop == MixerBackdrop::ClearGlass ? L"ClearGlass"
        : settings.mixerBackdrop == MixerBackdrop::Acrylic ? L"Acrylic" : L"Mica";
    json.SetNamedValue(L"mixerBackdrop", winrt::Windows::Data::Json::JsonValue::CreateStringValue(backdrop));
    json.SetNamedValue(L"deviceRoleMask", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(settings.deviceRoleMask));
    winrt::Windows::Data::Json::JsonArray favorites;
    for (auto const& id : settings.favoriteDevices)
    {
        favorites.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(id));
    }
    json.SetNamedValue(L"favoriteDevices", favorites);
    winrt::Windows::Data::Json::JsonArray hiddenDevices;
    for (auto const& id : settings.hiddenDevices)
    {
        hiddenDevices.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(id));
    }
    json.SetNamedValue(L"hiddenDevices", hiddenDevices);
    winrt::Windows::Data::Json::JsonArray pinned;
    for (auto const& application : settings.pinnedApplications)
    {
        JsonObject entry;
        entry.SetNamedValue(L"id", winrt::Windows::Data::Json::JsonValue::CreateStringValue(application.id));
        entry.SetNamedValue(L"displayName", winrt::Windows::Data::Json::JsonValue::CreateStringValue(application.displayName));
        entry.SetNamedValue(L"executablePath", winrt::Windows::Data::Json::JsonValue::CreateStringValue(application.executablePath));
        entry.SetNamedValue(L"order", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(application.order));
        pinned.Append(entry);
    }
    json.SetNamedValue(L"pinnedApplications", pinned);
    winrt::Windows::Data::Json::JsonArray hidden;
    for (auto const& application : settings.hiddenApplications)
    {
        hidden.Append(winrt::Windows::Data::Json::JsonValue::CreateStringValue(application));
    }
    json.SetNamedValue(L"hiddenApplications", hidden);

    auto const text = winrt::to_string(json.Stringify());
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
        if (!stream)
        {
            DeleteFileW(temporary.c_str());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
catch (...)
{
    return false;
}
