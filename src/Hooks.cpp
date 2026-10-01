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

    RE::BSEventNotifyControl AnimSinkHook(void* a_this, RE::BSAnimationGraphEvent* a_event, void* a_source) {
        const char* tag = a_event->tag.c_str();
        if (_stricmp(tag, "weaponDraw") == 0) {
            Schedule([]() { OnPlayerDrawFlip(true); }, 0);
        } else if (_stricmp(tag, "weaponSheathe") == 0) {
            Schedule([]() { OnPlayerDrawFlip(false); }, 0);
        }
        return g_animSinkOrig(a_this, a_event, a_source);
    }

    RE::BSEventNotifyControl NpcAnimSinkHook(void* a_this, RE::BSAnimationGraphEvent* a_event, void* a_source) {
        const char* tag = a_event->tag.c_str();
        const bool draw = _stricmp(tag, "weaponDraw") == 0;
        if (draw || _stricmp(tag, "weaponSheathe") == 0) {
            auto* sink = static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(a_this);
            const auto id = static_cast<RE::TESObjectREFR*>(sink)->GetFormID();
            Schedule([id, draw]() { Npc::OnDrawFlip(id, draw); }, 0);
        }
        return g_npcAnimSinkOrig(a_this, a_event, a_source);
    }

    void InstallAnimSinkHook() {
        REL::Relocation<std::uintptr_t> pcVtbl{ RE::VTABLE::PlayerCharacter[3] };
        g_animSinkOrig = reinterpret_cast<AnimSinkFn>(pcVtbl.write_vfunc(1, AnimSinkHook));
        REL::Relocation<std::uintptr_t> actorVtbl{ RE::VTABLE::Actor[3] };
        g_npcAnimSinkOrig = reinterpret_cast<AnimSinkFn>(actorVtbl.write_vfunc(1, NpcAnimSinkHook));
        logger::info("anim-event hooks installed (player + actor)");
    }
}

//============= Cursor Cage (KB/M) =============
namespace Overlay
{
    using SetCursorPosFn = BOOL(WINAPI*)(int, int);
    SetCursorPosFn g_origSetCursorPos = nullptr;
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

    BOOL WINAPI HookedSetCursorPos(int a_x, int a_y) {
        if (g_open.load()) {
            return TRUE;
        }
        return g_origSetCursorPos(a_x, a_y);
    }

    BOOL WINAPI HookedClipCursor(const RECT* a_rect) {
        if (g_open.load()) {
            const RECT full = FullScreenClip();
            return g_origClipCursor(&full);
        }
        return g_origClipCursor(a_rect);
    }
}

//============= Window Messages -> ImGui =============
namespace Overlay
{
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
    std::atomic<WNDPROC> g_origWndProc{ nullptr };

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
}

//============= ImGui on the Renderer's Swap Chain =============
namespace Overlay
{
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    PresentFn g_origPresent = nullptr;
    void* g_presentTarget = nullptr;
    std::atomic<IDXGISwapChain*> g_chain{ nullptr };
    std::atomic<ULONGLONG> g_lastPresent{ 0 };
    std::atomic<ID3D11Device*> g_device{ nullptr };
    std::atomic<HWND> g_hwnd{ nullptr };
    std::atomic<bool> g_imguiReady{ false };
    ID3D11DeviceContext* g_context = nullptr;
    bool g_initFailed = false;
    bool g_panelWasOpen = false;
    unsigned g_panelFrames = 0;
    unsigned g_panelSkips = 0;

    IDXGISwapChain* RendererChain() {
        return reinterpret_cast<IDXGISwapChain*>(RE::BSGraphics::RendererData::GetSingleton()->renderWindow[0].swapChain);
    }

    void* PresentOf(IDXGISwapChain* a_chain) {
        return (*reinterpret_cast<void***>(a_chain))[8];
    }

    std::string PresentStatus() {
        auto* rd = RE::BSGraphics::RendererData::GetSingleton();
        auto* chain = RendererChain();
        void* present = PresentOf(chain);
        const auto last = g_lastPresent.load();
        return fmt::format("renderer chain {} ({}), its Present {} ({}), window {} (imgui on {}), device {} (imgui on {}), last hooked Present {}",
            fmt::ptr(chain), chain == g_chain.load() ? "adopted" : "not adopted",
            fmt::ptr(present), present == g_presentTarget ? "hooked" : "NOT hooked",
            fmt::ptr(rd->renderWindow[0].hwnd), fmt::ptr(g_hwnd.load()),
            fmt::ptr(rd->device), fmt::ptr(g_device.load()),
            last ? fmt::format("{} ms ago", GetTickCount64() - last) : "never");
    }

