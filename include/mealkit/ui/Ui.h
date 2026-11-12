#pragma once

#include "mealkit/render/Fwd.h"
#include "mealkit/core/Math.h"
#include "mealkit/core/Reflect.h"
#include "mealkit/core/Types.h"

#include "mkui/Ui.h"

#include <functional>
#include <string>
#include <vector>

namespace mealkit {
class Input;
namespace live {
class Server;
} // namespace live
namespace loc {
class Strings;
}
namespace input {
class Actions;
}
} // namespace mealkit

// Game screen UI (ROADMAP stage 1 "Game screen UI", docs/CODING_MODEL.md "How to build game UI" / "Connecting game code and UI").
// Glue over the mkui runtime (engine/libs/mkui, no engine headers): it reads assets through findAsset, string tables, Reflect view models,
// input actions, and draws mkui's quads with render::Renderer.
//   Screen = one assets/ui/*.ui (mkui/Document.h). The engine lays it out in retained mode and draws it in one 2D pass after 3D.
//   The connection to game code is only a one-way view model + actions:
//     MEALKIT_VIEWMODEL(HudVm) { MEALKIT_FIELD(u32, score, 0, "Score"); };
//     m_hud = ctx.ui.open("ui/hud.ui");  ctx.ui.bind(m_hud, m_hudVm);   // once
//     fillHud(world, m_hudVm);                                          // every frame: state → view model
//     for (const ui::Action& a : ctx.ui.actions()) onUiAction(a);       // actions fired by buttons/input widgets
//   UI doesn't modify simulation state. Only input widgets (toggles/sliders/selects) write values to their own view model fields and fire actions.
//   Coordinates are logical units of the board (reference 1920×1080), scale = min(width/board width, height/board height) × UI scale.
//   Rereads when files (.ui, theme, fonts, images) change. Controls are the mouse plus input actions ui.up/down/left/right/confirm/back (keyboard/gamepad).
//   Text input (textInput) writes keyboard-typed text (Input::text) into a string field and fires an action on Enter (the game doesn't receive the keyboard while typing).
namespace mealkit::ui {

using ScreenId = mkui::ScreenId;
using mkui::kNoScreen;
using Action = mkui::Action;
using ScreenInfo = mkui::ScreenInfo; // used by the preview panel and checks
using NodeBox = mkui::NodeBox;

struct FrameInput {
    const Input* input = nullptr;          // mouse (no mouse if absent)
    const input::Actions* actions = nullptr; // ui.* actions (no keyboard/gamepad controls if absent)
    bool mouseAllowed = true, keyboardAllowed = true;
};

class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Renderer (where to upload atlases; layout only if absent), string table (text keys)
    void init(render::Renderer* renderer, const loc::Strings* strings);

    // Open/close screens. Screens with a larger layer draw on top and receive input first
    ScreenId open(const std::string& uiPath, i32 layer = 0);
    // Template for world-space UI (one name tag): not drawn as a screen, only via drawAt, receives no input
    ScreenId openWorld(const std::string& uiPath);
    void close(ScreenId screen);
    bool isOpen(ScreenId screen) const;
    void closeAll();
    // Bind a view model (MEALKIT_VIEWMODEL value, must live while the screen is open). Input widgets write values into it
    void bind(ScreenId screen, reflect::View viewModel);
    template <class Vm>
    void bind(ScreenId screen, Vm& viewModel) {
        bind(screen, reflect::View{&reflect::typeOf<Vm>(), &viewModel});
    }
    void setScale(f32 uiScale); // UI scale from settings (default 1)
    f32 scale() const;

    // The engine every frame: input → state/actions (before game update). viewport is the screen area (pixels) to draw UI in
    void update(const FrameInput& in, const render::Viewport& viewport, f64 dt);
    // Actions fired this frame (until the next update)
    const std::vector<Action>& actions() const;
    // The mouse is over the UI / a widget has focus (the game doesn't receive that input)
    bool wantsMouse() const;
    bool wantsKeyboard() const;

    // World-space UI (name tags, damage numbers): draws one template screen at a 3D position this frame (the game, in buildFrame).
    // The engine passes that frame's camera via setCamera after buildFrame, and render moves it to the screen position
    void setCamera(const Mat4& viewProj);
    void drawAt(ScreenId templateScreen, Vec3 worldPosition, reflect::View viewModel, bool clampToEdges = false);
    template <class Vm>
    void drawAt(ScreenId templateScreen, Vec3 worldPosition, const Vm& viewModel, bool clampToEdges = false) {
        drawAt(templateScreen, worldPosition, reflect::View{&reflect::typeOf<Vm>(), &viewModel}, clampToEdges);
    }

    // Lays out and draws (after 3D, before debug UI). Inside a renderer frame
    void render(render::Renderer& renderer);

    // Lays out only without drawing (tests, layout comparison checks). render uses the same layout
    void layout(const render::Viewport& viewport);

    // Diagnostics (editor [UI] panel, layout comparison checks)
    ScreenInfo info(ScreenId screen) const;
    std::vector<NodeBox> boxes(ScreenId screen) const;
    void setDraft(ScreenId screen, const std::string* text); // editor: unsaved .ui text instead of the file (nullptr = the file)
    void setPreviewState(ScreenId screen, i32 node, mkui::State state); // editor: a Document node shows this state (node < 0: none)
    void moveFocus(ScreenId screen, int direction); // editor: one ui.* move (0 up, 1 down, 2 left, 3 right)
    void setFocusVisible(bool always); // preview: always show the focus indicator
    u32 quadCount() const;             // rectangle count of the last render
    // Editor preview: string table text lengthened by about 40%, accented and bracketed ("[Sett~ ~~]" style) to find text that won't fit
    // translations (pseudo-localization). Rereads the open screens
    void setPseudoText(bool on);
    bool pseudoText() const;
    std::vector<ScreenId> screens() const;          // open screens (editor: the game's screens while playing)
    reflect::View viewModel(ScreenId screen) const; // the view model bound to a screen (empty if none)
    // For widget interaction tests: as if pressed, by node name (automated tests/preview)
    bool press(ScreenId screen, const std::string& nodeName);
    // Finds and presses a widget with this name from the top of the open screens (automated tests --ui-press)
    bool pressAny(const std::string& nodeName);
    // Starts typing in a text input field (chat key etc.). false if that node isn't a text input
    bool focusText(ScreenId screen, const std::string& nodeName);
    // Typing in some screen (the game doesn't receive the keyboard)
    bool editingText() const;

private:
    struct Impl;
    Impl* m;
};

// Live connection ui/ (MkUI Designer takes a running game's real view-model values): GET ui/screens lists open screens with the
// bound view model type, GET ui/values?screen=ui/menu.ui gives that screen's view model as "path value" pairs (reflect::toText).
// system() may return nullptr (editor not playing: 503)
void addLiveService(live::Server& server, std::function<System*()> system);

} // namespace mealkit::ui
