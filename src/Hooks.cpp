#include "Hooks.h"

#include "Config.h"
#include "DisplayManager.h"
#include "InputHandler.h"
#include "Logger.h"
#include "NpcDisplay.h"
#include "Panel.h"
#include "SlotManager.h"

#include <imgui_internal.h>

#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d11.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

//============= Anim-Graph Events (draw/sheathe) =============
namespace Display
{
    using AnimSinkFn = RE::BSEventNotifyControl (*)(void*, RE::BSAnimationGraphEvent*, void*);
    AnimSinkFn g_animSinkOrig = nullptr;
    AnimSinkFn g_npcAnimSinkOrig = nullptr;
    bool g_animBaseChecked = false;
    bool g_animBaseVerified = false;

    RE::BSEventNotifyControl AnimSinkHook(void* a_this, RE::BSAnimationGraphEvent* a_event, void* a_source) {
        if (a_event) {
            if (!g_animBaseChecked) {
                g_animBaseChecked = true;
                auto* pc = RE::PlayerCharacter::GetSingleton();
                g_animBaseVerified = pc && a_this == static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(pc);
                logger::info("anim-sink base {}",
                    g_animBaseVerified ? "VERIFIED against player singleton" : "MISMATCH - npc draw flips disabled");
            }
            if (const char* tag = a_event->tag.c_str(); tag && tag[0]) {
                if (_stricmp(tag, "weaponDraw") == 0) {
                    Schedule([]() { OnPlayerDrawFlip(true); }, 0);
                } else if (_stricmp(tag, "weaponSheathe") == 0) {
                    Schedule([]() { OnPlayerDrawFlip(false); }, 0);
                }
            }
        }
        return g_animSinkOrig(a_this, a_event, a_source);
    }

    RE::BSEventNotifyControl NpcAnimSinkHook(void* a_this, RE::BSAnimationGraphEvent* a_event, void* a_source) {
        if (g_animBaseVerified && a_event) {
            if (const char* tag = a_event->tag.c_str(); tag && tag[0]) {
                const bool draw = _stricmp(tag, "weaponDraw") == 0;
                if (draw || _stricmp(tag, "weaponSheathe") == 0) {
                    auto* sink = static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(a_this);
                    const auto id = static_cast<RE::TESObjectREFR*>(sink)->GetFormID();
                    Schedule([id, draw]() { Npc::OnDrawFlip(id, draw); }, 0);
                }
            }
        }
        return g_npcAnimSinkOrig(a_this, a_event, a_source);
    }

    void InstallAnimSinkHook() {
        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE::PlayerCharacter[3] };
        g_animSinkOrig = *reinterpret_cast<AnimSinkFn*>(pcVtbl.address() + sizeof(std::uintptr_t));
        pcVtbl.write_vfunc(1, reinterpret_cast<std::uintptr_t>(AnimSinkHook));
        REL::Relocation<std::uintptr_t> actorVtbl{ RE::VTABLE::Actor[3] };
        g_npcAnimSinkOrig = *reinterpret_cast<AnimSinkFn*>(actorVtbl.address() + sizeof(std::uintptr_t));
        actorVtbl.write_vfunc(1, reinterpret_cast<std::uintptr_t>(NpcAnimSinkHook));
        logger::info("anim-event hooks installed (player + actor)");
    }
}

//============= Win32 & D3D Hook State =============
namespace Overlay
{
    bool g_imguiReady = false;

    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_origPresent = nullptr;

    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    SetCursorPosFn g_origSetCursorPos = nullptr;
    BOOL WINAPI HookedSetCursorPos(int a_x, int a_y) {
        if (g_open.load()) {
            return TRUE;
        }
        return g_origSetCursorPos(a_x, a_y);
    }

    using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
    ClipCursorFn g_origClipCursor = nullptr;

    RECT FullScreenClip() {
        RECT r{};
        r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return r;
    }

    BOOL WINAPI HookedClipCursor(const RECT* a_rect) {
        if (g_open.load()) {
            const RECT full = FullScreenClip();
            return g_origClipCursor(&full);
        }
        return g_origClipCursor(a_rect);
    }
    std::atomic<WNDPROC> g_origWndProc{ nullptr };
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    bool g_initFailed = false;
    IDXGISwapChain* g_gameChain = nullptr;

    struct InputEvent
    {
        enum class Kind : std::uint8_t
        {
            MousePos,
            MouseButton,
            Wheel,
            Key,
            Char
        };
        Kind kind{ Kind::MousePos };
        float x{ 0 }, y{ 0 };
        int button{ 0 };
        bool down{ false };
        ImGuiKey key{ ImGuiKey_None };
        unsigned int ch{ 0 };
    };
    std::mutex g_inputMutex;
    std::vector<InputEvent> g_inputQueue;
    std::atomic<bool> g_wantMouse{ false };

