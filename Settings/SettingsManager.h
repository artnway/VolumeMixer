#pragma once
#include <filesystem>
#include "SettingsModel.h"

class SettingsManager final
{
public:
    SettingsModel Load() const;
    bool Save(SettingsModel const& settings) const;

private:
    std::filesystem::path FilePath() const;
};
