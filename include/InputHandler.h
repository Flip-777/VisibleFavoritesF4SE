#pragma once

#include <cstdint>

//============= Input =============
namespace EngineInput
{
    extern bool g_keyDown[256];
    extern std::uint32_t g_padMask;
    void ClearAll();
    void Install();
}
