#include "InputHandler.h"

#include "DisplayManager.h"

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

//============= Engine Input Handler =============
namespace EngineInput
{
    bool g_keyDown[256] = {};
    std::uint32_t g_padMask = 0;

    void ClearAll() {
        std::memset(g_keyDown, 0, sizeof(g_keyDown));
        g_padMask = 0;
    }

    class HotkeySource final : public RE::PlayerInputHandler
    {
    public:
        using RE::PlayerInputHandler::PlayerInputHandler;

        bool ShouldHandleEvent(const RE::InputEvent*) override { return true; }

        void OnButtonEvent(const RE::ButtonEvent* event) override {
            if (!event) {
                return;
            }
            const bool down = event->QPressed();
            const auto code = event->QIDCode();
            switch (event->device.get()) {
            case RE::INPUT_DEVICE::kKeyboard:
                if (code < 256) {
                    g_keyDown[code] = down;
                    g_keyDown[VK_SHIFT] = g_keyDown[VK_LSHIFT] || g_keyDown[VK_RSHIFT];
                    g_keyDown[VK_CONTROL] = g_keyDown[VK_LCONTROL] || g_keyDown[VK_RCONTROL];
                    g_keyDown[VK_MENU] = g_keyDown[VK_LMENU] || g_keyDown[VK_RMENU];
                }
                break;
            case RE::INPUT_DEVICE::kGamepad:
                if (code != 0 && code <= 0xFFFF && (code & (code - 1)) == 0) {
                    g_padMask = down ? (g_padMask | code) : (g_padMask & ~code);
                }
                break;
            default:
                return;
            }
            Display::OnButtonInput();
        }
    };

    void Install() {
        auto* controls = RE::PlayerControls::GetSingleton();
        if (!controls) {
            logger::error("PlayerControls singleton missing - hotkeys stay dead this session");
            return;
        }
        static HotkeySource sourceCache{ controls->data };
        controls->RegisterHandler(static_cast<RE::PlayerInputHandler*>(&sourceCache));
        logger::info("engine input handler registered - hotkeys are engine-fed");
    }
}
