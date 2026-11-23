#include "pch.h"
#include "HotkeyManager.h"
#include <cwctype>
#include <sstream>

namespace
{
    std::wstring KeyName(UINT key)
    {
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        {
            return std::wstring(1, static_cast<wchar_t>(key));
        }
        if (key >= VK_F1 && key <= VK_F24)
        {
            return L"F" + std::to_wstring(key - VK_F1 + 1);
        }
        return {};
    }

    bool ParseBinding(std::wstring const& text, UINT& modifiers, UINT& key)
    {
        std::wistringstream tokens(text);
        std::wstring token;
        while (std::getline(tokens, token, L'+'))
        {
            std::erase_if(token, [](wchar_t value) { return std::iswspace(value); });
            std::ranges::transform(token, token.begin(), [](wchar_t value) { return std::towupper(value); });
            if (key)
            {
                return false;
            }
            if (token == L"CTRL") modifiers |= MOD_CONTROL;
            else if (token == L"ALT") modifiers |= MOD_ALT;
            else if (token == L"SHIFT") modifiers |= MOD_SHIFT;
            else if (token == L"WIN") modifiers |= MOD_WIN;
            else if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') || (token[0] >= L'0' && token[0] <= L'9')))
            {
                key = token[0];
            }
            else
            {
                for (UINT number = 1; number <= 24; ++number)
                {
                    if (token == KeyName(VK_F1 + number - 1))
                    {
                        key = VK_F1 + number - 1;
                        break;
                    }
                }
                if (!key)
                {
                    return false;
                }
            }
        }
        return modifiers && key && text.back() != L'+';
    }
}

std::wstring HotkeyManager::FormatBinding(UINT modifiers, UINT key)
{
    auto const name = KeyName(key);
    if (!modifiers || name.empty()) return {};

    std::wstring binding;
    if (modifiers & MOD_CONTROL) binding += L"Ctrl+";
    if (modifiers & MOD_ALT) binding += L"Alt+";
    if (modifiers & MOD_SHIFT) binding += L"Shift+";
    if (modifiers & MOD_WIN) binding += L"Win+";
    return binding + name;
}

HotkeyManager::~HotkeyManager()
{
    Shutdown();
}

std::wstring HotkeyManager::Initialize(HWND target, HotkeyBindings const& bindings)
{
    Shutdown();
    m_target = target;
    std::wstring errors;
    for (size_t i = 0; i < bindings.size(); ++i)
    {
        if (bindings[i].find_first_not_of(L" \t") == std::wstring::npos)
        {
            continue;
        }
        UINT modifiers{};
        UINT key{};
        bool const valid = ParseBinding(bindings[i], modifiers, key);
        if (valid)
        {
            m_registered[i] = RegisterHotKey(target, static_cast<int>(i), modifiers | MOD_NOREPEAT, key) != FALSE;
        }
        if (!m_registered[i])
        {
            if (!errors.empty()) errors += L"\n";
            errors += std::wstring(HotkeyLabels[i]) + (valid ? L" — combination unavailable" : L" — invalid combination");
        }
    }
    return errors;
}

void HotkeyManager::Shutdown()
{
    for (size_t i = 0; i < m_registered.size(); ++i)
    {
        if (m_registered[i])
        {
            UnregisterHotKey(m_target, static_cast<int>(i));
            m_registered[i] = false;
        }
    }
    m_target = nullptr;
}
