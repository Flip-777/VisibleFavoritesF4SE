#pragma once

#include <string>

//============= INI Persistence =============
namespace Slots
{
    std::string IniSafe(std::string s);
    void Save();
    void Load();
}
