#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Ui.h"

#include <string>
#include <vector>

// Editor [UI] panel: preview of game screens (assets/ui/*.ui). Draws the chosen screen over the scene view in edit mode.
// Change resolution (aspect), UI scale, language (to check text overflow), and put fake values into the game's view models (MEALKIT_VIEWMODEL).
// Shows file errors, binding errors (missing fields), and node count (budget 500). Editing and saving a .ui rereads it immediately.
namespace mealkit::editor {

class UiPanel {
public:
    // When a project opens: default language (language in mealkit.project)
    void setProject(const std::string& defaultLanguage);
    // View model list of the game DLL (GameModule::viewModels, nullptr before unloading the DLL)
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types);
    void draw(bool* open);
    void select(const std::string& uiFile); // automated tests: "hud.ui"
    void refresh();                         // reread the assets/ui file list
    void setPenpotUrl(const std::string& url) { m_penpotUrl = url; } // Penpot plugin install address (import server)
    // In edit mode, draws over the scene view if the panel is open (after 3D, before editor UI)
    void render(render::Renderer& renderer, const render::Viewport& sceneView, f64 dt);

private:
    void openSelected();
    void bindViewModel();

    std::vector<std::string> m_files;
    int m_selected = -1;
    ui::System m_ui;
    loc::Strings m_strings;
    bool m_ready = false;
    ui::ScreenId m_screen = ui::kNoScreen;
    int m_resolution = 0;
    f32 m_scale = 1.0f;
    std::string m_language, m_defaultLanguage = "ko";
    bool m_showFocus = false;
    const std::vector<const reflect::TypeInfo*>* m_viewModels = nullptr;
    const reflect::TypeInfo* m_vmType = nullptr;
    reflect::Instance m_vm;
    f64 m_refreshTimer = 0.0;
    std::string m_penpotUrl;
};

} // namespace mealkit::editor