    void InitImGui(IDXGISwapChain* a_chain) {
        g_initFailed = true;
        auto* rd = RE::BSGraphics::RendererData::GetSingleton();
        ID3D11Device* dev = nullptr;
        if (FAILED(a_chain->GetDevice(IID_PPV_ARGS(&dev)))) {
            dev = reinterpret_cast<ID3D11Device*>(rd->device);
            dev->AddRef();
            logger::info("overlay: swap chain refused GetDevice (proxied by another mod) - using the game's device");
        }
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        DXGI_SWAP_CHAIN_DESC desc{};
        const HWND hwnd = SUCCEEDED(a_chain->GetDesc(&desc)) && desc.OutputWindow ?
                              desc.OutputWindow :
                              reinterpret_cast<HWND>(rd->renderWindow[0].hwnd);
        SetLastError(0);
        const auto prev = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));
        if (!prev && GetLastError() != 0) {
            logger::error("overlay: wndproc subclass failed (error {}) - overlay disabled", GetLastError());
            ctx->Release();
            dev->Release();
            return;
        }
        g_origWndProc.store(reinterpret_cast<WNDPROC>(prev));
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.FontGlobalScale = Slots::g_overlayScale;
        io.Fonts->AddFontDefault();
        char winDir[MAX_PATH]{};
        GetWindowsDirectoryA(winDir, MAX_PATH);
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
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplDX11_Init(dev, ctx);
        g_device.store(dev);
        g_context = ctx;
        g_hwnd.store(hwnd);
        LoadBodyTexture();
        g_initFailed = false;
        g_imguiReady.store(true);
        logger::info("overlay: imgui ready (hwnd={})", fmt::ptr(hwnd));
    }

    void BuildPanelFrame() {
        std::unique_lock tables(Display::g_tablesMutex, std::try_to_lock);
        if (!tables) {
            ++g_panelSkips;
            return;
        }
        if (g_panelFrames++ == 0) {
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
        g_wantMouse.store(ImGui::GetIO().WantCaptureMouse);
    }

    void DrawPanelFrame() {
        auto* draw = ImGui::GetDrawData();
        if (g_panelFrames == 0 || !draw || !draw->Valid) {
            return;
        }
        auto* rd = RE::BSGraphics::RendererData::GetSingleton();
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(rd->renderWindow[0].swapChainRenderTarget.rtView);
        ID3D11RenderTargetView* ownRtv = nullptr;
        if (!rtv) {
            if (ID3D11Texture2D* back = nullptr; SUCCEEDED(g_chain.load()->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                g_device.load()->CreateRenderTargetView(back, nullptr, &ownRtv);
                back->Release();
            }
            rtv = ownRtv;
        }
        if (!rtv) {
            return;
        }
        ID3D11RenderTargetView* prevRtv = nullptr;
        ID3D11DepthStencilView* prevDsv = nullptr;
        g_context->OMGetRenderTargets(1, &prevRtv, &prevDsv);
        g_context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(draw);
        g_context->OMSetRenderTargets(1, &prevRtv, prevDsv);
        if (prevRtv) {
            prevRtv->Release();
        }
        if (prevDsv) {
            prevDsv->Release();
        }
        if (ownRtv) {
            ownRtv->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_chain, UINT a_sync, UINT a_flags) {
        if (a_chain != RendererChain()) {
            return g_origPresent(a_chain, a_sync, a_flags);
        }
        g_lastPresent.store(GetTickCount64());
        if (auto* prev = g_chain.load(); a_chain != prev) {
            if (prev) {
                logger::info("overlay: the renderer moved to swap chain {} (was {})", fmt::ptr(a_chain), fmt::ptr(prev));
            }
            g_chain.store(a_chain);
            if (!g_imguiReady.load() && !g_initFailed) {
                InitImGui(a_chain);
            }
        }
        if (g_imguiReady.load()) {
            const bool open = g_open.load();
            ImGui::GetIO().MouseDrawCursor = open;
            if (open != g_panelWasOpen) {
                g_panelWasOpen = open;
                if (open) {
                    g_panelFrames = 0;
                    g_panelSkips = 0;
                    const RECT full = FullScreenClip();
                    ClipCursor(&full);
                } else {
                    logger::info("overlay: panel session ended - {} frames drawn, {} skipped (tables busy)",
                        g_panelFrames, g_panelSkips);
                }
            }
            if (open) {
                BuildPanelFrame();
                DrawPanelFrame();
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
        auto* chain = RendererChain();
        if (const auto st = MH_Initialize(); st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
            logger::error("overlay: MinHook init failed - overlay disabled");
            return;
        }
        void* target = PresentOf(chain);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            logger::error("overlay: Present hook failed - overlay disabled");
            return;
        }
        g_presentTarget = target;
        logger::info("overlay: Present hooked at {} (swap chain {})", fmt::ptr(target), fmt::ptr(chain));
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
