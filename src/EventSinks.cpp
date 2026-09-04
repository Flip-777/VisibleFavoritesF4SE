#include "EventSinks.h"

#include "DisplayManager.h"
#include "Hooks.h"
#include "InputHandler.h"
#include "NpcDisplay.h"
#include "Versions.h"

#include <cstring>

//============= Event Sinks =============
namespace Display
{
    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            const char* nm = event.menuName.c_str();
            if (!nm || !nm[0]) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (std::size_t i = 0; i < std::size(INPUT_CAPTURING_MENUS); ++i) {
                if (_stricmp(nm, INPUT_CAPTURING_MENUS[i]) == 0) {
                    g_menuCapState[i] = event.opening;
                    break;
                }
            }
            if (event.opening) {
                EngineInput::ClearAll();
            } else {
                if (_stricmp(nm, "ExamineMenu") == 0) {
                    RequestReconcile(300);
                    RequestReconcile(1200);
                } else if (_stricmp(nm, "PipboyMenu") == 0 || _stricmp(nm, "FavoritesMenu") == 0 ||
                           _stricmp(nm, "ContainerMenu") == 0 || _stricmp(nm, "BarterMenu") == 0) {
                    g_dirty = true;
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuSink g_menuSink;

    class FavSink final : public RE::BSTEventSink<RE::InventoryInterface::FavoriteChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::InventoryInterface::FavoriteChangedEvent&, RE::BSTEventSource<RE::InventoryInterface::FavoriteChangedEvent>*) override {
            g_dirty = true;
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    FavSink g_favSink;

    class EquipSink final : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent& event, RE::BSTEventSource<RE::TESEquipEvent>*) override {
            if (event.actor.get() == RE::PlayerCharacter::GetSingleton()) {
                g_dirty = true;
            } else {
                Npc::MarkDirty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    EquipSink g_equipSink;

    class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent& event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
            const auto isNpc = [](std::uint32_t formID) {
                if (!formID) {
                    return false;
                }
                const auto* form = RE::TESForm::GetFormByID(formID);
                return form && form->As<RE::Actor>() &&
                       form != RE::PlayerCharacter::GetSingleton();
            };
            if (isNpc(event.oldContainerFormID) || isNpc(event.newContainerFormID)) {
                Npc::MarkDirty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    ContainerSink g_containerSink;

    class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent&, RE::BSTEventSource<RE::TESDeathEvent>*) override {
            Npc::MarkDirty();
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    DeathSink g_deathSink;

    class FurnitureSink final : public RE::BSTEventSink<RE::TESFurnitureEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESFurnitureEvent& event, RE::BSTEventSource<RE::TESFurnitureEvent>*) override {
            auto* actor = event.actor.get();
            auto* furn = event.targetFurniture.get();
            if (!actor || !furn) {
                return RE::BSEventNotifyControl::kContinue;
            }
            static RE::BGSKeyword* sleepKw = RE::TESForm::GetFormByID<RE::BGSKeyword>(0x00021B18);
            const auto* base = furn->GetObjectReference();
            const auto* kwf = base ? base->As<RE::BGSKeywordForm>() : nullptr;
            if (!sleepKw || !kwf || !kwf->HasKeyword(sleepKw)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const bool enter = event.type.get() == RE::TESFurnitureEvent::FurnitureEventType::kEnter;
            if (actor == RE::PlayerCharacter::GetSingleton()) {
                g_playerInBed = enter;
                Schedule([]() { UpdateVisibilityAll(); }, 0);
            } else {
                const auto id = actor->GetFormID();
                Schedule([id, enter]() { Npc::OnFurnitureFlip(id, enter); }, 0);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    FurnitureSink g_furnitureSink;

    void RegisterSinks() {
        if (auto* ui = RE::UI::GetSingleton()) {
            static_cast<RE::BSTEventSource<RE::MenuOpenCloseEvent>*>(ui)->RegisterSink(&g_menuSink);
        }
        if (auto* inv = RE::BGSInventoryInterface::GetSingleton()) {
            static_cast<RE::BSTEventSource<RE::InventoryInterface::FavoriteChangedEvent>*>(inv)->RegisterSink(&g_favSink);
        }
        logger::info("event sinks registered - dirty-flag reconcile active");
    }

    //============= Post-Load Sink Registration =============

    class ObjectLoadedSink final : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent& event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
            const auto* form = RE::TESForm::GetFormByID(event.formID);
            if (form && form->As<RE::Actor>()) {
                Npc::MarkDirty();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    ObjectLoadedSink g_objLoadedSink;

    void RegisterEquipSinkOnce() {
        static bool done = false;
        if (done) {
            return;
        }
        done = true;
        if (auto* source = Versions::EquipEventSource()) {
            source->RegisterSink(&g_equipSink);
            logger::info("equip sink registered (source at 0x{:X})", reinterpret_cast<std::uintptr_t>(source));
        }
        if (auto* loadedSource = Versions::ObjectLoadedEventSource()) {
            loadedSource->RegisterSink(&g_objLoadedSink);
            logger::info("object-loaded sink registered (source at 0x{:X})", reinterpret_cast<std::uintptr_t>(loadedSource));
        }
        if (auto* ccSource = RE::TESContainerChangedEvent::GetEventSource()) {
            ccSource->RegisterSink(&g_containerSink);
            logger::info("container-changed sink registered");
        }
        if (auto* dSource = RE::TESDeathEvent::GetEventSource()) {
            dSource->RegisterSink(&g_deathSink);
            logger::info("death sink registered");
        }
        if (auto* fSource = RE::TESFurnitureEvent::GetEventSource()) {
            fSource->RegisterSink(&g_furnitureSink);
            logger::info("furniture sink registered");
        }
        InstallAnimSinkHook();
    }
}