    void PushInput(const InputEvent& a_ev) {
        std::lock_guard lock(g_inputMutex);
        if (g_inputQueue.size() < 512) {
            g_inputQueue.push_back(a_ev);
        }
    }

    void DrainInput() {
        std::vector<InputEvent> evs;
        {
            std::lock_guard lock(g_inputMutex);
            evs.swap(g_inputQueue);
        }
        auto& io = ImGui::GetIO();
        for (const auto& e : evs) {
            switch (e.kind) {
            case InputEvent::Kind::MousePos:
                io.AddMousePosEvent(e.x, e.y);
                break;
            case InputEvent::Kind::MouseButton:
                io.AddMouseButtonEvent(e.button, e.down);
                break;
            case InputEvent::Kind::Wheel:
                io.AddMouseWheelEvent(0.0f, e.x);
                break;
            case InputEvent::Kind::Key:
                io.AddKeyEvent(e.key, e.down);
                break;
            case InputEvent::Kind::Char:
                io.AddInputCharacter(e.ch);
                break;
            }
        }
    }

    ImGuiKey MapVk(WPARAM a_vk) {
        switch (a_vk) {
        case VK_TAB:
            return ImGuiKey_Tab;
        case VK_LEFT:
            return ImGuiKey_LeftArrow;
        case VK_RIGHT:
            return ImGuiKey_RightArrow;
        case VK_UP:
            return ImGuiKey_UpArrow;
        case VK_DOWN:
            return ImGuiKey_DownArrow;
        case VK_PRIOR:
            return ImGuiKey_PageUp;
        case VK_NEXT:
            return ImGuiKey_PageDown;
        case VK_HOME:
            return ImGuiKey_Home;
        case VK_END:
            return ImGuiKey_End;
        case VK_DELETE:
            return ImGuiKey_Delete;
        case VK_BACK:
            return ImGuiKey_Backspace;
        case VK_RETURN:
            return ImGuiKey_Enter;
        case VK_ESCAPE:
            return ImGuiKey_Escape;
        case VK_SHIFT:
            return ImGuiMod_Shift;
        case VK_CONTROL:
            return ImGuiMod_Ctrl;
        case 'A':
            return ImGuiKey_A;
        case 'C':
            return ImGuiKey_C;
        case 'V':
            return ImGuiKey_V;
        case 'X':
            return ImGuiKey_X;
        case 'Z':
            return ImGuiKey_Z;
        default:
            return ImGuiKey_None;
        }
    }

