#pragma once

#include <atomic>

//============= Overlay =============
namespace Overlay
{
    extern std::atomic<bool> g_open;
    extern bool g_imguiReady;
    extern std::atomic<bool> g_kbOwned;
    extern std::atomic<bool> g_unsaved;
    extern std::atomic<bool> g_confirmClose;
    extern std::atomic<bool> g_needCapture;
    extern float g_lastW;
    extern float g_lastH;
    extern float g_lastX;
    extern float g_lastY;
    void DrawPanel();
    void LoadBodyTexture();
}
