#include "pch.h"
#include "StartupManager.h"
#include <wil/resource.h>

StartupManager::SaveResult StartupManager::SaveWithChange(bool enabled, std::function<bool()> const& save) const
{
    constexpr wchar_t Name[] = L"VolumeMixer";
    wil::unique_hkey key;
    auto result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
        nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, key.put(), nullptr);
    if (result != ERROR_SUCCESS) return SaveResult::StartupChangeFailed;

    DWORD previousType{}, previousSize{};
    result = RegQueryValueExW(key.get(), Name, nullptr, &previousType, nullptr, &previousSize);
    bool const hadValue = result == ERROR_SUCCESS;
    if (!hadValue && result != ERROR_FILE_NOT_FOUND) return SaveResult::StartupChangeFailed;
    std::vector<BYTE> previousValue(previousSize);
    if (hadValue && RegQueryValueExW(key.get(), Name, nullptr, &previousType, previousValue.data(), &previousSize) != ERROR_SUCCESS)
    {
        return SaveResult::StartupChangeFailed;
    }

    auto const remove = [&]
    {
        auto const status = RegDeleteValueW(key.get(), Name);
        return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
    };
    if (enabled)
    {
        std::wstring executable(32768, L'\0');
        auto const length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length == executable.size()) return SaveResult::StartupChangeFailed;
        executable.resize(length);
        auto const value = L'"' + executable + L'"';
        result = RegSetValueExW(key.get(), Name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        result = remove();
    }
    if (result != ERROR_SUCCESS) return SaveResult::StartupChangeFailed;
    if (save()) return SaveResult::Saved;

    auto const restored = hadValue
        ? RegSetValueExW(key.get(), Name, 0, previousType, previousValue.data(), previousSize) : remove();
    return restored == ERROR_SUCCESS ? SaveResult::SettingsSaveFailed : SaveResult::StartupRestoreFailed;
}