    //============= Present Hook & ImGui Bootstrap =============
    LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_w, LPARAM a_l) {
        const WNDPROC orig = g_origWndProc.load();
        if (a_msg == WM_ACTIVATEAPP && a_w == FALSE) {
            EngineInput::ClearAll();
        }
        if (g_open.load()) {
            const bool overUI = g_wantMouse.load();
            const bool mmbLook = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            const bool kbOwned = g_kbOwned.load();
            switch (a_msg) {
            case WM_INPUT: {
                RAWINPUT ri{};
                UINT sz = sizeof(ri);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(a_l), RID_INPUT, &ri, &sz,
                        sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                    if (ri.header.dwType == RIM_TYPEKEYBOARD && !kbOwned) {
                        break;
                    }
                    if (ri.header.dwType == RIM_TYPEMOUSE) {
                        if ((ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && !overUI) {
                            break;
                        }
                        if (mmbLook && ri.data.mouse.usButtonFlags == 0) {
                            break;
                        }
                    }
                }
                return DefWindowProcA(a_hwnd, a_msg, a_w, a_l);
            }
            case WM_MOUSEWHEEL:
                if (!overUI) {
                    break;
                }
                PushInput({ .kind = InputEvent::Kind::Wheel,
                    .x = static_cast<float>(GET_WHEEL_DELTA_WPARAM(a_w)) / WHEEL_DELTA });
                return 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (!kbOwned) {
                    break;
                }
                if (const auto k = MapVk(a_w); k != ImGuiKey_None) {
                    PushInput({ .kind = InputEvent::Kind::Key,
                        .down = a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN, .key = k });
                }
                return 0;
            case WM_CHAR:
                if (!kbOwned) {
                    break;
                }
                PushInput({ .kind = InputEvent::Kind::Char, .ch = static_cast<unsigned int>(a_w) });
                return 0;
            case WM_MOUSEMOVE:
                PushInput({ .kind = InputEvent::Kind::MousePos,
                    .x = static_cast<float>(static_cast<short>(LOWORD(a_l))),
                    .y = static_cast<float>(static_cast<short>(HIWORD(a_l))) });
                return 0;
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 0, .down = true });
                return 0;
            case WM_LBUTTONUP:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 0, .down = false });
                return 0;
            case WM_RBUTTONDOWN:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 1, .down = true });
                return 0;
            case WM_RBUTTONUP:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 1, .down = false });
                return 0;
            case WM_MBUTTONDOWN:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 2, .down = true });
                return 0;
            case WM_MBUTTONUP:
                PushInput({ .kind = InputEvent::Kind::MouseButton, .button = 2, .down = false });
                return 0;
            default:
                break;
            }
        }
        if (!orig) {
            return DefWindowProcA(a_hwnd, a_msg, a_w, a_l);
        }
        return CallWindowProcA(orig, a_hwnd, a_msg, a_w, a_l);
    }

    void InitOverlayOnce(IDXGISwapChain* a_chain) {
        g_initFailed = true;
        auto* rd = RE::BSGraphics::RendererData::GetSingleton();
        ID3D11Device* dev = nullptr;
        if (FAILED(a_chain->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
            dev = rd ? reinterpret_cast<ID3D11Device*>(rd->device) : nullptr;
            if (!dev) {
                logger::error("overlay: no D3D11 device from the swap chain or the renderer - overlay disabled");
                return;
            }
            dev->AddRef();
            logger::info("overlay: swap chain refused GetDevice (proxied by another mod) - using the game's device");
        }
        DXGI_SWAP_CHAIN_DESC desc{};
        HWND hwnd = SUCCEEDED(a_chain->GetDesc(&desc)) ? desc.OutputWindow : nullptr;
        if (!hwnd && rd) {
            hwnd = reinterpret_cast<HWND>(rd->renderWindow[0].hwnd);
        }
        if (!hwnd) {
            logger::error("overlay: no game window - overlay disabled");
            dev->Release();
            return;
        }
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (!ctx) {
            logger::error("overlay: no immediate context - overlay disabled");
            dev->Release();
            return;
        }
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().FontGlobalScale = Slots::g_overlayScale;
        {
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->AddFontDefault();
            char winDir[MAX_PATH]{};
            if (GetWindowsDirectoryA(winDir, MAX_PATH)) {
                ImFontConfig cfg;
                cfg.MergeMode = true;
                bool merged = false;
                for (const char* fontName : { "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" }) {
                    const std::string fontPath = std::string(winDir) + "\\Fonts\\" + fontName;
                    std::error_code fec;
                    if (std::filesystem::exists(fontPath, fec) &&
                        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 15.0f, &cfg, io.Fonts->GetGlyphRangesJapanese())) {
                        logger::info("overlay font: merged {}", fontName);
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    logger::info("overlay font: no Japanese font found - non-ASCII names may not render");
                }
            }
        }
        if (!ImGui_ImplWin32_Init(hwnd)) {
            logger::error("overlay: imgui win32 backend init failed - overlay disabled");
            ImGui::DestroyContext();
            ctx->Release();
            dev->Release();
            return;
        }
        if (!ImGui_ImplDX11_Init(dev, ctx)) {
            logger::error("overlay: imgui dx11 backend init failed - overlay disabled");
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            ctx->Release();
            dev->Release();
            return;
        }
        g_device = dev;
        g_context = ctx;
        LoadBodyTexture();
        SetLastError(0);
        const auto prev = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));
        if (!prev && GetLastError() != 0) {
            logger::error("overlay: wndproc subclass failed (error {}) - overlay disabled", GetLastError());
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_device = nullptr;
            g_context = nullptr;
            ctx->Release();
            dev->Release();
            return;
        }
        g_origWndProc.store(reinterpret_cast<WNDPROC>(prev));
        g_initFailed = false;
        g_imguiReady = true;
        logger::info("overlay: imgui ready (hwnd={})", fmt::ptr(hwnd));
    }

    HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_chain, UINT a_sync, UINT a_flags) {
        if (a_chain != g_gameChain) {
            return g_origPresent(a_chain, a_sync, a_flags);
        }
        if (!g_imguiReady && !g_initFailed) {
            InitOverlayOnce(a_chain);
        }
        if (g_imguiReady) {
            ImGui::GetIO().MouseDrawCursor = g_open.load();
            static bool wasOpen = false;
            static unsigned framesDrawn = 0;
            static unsigned framesSkipped = 0;
            if (const bool nowOpen = g_open.load(); nowOpen != wasOpen) {
                wasOpen = nowOpen;
                if (nowOpen) {
                    framesDrawn = 0;
                    framesSkipped = 0;
                    const RECT full = FullScreenClip();
                    ClipCursor(&full);
                } else {
                    logger::info("overlay: panel session ended - {} frames drawn, {} skipped (tables busy)", framesDrawn, framesSkipped);
                }
            }
            std::unique_lock tables(Display::g_tablesMutex, std::defer_lock);
            if (g_open.load() && !tables.try_lock()) {
                ++framesSkipped;
            }
            if (tables.owns_lock()) {
                if (framesDrawn++ == 0) {
                    logger::info("overlay: first panel frame drawn");
                }
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                DrainInput();
                ImGui::NewFrame();
                DrawPanel();
                ImGui::Render();
                if (auto& ctx = *ImGui::GetCurrentContext(); Slots::g_verboseLog) {
                    ctx.DebugLogFlags |= ImGuiDebugLogFlags_EventPopup | ImGuiDebugLogFlags_EventFocus;
                    if (!ctx.DebugLogBuf.empty()) {
                        logger::info("imgui: {}", ctx.DebugLogBuf.c_str());
                        ctx.DebugLogBuf.clear();
                        ctx.DebugLogIndex.clear();
                    }
                }
                tables.unlock();
                g_wantMouse.store(ImGui::GetIO().WantCaptureMouse);
            }
            if (auto* draw = ImGui::GetDrawData(); g_open.load() && framesDrawn > 0 && draw && draw->Valid) {
                ID3D11RenderTargetView* rtv = nullptr;
                bool ownRtv = false;
                if (auto* rd = RE::BSGraphics::RendererData::GetSingleton(); rd && rd->renderWindow[0].swapChainRenderTarget.rtView) {
                    rtv = reinterpret_cast<ID3D11RenderTargetView*>(rd->renderWindow[0].swapChainRenderTarget.rtView);
                } else if (ID3D11Texture2D* back = nullptr; SUCCEEDED(a_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                    ownRtv = SUCCEEDED(g_device->CreateRenderTargetView(back, nullptr, &rtv));
                    back->Release();
                }
                if (rtv) {
                    ID3D11RenderTargetView* prevRTV = nullptr;
                    ID3D11DepthStencilView* prevDSV = nullptr;
                    g_context->OMGetRenderTargets(1, &prevRTV, &prevDSV);
                    g_context->OMSetRenderTargets(1, &rtv, nullptr);
                    ImGui_ImplDX11_RenderDrawData(draw);
                    g_context->OMSetRenderTargets(1, &prevRTV, prevDSV);
                    if (prevRTV) {
                        prevRTV->Release();
                    }
                    if (prevDSV) {
                        prevDSV->Release();
                    }
                    if (ownRtv) {
                        rtv->Release();
                    }
                }
            }
        }
        if (!g_open.load()) {
            g_needCapture.store(true);
            if (g_lastW > 0.0f) {
                const float x = g_lastX, y = g_lastY, w = g_lastW, h = g_lastH;
                Display::Schedule([x, y, w, h]() {
                    std::lock_guard lock(Display::g_tablesMutex);
                    Slots::g_panelX = x;
                    Slots::g_panelY = y;
                    Slots::g_panelW = w;
                    Slots::g_panelH = h;
                    Slots::Save();
                }, 0);
                VF_VLOG("panel geometry saved: {:.0f},{:.0f} {:.0f}x{:.0f}",
                    g_lastX, g_lastY, g_lastW, g_lastH);
                g_lastW = 0.0f;
            }
        }
        return g_origPresent(a_chain, a_sync, a_flags);
    }

    void Install() {
        auto* rd = RE::BSGraphics::RendererData::GetSingleton();
        if (!rd || !rd->renderWindow[0].swapChain) {
            logger::error("overlay: renderer/swapchain not ready - overlay disabled");
            return;
        }
        auto* chain = reinterpret_cast<IDXGISwapChain*>(rd->renderWindow[0].swapChain);
        g_gameChain = chain;
        void** vtbl = *reinterpret_cast<void***>(chain);
        void* presentAddr = vtbl[8];

        if (const auto st = MH_Initialize(); st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("overlay: MinHook init failed - overlay disabled");
            return;
        }
        if (MH_CreateHook(presentAddr, reinterpret_cast<void*>(&HookedPresent),
                reinterpret_cast<void**>(&g_origPresent)) == MH_OK &&
            MH_EnableHook(presentAddr) == MH_OK) {
            logger::info("overlay: Present hooked at {}", fmt::ptr(presentAddr));
        } else {
            logger::error("overlay: Present hook failed - overlay disabled");
            return;
        }
        if (MH_CreateHookApi(L"user32", "SetCursorPos", reinterpret_cast<void*>(&HookedSetCursorPos),
                reinterpret_cast<void**>(&g_origSetCursorPos)) == MH_OK &&
            MH_CreateHookApi(L"user32", "ClipCursor", reinterpret_cast<void*>(&HookedClipCursor),
                reinterpret_cast<void**>(&g_origClipCursor)) == MH_OK &&
            MH_EnableHook(MH_ALL_HOOKS) == MH_OK) {
            logger::info("overlay: cursor cage hooks armed (SetCursorPos/ClipCursor)");
        } else {
            logger::warn("overlay: cursor cage hooks failed - KB/M cursor may fight the panel");
        }
    }
}
