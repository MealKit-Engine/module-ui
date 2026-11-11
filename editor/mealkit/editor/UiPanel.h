#pragma once

#include "mealkit/ui/Ui.h"

#include <functional>
#include <string>
#include <vector>

// Editor [UI] tab (DECISIONS 2026-11-11 "MkUI Designer is its own app"): game screens (assets/ui/*.ui) are edited in the MkUI Designer,
// a separate tool (MkUI repo tool/). Here: the project's screens with "Open in MkUI" and, while playing, the game's open screens with their
// view-model values and binding errors. The Designer takes the same values from a running game over the live connection (ui/ service).
namespace mealkit::editor {

class UiPanel {
public:
    // Opens the Designer, with one screen ("ui/menu.ui") or none (empty). false if no MkUI tool is installed
    using OpenFn = std::function<bool(const std::string& uiFile)>;
    void setOpen(OpenFn open) { m_open = std::move(open); }
    void setPenpotUrl(const std::string& url) { m_penpotUrl = url; } // Penpot plugin install address (import server), shown on the tab
    void refresh(); // reread the assets/ui file list (when a project opens, on asset changes)
    // Inside the current ImGui window. gameUi: the play session's screens (nullptr while editing)
    void draw(ui::System* gameUi);
    // Play-time binding errors (missing fields) of the game's open screens, for the editor's Problems list
    static void collectBindingProblems(ui::System* gameUi, std::vector<std::string>& out);

private:
    OpenFn m_open;
    std::vector<std::string> m_files; // "ui/menu.ui"
    std::string m_status;
    std::string m_penpotUrl;
};

} // namespace mealkit::editor
