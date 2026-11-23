#include "TaskbarWidgetManager.h"
#include "DynamicIconRenderer.h"

#include <ole2.h>
#include <CommCtrl.h>
#include <UIAutomation.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace std::chrono_literals;
    constexpr wchar_t WidgetClass[] = L"VolumeMixer.TaskbarWidget";
    constexpr UINT RecheckMs = 1500;
    constexpr UINT ConfirmSlotMs = 150;
    constexpr UINT VisibilityCheckMs = 250;
    constexpr UINT PlacementGraceMs = 8 * RecheckMs;
    constexpr UINT RestartPlacementGraceMs = 15000;
    constexpr int PreferredSlotSize = 42;
    constexpr int CompactSlotSize = 32;

    ComPtr<IUIAutomationCacheRequest> ConnectTaskbarAutomation(ComPtr<IUIAutomation2>& automation)
    {
        automation.Reset();
        ComPtr<IUIAutomationCacheRequest> cache;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->CreateCacheRequest(&cache)))
        {
            automation->put_ConnectionTimeout(350);
            automation->put_TransactionTimeout(500);
            for (auto const property : {UIA_AutomationIdPropertyId, UIA_BoundingRectanglePropertyId,
                UIA_IsOffscreenPropertyId, UIA_ControlTypePropertyId, UIA_ProcessIdPropertyId})
            {
                cache->AddProperty(property);
            }
            cache->put_AutomationElementMode(AutomationElementMode_None);
        }
        return cache;
    }

    int Pixels(int dip, UINT dpi)
    {
        return MulDiv(dip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    }

    RECT NotificationRect(HWND taskbar)
    {
        RECT rect{};
        if (!taskbar) return rect;
        auto const tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (tray && IsWindowVisible(tray))
        {
            GetWindowRect(tray, &rect);
        }
        return rect;
    }

    std::vector<RECT> CompanionBounds(HWND taskbar, RECT const& bar, RECT tray = {}, int gap = 0, bool externalOnly = false)
    {
        std::vector<RECT> controls;
        DWORD shellPid{};
        GetWindowThreadProcessId(taskbar, &shellPid);
        const auto battery = FindWindowExW(taskbar, nullptr, L"WirelessBatteryWidget", nullptr);
        RECT batteryRect{};
        if (battery && GetWindowRect(battery, &batteryRect) && !IsRectEmpty(&batteryRect))
        {
            controls.push_back(batteryRect);
            if (!IsRectEmpty(&tray))
            {
                // Reserve the battery's tray-side slot while Explorer or the battery is repositioning it.
                auto anchored = batteryRect;
                OffsetRect(&anchored, tray.left - gap - batteryRect.right, 0);
                if (!EqualRect(&anchored, &batteryRect)) controls.push_back(anchored);
            }
        }
        for (auto child = GetWindow(taskbar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            if (child == battery) continue;
            DWORD pid{};
            RECT rect{};
            GetWindowThreadProcessId(child, &pid);
            if (pid != GetCurrentProcessId() && (!externalOnly || pid != shellPid)
                && IsWindowVisible(child) && GetWindowRect(child, &rect)
                && rect.right - rect.left < (bar.right - bar.left) / 2)
            {
                if (pid != shellPid && !IsRectEmpty(&tray) && rect.left < tray.left && rect.right > tray.left - gap)
                {
                    // Tray-adjacent widgets may move after Explorer has already widened the tray.
                    rect.left -= rect.right + gap - tray.left;
                }
                controls.push_back(rect);
            }
        }
        return controls;
    }

    bool SlotIsClear(RECT const& slot, std::span<RECT const> controls, int gap)
    {
        return std::ranges::none_of(controls, [&](RECT rect)
        {
            RECT overlap{};
            InflateRect(&rect, gap, 0);
            return IntersectRect(&overlap, &rect, &slot) != FALSE;
        });
    }

    bool CoveredByTaskbarChild(HWND widget, HWND taskbar, RECT const& bounds)
    {
        if (!GetWindow(widget, GW_HWNDPREV)) return false;

        auto const centerY = (bounds.top + bounds.bottom) / 2;
        for (LONG const x : {bounds.left + 1, (bounds.left + bounds.right) / 2, bounds.right - 2})
        {
            auto const hit = WindowFromPoint({x, centerY});
            if (hit && hit != widget && hit != taskbar && GetAncestor(hit, GA_ROOT) == taskbar)
            {
                return true;
            }
        }
        return false;
    }

    bool CoversMonitor(HWND taskbar)
    {
        auto const foreground = GetForegroundWindow();
        if (!foreground || foreground == taskbar || foreground == GetShellWindow() || foreground == GetDesktopWindow()
            || IsIconic(foreground) || IsZoomed(foreground)
            || (GetWindowLongPtrW(foreground, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
        {
            return false;
        }
        MONITORINFO monitor{sizeof(monitor)};
        RECT rect{};
        if (!GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &monitor)
            || !GetWindowRect(foreground, &rect))
        {
            return false;
        }
        auto const& screen = monitor.rcMonitor;
        return rect.left <= screen.left && rect.right >= screen.right
            && rect.top <= screen.top && rect.bottom >= screen.bottom;
    }

    // Walk toward the task buttons when a shell control occupies the space next to the tray.
    constexpr RECT FindSlot(RECT bar, LONG edge, int width, int height, int gap,
        std::span<RECT const> controls)
    {
        LONG const top = (bar.top + bar.bottom - height) / 2;
        LONG right = edge - gap;
        while (right - width >= bar.left + gap)
        {
            LONG const left = right - width;
            LONG next = right;
            for (auto const& rect : controls)
            {
                if (rect.right + gap > left && rect.left - gap < right
                    && rect.bottom > top && rect.top < top + height)
                {
                    next = std::min(next, rect.left - gap);
                }
            }
            if (next == right)
            {
                return {left, top, right, top + height};
            }
            right = next;
        }
        return {};
    }

    constexpr RECT TestBar{100, 100, 500, 140};
    constexpr RECT TestControls[]{{200, 100, 260, 140}, {340, 100, 380, 140}};
    static_assert(FindSlot(TestBar, 410, 56, 28, 4, {}).left == 350);
    static_assert(FindSlot(TestBar, 410, 56, 28, 4, TestControls).left == 280);
    static_assert(FindSlot(TestBar, 160, 56, 28, 4, {}).right == 0);

    bool ReadControls(IUIAutomation& automation, IUIAutomationCacheRequest& cache, HWND taskbar,
        RECT bar, std::vector<RECT>& controls, LONG& secondaryTrayEdge)
    {
        ComPtr<IUIAutomationElement> root;
        ComPtr<IUIAutomationCondition> controlView;
        ComPtr<IUIAutomationElementArray> elements;
        if (FAILED(automation.ElementFromHandle(taskbar, &root))
            || FAILED(automation.get_ControlViewCondition(&controlView))
            || FAILED(root->FindAllBuildCache(TreeScope_Descendants, controlView.Get(), &cache, &elements)))
        {
            return false;
        }

        int count{};
        if (FAILED(elements->get_Length(&count)))
        {
            return false;
        }
        for (int index = 0; index < count; ++index)
        {
            ComPtr<IUIAutomationElement> element;
            RECT rect{};
            BOOL offscreen{};
            CONTROLTYPEID type{};
            int processId{};
            if (FAILED(elements->GetElement(index, &element))
                || FAILED(element->get_CachedBoundingRectangle(&rect))
                || FAILED(element->get_CachedIsOffscreen(&offscreen))
                || FAILED(element->get_CachedControlType(&type))
                || (SUCCEEDED(element->get_CachedProcessId(&processId))
                    && static_cast<DWORD>(processId) == GetCurrentProcessId())
                || offscreen || !IntersectRect(&rect, &rect, &bar))
            {
                continue;
            }

            BSTR id{};
            if (SUCCEEDED(element->get_CachedAutomationId(&id)) && id)
            {
                std::wstring_view const automationId(id, SysStringLen(id));
                if (automationId == L"SystemTrayIcon" && rect.left > (bar.left + bar.right) / 2)
                {
                    secondaryTrayEdge = std::min(secondaryTrayEdge, rect.left);
                }
                if (automationId == L"WidgetsButton")
                {
                    controls.push_back(rect);
                }
            }
            SysFreeString(id);

            switch (type)
            {
            case UIA_ButtonControlTypeId:
            case UIA_SplitButtonControlTypeId:
            case UIA_EditControlTypeId:
            case UIA_CheckBoxControlTypeId:
            case UIA_MenuItemControlTypeId:
            case UIA_ListItemControlTypeId:
            case UIA_TabItemControlTypeId:
            case UIA_CustomControlTypeId:
                if (rect.right - rect.left < (bar.right - bar.left) / 2)
                {
                    controls.push_back(rect);
                }
                break;
            }
        }
        return !controls.empty();
    }

    void DrawCentered(HDC dc, wchar_t const* text, RECT rect, int height, wchar_t const* face)
    {
        auto const font = CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
        auto const previous = font ? SelectObject(dc, font) : nullptr;
        DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (previous)
        {
            SelectObject(dc, previous);
        }
        if (font)
        {
            DeleteObject(font);
        }
    }

    void CenterNumberInk(std::uint32_t* pixels, int width, int height)
    {
        RECT ink{width, height, 0, 0};
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if ((pixels[y * width + x] & 0xffu) <= 8) continue;
                ink.left = std::min<LONG>(ink.left, x);
                ink.top = std::min<LONG>(ink.top, y);
                ink.right = std::max<LONG>(ink.right, x + 1);
                ink.bottom = std::max<LONG>(ink.bottom, y + 1);
            }
        }
        if (IsRectEmpty(&ink)) return;

        // DrawText centers the font's line box. Use the visible digits and allow half-pixel offsets.
        double const sourceX = (ink.left + ink.right - width) / 2.0;
        double const sourceY = (ink.top + ink.bottom - height) / 2.0;
        if (sourceX == 0 && sourceY == 0) return;
        auto const dx = static_cast<int>(std::floor(sourceX));
        auto const dy = static_cast<int>(std::floor(sourceY));
        auto const weightX = sourceX - dx;
        auto const weightY = sourceY - dy;
        std::vector<std::uint32_t> const original(pixels, pixels + width * height);
        auto const coverage = [&](int x, int y)
        {
            return x >= 0 && x < width && y >= 0 && y < height ? original[y * width + x] & 0xffu : 0u;
        };
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                auto const top = std::lerp(coverage(x + dx, y + dy), coverage(x + dx + 1, y + dy), weightX);
                auto const bottom = std::lerp(coverage(x + dx, y + dy + 1), coverage(x + dx + 1, y + dy + 1), weightX);
                pixels[y * width + x] = static_cast<unsigned>(std::lround(std::lerp(top, bottom, weightY))) * 0x010101u;
            }
        }
    }

    void DrawVolumeRing(std::uint32_t* pixels, int width, int height, int diameter, UINT dpi, unsigned volume)
    {
        constexpr auto turn = 2 * std::numbers::pi;
        auto const halfStroke = 1.0 * dpi / USER_DEFAULT_SCREEN_DPI;
        auto const radius = diameter / 2.0 - halfStroke;
        auto const centerX = width / 2.0;
        auto const reach = static_cast<int>(std::ceil(radius + halfStroke + 0.5));
        auto const centerY = height / 2.0;
        auto const arc = turn * volume / 100.0;
        for (int y = std::max(0, height / 2 - reach); y < std::min(height, height / 2 + reach + 1); ++y)
        {
            for (int x = std::max(0, static_cast<int>(centerX) - reach);
                x < std::min(width, static_cast<int>(centerX) + reach + 1); ++x)
            {
                auto const dx = x + 0.5 - centerX;
                auto const dy = y + 0.5 - centerY;
                auto const radialDistance = std::abs(std::hypot(dx, dy) - radius);
                double const ring = std::clamp(halfStroke + 0.5 - radialDistance, 0.0, 1.0);
                if (ring == 0) continue;
                auto angle = std::atan2(dx, -dy);
                if (angle < 0) angle += turn;
                double const distance = angle <= arc ? std::min(angle, arc - angle)
                    : -std::min(angle - arc, turn - angle);
                auto const sweep = volume == 100 ? 1.0 : volume == 0 ? 0.0
                    : std::clamp(0.5 + distance * radius, 0.0, 1.0);
                auto const coverage = static_cast<unsigned>(ring * (60 + 195 * sweep));
                auto& pixel = pixels[y * width + x];
                pixel = std::max(pixel & 0xffu, coverage) * 0x010101u;
            }
        }
    }
}

