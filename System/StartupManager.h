#pragma once

#include <functional>

class StartupManager final
{
public:
    enum class SaveResult { Saved, StartupChangeFailed, SettingsSaveFailed, StartupRestoreFailed };
    SaveResult SaveWithChange(bool enabled, std::function<bool()> const& save) const;
};
