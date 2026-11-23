#pragma once
#include "Settings/SettingsModel.h"

class HotkeyManager final
{
public:
    ~HotkeyManager();
    static std::wstring FormatBinding(UINT modifiers, UINT key);
    std::wstring Initialize(HWND target, HotkeyBindings const& bindings);
    void Shutdown();

private:
    HWND m_target{ nullptr };
    std::array<bool, HotkeyLabels.size()> m_registered{};
};
