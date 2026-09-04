#pragma once

struct ID3D11Device;

//============= Hooks =============
namespace Display
{
    void InstallAnimSinkHook();
}

namespace Overlay
{
    extern ID3D11Device* g_device;
    void Install();
}