TaskbarWidgetManager::~TaskbarWidgetManager()
{
    Stop();
}

void TaskbarWidgetManager::Initialize(HWND host)
{
    m_host = host;
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = WidgetClass;
    RegisterClassW(&windowClass);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
}

void TaskbarWidgetManager::SetEnabled(bool enabled)
{
    if (m_enabled == enabled)
    {
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_enabled = enabled;
    }
    if (enabled)
    {
        SetTimer(m_host, LayoutTimer, VisibilityCheckMs, nullptr);
        if (IsStopped())
        {
            m_worker = std::jthread([this](std::stop_token stop) { Measure(stop); });
        }
        m_wake.notify_one();
    }
    else
    {
        KillTimer(m_host, LayoutTimer);
        Reset();
    }
}

void TaskbarWidgetManager::RequestStop()
{
    SetEnabled(false);
    m_worker.request_stop();
}

bool TaskbarWidgetManager::IsStopped()
{
    return !m_worker.joinable() || WaitForSingleObject(m_worker.native_handle(), 0) == WAIT_OBJECT_0;
}

void TaskbarWidgetManager::Stop()
{
    RequestStop();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void TaskbarWidgetManager::Reset()
{
    m_widgets.clear();
    {
        std::lock_guard lock(m_mutex);
        ++m_layoutRevision;
        m_placements.clear();
        m_taskbarsWithoutSpace.clear();
        m_measuredAt = 0;
    }
    m_wake.notify_one();
}

void TaskbarWidgetManager::RecheckLayout()
{
    {
        std::lock_guard lock(m_mutex);
        m_layoutCheckRequested = true;
    }
    m_wake.notify_one();
}

void TaskbarWidgetManager::Measure(std::stop_token stop)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return;
    }
    {
        ComPtr<IUIAutomation2> automation;
        ComPtr<IUIAutomationCacheRequest> cache;
        DWORD shellPid{};
        ULONGLONG shellRestartedAt{};
        std::vector<Placement> confirmedPlacements;
        while (!stop.stop_requested())
        {
            unsigned revision;
            bool confirmSoon;
            std::vector<Placement> previousPlacements;
            {
                std::unique_lock lock(m_mutex);
                if (!m_wake.wait(lock, stop, [&] { return m_enabled; }) || stop.stop_requested())
                {
                    break;
                }
                revision = m_layoutRevision;
                confirmSoon = m_layoutCheckRequested;
                m_layoutCheckRequested = false;
                previousPlacements = m_placements;
            }
            std::vector<HWND> bars;
            if (auto const primary = FindWindowW(L"Shell_TrayWnd", nullptr))
            {
                bars.push_back(primary);
            }
            for (HWND secondary{}; (secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr));)
            {
                bars.push_back(secondary);
            }
            DWORD currentShellPid{};
            if (!bars.empty()) GetWindowThreadProcessId(bars.front(), &currentShellPid);
            if (currentShellPid && (!cache || shellPid != currentShellPid))
            {
                // A long-lived UI Automation client may keep Explorer's previous provider after a restart.
                if (shellPid && shellPid != currentShellPid) shellRestartedAt = GetTickCount64();
                cache.Reset();
                cache = ConnectTaskbarAutomation(automation);
                shellPid = currentShellPid;
            }
            std::vector<Placement> placements;
            std::vector<HWND> unmeasured;
            std::vector<HWND> withoutSpace;
            auto const measuredAt = GetTickCount64();
            bool const recovering = shellRestartedAt && measuredAt - shellRestartedAt < PlacementGraceMs;
            std::erase_if(confirmedPlacements, [&](Placement const& placement)
            {
                return measuredAt - placement.measuredAt >= RestartPlacementGraceMs;
            });
            for (auto const taskbar : bars)
            {
                {
                    std::lock_guard lock(m_mutex);
                    if (stop.stop_requested() || !m_enabled || revision != m_layoutRevision)
                    {
                        break;
                    }
                }
                RECT bar{};
                auto const dpi = GetDpiForWindow(taskbar);
                if (!dpi || !IsWindowVisible(taskbar) || !GetWindowRect(taskbar, &bar)
                    || bar.right - bar.left <= bar.bottom - bar.top
                    || bar.bottom - bar.top < Pixels(28, dpi)
                    || (GetWindowLongPtrW(taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL))
                {
                    continue;
                }
                auto const tray = NotificationRect(taskbar);
                LONG edge = bar.right;
                std::vector<RECT> controls;
                bool const measured = cache && ReadControls(*automation.Get(), *cache.Get(), taskbar, bar, controls, edge);
                auto const gap = Pixels(4, dpi);
                auto const companionControls = CompanionBounds(taskbar, bar, tray, gap);
                controls.insert(controls.end(), companionControls.begin(), companionControls.end());
                auto const confirmed = std::ranges::find_if(confirmedPlacements,
                    [&](Placement const& prior) { return EqualRect(&prior.bar, &bar); });
                if (!measured)
                {
                    if (confirmed != confirmedPlacements.end() && confirmed->taskbar != taskbar
                        && confirmed->dpi == dpi
                        && (IsRectEmpty(&tray) || confirmed->slot.right <= tray.left - gap)
                        && SlotIsClear(confirmed->slot, controls, gap))
                    {
                        // Explorer may show the bar before publishing its accessible controls.
                        auto provisional = *confirmed;
                        provisional.taskbar = taskbar;
                        provisional.pendingCloserSlot = {};
                        provisional.provisional = true;
                        placements.push_back(provisional);
                    }
                    else
                    {
                        unmeasured.push_back(taskbar);
                    }
                    continue;
                }
                if (!IsRectEmpty(&tray))
                {
                    edge = std::min(edge, tray.left);
                }
                if (edge == bar.right)
                {
                    unmeasured.push_back(taskbar);
                    continue;
                }
                auto nearby = bar;
                nearby.left = std::max(nearby.left, edge - Pixels(256, dpi));
                auto const maxHeight = static_cast<int>(bar.bottom - bar.top) - Pixels(4, dpi);
                auto const size = std::min(Pixels(PreferredSlotSize, dpi), maxHeight);
                auto slot = FindSlot(nearby, edge, size, size, gap, controls);
                if (IsRectEmpty(&slot))
                {
                    auto const compact = std::min(size, Pixels(CompactSlotSize, dpi));
                    if (compact < size)
                    {
                        slot = FindSlot(nearby, edge, compact, compact, gap, controls);
                    }
                }
                auto const prior = std::ranges::find(previousPlacements, taskbar, &Placement::taskbar);
                RECT pendingCloserSlot{};
                if (!IsRectEmpty(&slot) && prior != previousPlacements.end() && prior->dpi == dpi
                    && prior->slot.right - prior->slot.left == slot.right - slot.left && EqualRect(&bar, &prior->bar)
                    && (recovering || prior->slot.left >= nearby.left) && prior->slot.right <= edge - gap
                    && SlotIsClear(prior->slot, controls, gap))
                {
                    // Keep the current safe position until the closer opening is measured twice.
                    if (slot.right <= prior->slot.right || recovering
                        || !EqualRect(&slot, &prior->pendingCloserSlot))
                    {
                        if (slot.right > prior->slot.right)
                        {
                            pendingCloserSlot = slot;
                            // Verify the newly available slot soon, without speeding up routine polling.
                            confirmSoon |= !recovering;
                        }
                        slot = prior->slot;
                    }
                }
                if (!IsRectEmpty(&slot))
                {
                    placements.push_back({taskbar, bar, slot, dpi, GetTickCount64(), pendingCloserSlot});
                    if (confirmed == confirmedPlacements.end()) confirmedPlacements.push_back(placements.back());
                    else *confirmed = placements.back();
                }
                else
                {
                    if (confirmed != confirmedPlacements.end()) confirmedPlacements.erase(confirmed);
                    withoutSpace.push_back(taskbar);
                }
            }
            std::unique_lock lock(m_mutex);
            if (m_enabled && revision == m_layoutRevision && !stop.stop_requested())
            {
                auto const now = GetTickCount64();
                for (auto const& previous : m_placements)
                {
                    if (!previous.provisional && now - previous.measuredAt < PlacementGraceMs
                        && std::ranges::find(unmeasured, previous.taskbar) != unmeasured.end())
                    {
                        placements.push_back(previous);
                        placements.back().pendingCloserSlot = {};
                    }
                }
                for (auto const bar : m_taskbarsWithoutSpace)
                {
                    if (std::ranges::find(unmeasured, bar) != unmeasured.end()) withoutSpace.push_back(bar);
                }
                m_placements = std::move(placements);
                m_taskbarsWithoutSpace = std::move(withoutSpace);
                m_measuredAt = now;
                PostMessageW(m_host, LayoutMessage, 0, 0);
            }
            m_wake.wait_for(lock, stop, std::chrono::milliseconds(confirmSoon ? ConfirmSlotMs : RecheckMs),
                [&] { return !m_enabled || revision != m_layoutRevision || m_layoutCheckRequested; });
        }
    }
    CoUninitialize();
}

