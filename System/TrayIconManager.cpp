#include "pch.h"
#include "TrayIconManager.h"

namespace
{
    constexpr UINT IconFields = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

TrayIconManager::~TrayIconManager()
{
    Shutdown();
}

bool TrayIconManager::Initialize(HWND owner, HICON icon, std::wstring const& tooltip)
{
    Shutdown();
    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = owner;
    m_data.uID = 1;
    m_identityFlags = NIF_GUID;
    m_data.uFlags = IconFields | m_identityFlags;
    m_data.uCallbackMessage = CallbackMessage;
    m_data.guidItem = { 0x6ee3d22f, 0x6eb8, 0x4d54, { 0x9b, 0x29, 0x84, 0x7f, 0x58, 0x4e, 0xf4, 0x31 } };
    Update(icon, tooltip);
    return Add();
}

void TrayIconManager::Update(HICON icon, std::wstring const& tooltip)
{
    wchar_t tip[ARRAYSIZE(m_data.szTip)]{};
    StringCchCopyW(tip, ARRAYSIZE(tip), tooltip.c_str());
    UINT changes = 0;
    if (m_data.hIcon != icon)
    {
        changes |= NIF_ICON;
    }
    if (wcscmp(m_data.szTip, tip) != 0)
    {
        changes |= NIF_TIP | NIF_SHOWTIP;
    }
    m_data.hIcon = icon;
    StringCchCopyW(m_data.szTip, ARRAYSIZE(m_data.szTip), tip);
    if (m_added && changes)
    {
        m_data.uFlags = changes | m_identityFlags;
        m_added = Shell_NotifyIconW(NIM_MODIFY, &m_data) != FALSE;
        m_data.uFlags = IconFields | m_identityFlags;
    }
}

void TrayIconManager::EnsureRegistered()
{
    if (m_added)
    {
        auto probe = m_data;
        probe.uFlags = m_identityFlags | NIF_MESSAGE;
        m_added = Shell_NotifyIconW(NIM_MODIFY, &probe) != FALSE;
    }
    if (!m_added) Add();
}

void TrayIconManager::Recreate()
{
    m_added = false;
    Add();
}

void TrayIconManager::Shutdown()
{
    if (m_added)
    {
        Shell_NotifyIconW(NIM_DELETE, &m_data);
        m_added = false;
    }
}

bool TrayIconManager::TryGetIconRect(RECT& rect) const
{
    NOTIFYICONIDENTIFIER identifier{ sizeof(identifier) };
    identifier.hWnd = m_data.hWnd;
    identifier.uID = m_data.uID;
    if (m_identityFlags) identifier.guidItem = m_data.guidItem;
    return SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &rect));
}

bool TrayIconManager::Add()
{
    if (!m_data.hWnd)
    {
        return false;
    }
    m_data.uFlags = IconFields | m_identityFlags;
    m_added = Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
    if (!m_added && m_identityFlags)
    {
        // Explorer can reject a GUID previously registered from a different executable path.
        m_data.uFlags = IconFields;
        m_added = Shell_NotifyIconW(NIM_ADD, &m_data) != FALSE;
        if (m_added) m_identityFlags = 0;
        else m_data.uFlags = IconFields | m_identityFlags;
    }
    if (m_added)
    {
        m_data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &m_data);
    }
    return m_added;
}
