#pragma once

#include <atomic>
#include <string>

struct ID3D11Device;

//============= Hooks =============
namespace Display
{
    void InstallAnimSinkHook();
}

namespace Overlay
{
    extern std::atomic<ID3D11Device*> g_device;
    void Install();
    std::string PresentStatus();
}