bool TaskbarWidgetManager::ApplyLayout()
{
    auto const primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    auto const primaryTray = NotificationRect(primary);
    RECT primaryBounds{};
    auto const primaryGap = Pixels(4, GetDpiForWindow(primary));
    auto companions = m_enabled && primary && GetWindowRect(primary, &primaryBounds)
        ? CompanionBounds(primary, primaryBounds, primaryTray, primaryGap, true) : std::vector<RECT>{};
    auto const sameBounds = [](RECT const& a, RECT const& b) { return EqualRect(&a, &b) != FALSE; };
    if (primary != m_primaryTaskbar || !EqualRect(&primaryTray, &m_primaryTray)
        || !std::ranges::equal(companions, m_primaryCompanions, sameBounds))
    {
        m_primaryTaskbar = primary;
        m_primaryTray = primaryTray;
        m_primaryCompanions = std::move(companions);
        RecheckLayout();
    }
    std::vector<Placement> placements;
    std::vector<HWND> withoutSpace;
    {
        std::lock_guard lock(m_mutex);
        if (m_enabled && GetTickCount64() - m_measuredAt < RecheckMs * 3)
        {
            placements = m_placements;
            withoutSpace = m_taskbarsWithoutSpace;
        }
    }
    APPBARDATA appbar{sizeof(appbar)};
    bool companionOverlap{};
    auto const invalid = [&](Placement const& placement)
    {
        RECT current{};
        auto const tray = NotificationRect(placement.taskbar);
        RECT overlap{};
        const int gap = Pixels(4, placement.dpi);
        if (placement.taskbar == primary && !SlotIsClear(placement.slot, m_primaryCompanions, gap))
        {
            companionOverlap = true;
            return true;
        }
        return !IsWindowVisible(placement.taskbar) || !GetWindowRect(placement.taskbar, &current)
            || !IntersectRect(&overlap, &placement.slot, &current) || !EqualRect(&overlap, &placement.slot)
            || IntersectRect(&overlap, &placement.slot, &tray)
            || GetDpiForWindow(placement.taskbar) != placement.dpi || CoversMonitor(placement.taskbar)
            || (placement.provisional && (!EqualRect(&current, &placement.bar)
                || (!IsRectEmpty(&tray) && placement.slot.right > tray.left - Pixels(4, placement.dpi))
                || !SlotIsClear(placement.slot, CompanionBounds(placement.taskbar, current), Pixels(4, placement.dpi))));
    };
    std::erase_if(placements, invalid);
    // Auto-hide can slide a child partly offscreen between measurements. Keep the tray entry in this mode.
    bool const autoHide = (SHAppBarMessage(ABM_GETSTATE, &appbar) & ABS_AUTOHIDE) != 0;
    if (autoHide)
    {
        placements.clear();
    }
    std::erase_if(m_widgets, [](auto const& widget)
    {
        return !IsWindow(widget->window) || !IsWindow(widget->taskbar);
    });
    for (auto const& widget : m_widgets)
    {
        if (std::ranges::find(placements, widget->taskbar, &Placement::taskbar) != placements.end()) continue;
        auto const previous = Placement{widget->taskbar, widget->taskbarBounds, widget->slot, widget->dpi, widget->lastPlacement};
        if (!autoHide && !widget->provisional && std::ranges::find(withoutSpace, widget->taskbar) == withoutSpace.end()
            && GetTickCount64() - widget->lastPlacement < PlacementGraceMs && !invalid(previous))
        {
            placements.push_back(previous);
        }
        else if (IsWindowVisible(widget->window))
        {
            ShowWindow(widget->window, SW_HIDE);
        }
    }
    bool const light = m_light;
    bool overlapFound{};
    for (auto const& placement : placements)
    {
        auto at = std::find_if(m_widgets.begin(), m_widgets.end(),
            [&](auto const& widget) { return widget->taskbar == placement.taskbar; });
        if (at == m_widgets.end())
        {
            auto widget = std::make_unique<Widget>();
            widget->manager = this;
            widget->taskbar = placement.taskbar;
            auto const module = GetModuleHandleW(nullptr);
            auto const oldContext = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(placement.taskbar));
            widget->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
                WidgetClass, L"Volume Mixer", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, placement.taskbar,
                nullptr, module, widget.get());
            if (oldContext) SetThreadDpiAwarenessContext(oldContext);
            if (!widget->window)
            {
                continue;
            }
            widget->tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, m_host, nullptr, module, nullptr);
            if (widget->tooltip)
            {
                TOOLINFOW tool{sizeof(tool)};
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = m_host;
                tool.uId = reinterpret_cast<UINT_PTR>(widget->window);
                tool.lpszText = m_tooltip.data();
                SendMessageW(widget->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
            m_widgets.push_back(std::move(widget));
            at = std::prev(m_widgets.end());
        }
        auto& widget = **at;
        widget.provisional = placement.provisional;
        widget.taskbarBounds = placement.bar;
        widget.lastPlacement = placement.measuredAt;
        bool const repaint = widget.dpi != placement.dpi || widget.light != light
            || widget.slot.right - widget.slot.left != placement.slot.right - placement.slot.left
            || widget.slot.bottom - widget.slot.top != placement.slot.bottom - placement.slot.top;
        widget.slot = placement.slot;
        if (repaint)
        {
            widget.dpi = placement.dpi;
            widget.light = light;
            if (!widget.Paint())
            {
                m_widgets.erase(at);
                continue;
            }
        }
        RECT current{};
        bool const needsPosition = !IsWindowVisible(widget.window) || !GetWindowRect(widget.window, &current)
            || !EqualRect(&current, &widget.slot);
        bool const covered = !needsPosition && CoveredByTaskbarChild(widget.window, widget.taskbar, current);
        overlapFound |= covered;
        if (needsPosition || covered)
        {
            POINT origin{widget.slot.left, widget.slot.top};
            if (!ScreenToClient(widget.taskbar, &origin)
                || !SetWindowPos(widget.window, HWND_TOP, origin.x, origin.y, 0, 0,
                    SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
            {
                m_widgets.erase(at);
            }
        }
    }
    if (overlapFound || companionOverlap) RecheckLayout();
    return std::any_of(m_widgets.begin(), m_widgets.end(),
        [](auto const& widget) { return IsWindowVisible(widget->window); });
}

bool TaskbarWidgetManager::TryGetRect(RECT& rect) const
{
    for (auto const& widget : m_widgets)
    {
        if (IsWindowVisible(widget->window) && GetWindowRect(widget->window, &rect))
        {
            return true;
        }
    }
    return false;
}

void TaskbarWidgetManager::Update(bool available, unsigned volume, bool muted, bool headphones, bool showNumber,
    bool zeroWhenMuted, std::wstring const& tooltip)
{
    volume = std::min(volume, 100u);
    bool const repaint = m_available != available || m_volume != volume || m_muted != muted
        || m_showNumber != showNumber || m_zeroWhenMuted != zeroWhenMuted
        || (!showNumber && m_headphones != headphones);
    m_available = available;
    m_volume = volume;
    m_muted = muted;
    m_headphones = headphones;
    m_showNumber = showNumber;
    m_zeroWhenMuted = zeroWhenMuted;
    if (m_tooltip != tooltip)
    {
        m_tooltip = tooltip;
        for (auto const& widget : m_widgets)
        {
            widget->UpdateTooltip();
        }
    }
    if (repaint)
    {
        for (auto const& widget : m_widgets)
        {
            widget->Paint();
        }
    }
}

TaskbarWidgetManager::Widget::~Widget()
{
    if (tooltip)
    {
        DestroyWindow(tooltip);
    }
    if (window)
    {
        DestroyWindow(window);
    }
}

void TaskbarWidgetManager::Widget::UpdateTooltip() const
{
    if (tooltip)
    {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = manager->m_host;
        tool.uId = reinterpret_cast<UINT_PTR>(window);
        tool.lpszText = manager->m_tooltip.data();
        SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}

bool TaskbarWidgetManager::Widget::Paint() const
{
    auto const width = slot.right - slot.left;
    auto const height = slot.bottom - slot.top;
    if (!dpi || !IsWindow(window) || width <= 0 || height <= 0)
    {
        return false;
    }
    HDC const dc = CreateCompatibleDC(nullptr);
    if (!dc)
    {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits{};
    HBITMAP const bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap)
    {
        DeleteDC(dc);
        return false;
    }
    auto const old = SelectObject(dc, bitmap);
    PatBlt(dc, 0, 0, width, height, BLACKNESS);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto const diameter = std::min(Pixels(34, dpi), std::min<int>(width, height) - Pixels(6, dpi));
    RECT const bounds{0, 0, width, height};
    if (manager->m_showNumber)
    {
        auto const value = manager->m_muted && manager->m_zeroWhenMuted ? 0u : manager->m_volume;
        auto const label = manager->m_available ? std::to_wstring(value) : L"\u2014";
        DrawCentered(dc, label.c_str(), bounds, std::min(Pixels(15, dpi), diameter * 4 / 9),
            L"Segoe UI Variable Text");
    }
    else
    {
        bool const headphones = manager->m_available && manager->m_headphones;
        if (headphones && manager->m_muted) SetTextColor(dc, RGB(145, 145, 145));
        wchar_t const icon[]{headphones ? L'\uE7F6' : DynamicIconRenderer::SpeakerGlyph(manager->m_volume,
            manager->m_muted || !manager->m_available), L'\0'};
        DrawCentered(dc, icon, bounds, std::min(Pixels(19, dpi), diameter * 5 / 9), L"Segoe Fluent Icons");
    }
    GdiFlush();
    auto const pixels = static_cast<std::uint32_t*>(bits);
    if (manager->m_showNumber)
    {
        CenterNumberInk(pixels, width, height);
    }
    auto const ringVolume = manager->m_muted || !manager->m_available ? 0u : manager->m_volume;
    DrawVolumeRing(pixels, width, height, diameter, dpi, ringVolume);
    double const hoverRadius = (diameter + Pixels(6, dpi)) / 2.0;
    for (LONG y = 0; y < height; ++y)
    {
        auto const dy = y + 0.5 - height / 2.0;
        for (LONG x = 0; x < width; ++x)
        {
            unsigned background = 1; // Minimal alpha keeps even the rounded corners clickable.
            if (hover)
            {
                auto const dx = x + 0.5 - width / 2.0;
                auto const fill = std::clamp(hoverRadius + 0.5 - std::hypot(dx, dy), 0.0, 1.0);
                background += static_cast<unsigned>(std::lround(21 * fill));
            }
            auto& pixel = pixels[y * width + x];
            unsigned const coverage = pixel & 0xffu;
            unsigned const alpha = coverage + background * (255 - coverage) / 255;
            pixel = (alpha << 24) | (light ? 0u : alpha * 0x010101u);
        }
    }
    SIZE size{width, height};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    bool const drawn = UpdateLayeredWindow(window, nullptr, nullptr, &size, dc, &source, 0, &blend, ULW_ALPHA) != FALSE;
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return drawn;
}

LRESULT CALLBACK TaskbarWidgetManager::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        auto const create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto const widget = reinterpret_cast<Widget*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!widget)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    auto const send = [&](Input input, LPARAM payload)
    {
        PostMessageW(widget->manager->m_host, InputMessage, static_cast<WPARAM>(input), payload);
    };
    switch (message)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (!widget->hover)
        {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window};
            TrackMouseEvent(&track);
            widget->hover = true;
            widget->Paint();
        }
        return 0;
    case WM_MOUSELEAVE:
        widget->hover = false;
        widget->Paint();
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    {
        POINT anchor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window, &anchor);
        send(message == WM_LBUTTONUP ? Input::ToggleMixer : Input::Menu, MAKELPARAM(anchor.x, anchor.y));
        return 0;
    }
    case WM_MBUTTONUP:
        send(Input::Mute, 0);
        return 0;
    case WM_MOUSEWHEEL:
    {
        POINT const cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rect{};
        if (GetWindowRect(window, &rect) && PtInRect(&rect, cursor))
        {
            bool const control = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
            auto& remainder = control ? widget->deviceWheelRemainder : widget->wheelRemainder;
            remainder += GET_WHEEL_DELTA_WPARAM(wParam);
            int const steps = remainder / WHEEL_DELTA;
            remainder %= WHEEL_DELTA;
            if (steps)
            {
                send(control ? Input::Device : Input::Volume, steps);
            }
        }
        return 0;
    }
    case WM_NCDESTROY:
        widget->window = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
