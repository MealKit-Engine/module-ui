#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Document.h"
#include "mealkit/ui/Ui.h"

#include <string>
#include <vector>

// Editor [UI] panel: preview of game screens (assets/ui/*.ui). Draws the chosen screen over the scene view in edit mode.
// Change resolution (aspect), UI scale, language (to check text overflow), and put fake values into the game's view models (MEALKIT_VIEWMODEL).
// Shows file errors, binding errors (missing fields), and node count (budget 500). Editing and saving a .ui rereads it immediately.
// [Edit screen]: WYSIWYG editing (ROADMAP "Game UI editor in the editor"). Edits the file's source (mkui/Source.h), the preview shows the unsaved text
// (ui::System::setDraft). Click the preview or the node tree to select (Ctrl+click: the box beneath), properties are its lines. Own undo history;
// the editor routes Ctrl+Z/Y/S and Delete here while editing. Penpot exports open read-only.
namespace mealkit::editor {

class UiPanel {
public:
    // When a project opens: default language (language in mealkit.project)
    void setProject(const std::string& defaultLanguage);
    // View model list of the game DLL (GameModule::viewModels, nullptr before unloading the DLL)
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types);
    void drawContent();
    void select(const std::string& uiFile); // automated tests: "hud.ui"
    void refresh();                         // reread the assets/ui file list
    void setPenpotUrl(const std::string& url) { m_penpotUrl = url; } // Penpot plugin install address (import server)
    // In edit mode, draws over the scene view if the panel is open (after 3D, before editor UI)
    void render(render::Renderer& renderer, const render::Viewport& sceneView, f64 dt);

    // Editing (the editor asks these while the panel is open in edit mode)
    bool editing() const { return m_editing && m_editable; }
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void undo();
    void redo();
    bool dirty() const { return m_editable && m_live != m_saved; }
    bool save(); // writes the screen's file; false on failure (logged)
    bool deleteSelected();
    bool duplicateSelected();
    bool copySelected(); // the node as .ui text on the clipboard
    bool paste();
    // kind: frame / text / image / use (args = component path). Inside the selected frame, else after the selected node
    bool addNew(const std::string& kind, const std::string& args = {});
    bool wrapSelected();
    // Mouse over the scene view every frame (screen pixels): press selects the topmost box (ctrl: the one beneath), dragging the
    // selection moves it (free nodes, snapping unless ctrl) or reorders it (nodes a layout places), handles resize. overUi = an editor
    // window has the mouse. true while the panel uses the mouse
    bool pointer(f32 x, f32 y, bool pressed, bool down, bool ctrl, bool overUi);
    void drawOverlay(); // selection, handles, padding, snap guides, insert line (ImGui background list, each frame)
    // Automated tests (--ui-select): edit mode on, node chosen by name
    bool selectNode(const std::string& name);
    void setPreviewState(int state) { m_previewState = state; } // --ui-state: mkui::State shown on the selected node
    // Automated tests (--ui-drag): drags node by (dx, dy) logical units after the first layout, logs the node's pos/size lines
    void queueTestDrag(const std::string& node, f32 dx, f32 dy) {
        m_testDragNode = node;
        m_testDrag[0] = dx;
        m_testDrag[1] = dy;
    }

private:
    void openSelected();
    void bindViewModel();
    std::string valuesFile(const reflect::TypeInfo& type) const; // art/ui/samples/<screen>.<type>.values
    void loadValues();  // view model with a saved values file for this screen, if there is one
    bool saveValues();
    void loadSource();          // reads the screen's file into m_source (edit history cleared)
    void syncSource();          // after edits: new text → preview draft, node map; commits history when no widget is active
    void setText(const std::string& text); // undo/redo: replace the source with this text
    void drawEditor();          // node tree + properties
    void drawTree(ui::SourceNode& node, const ui::SourcePath& path);
    void drawProperties(ui::SourceNode& node, bool placedByLayout);
    void drawSavePrompt();
    bool insertNode(ui::SourceNode node);
    enum class Drag : u8 { None, Move, Resize, Reorder };
    bool boxOf(const ui::SourcePath& path, ui::NodeBox& out) const; // first visible box of a node (last layout)
    std::string layoutOf(const ui::SourcePath& path) const;          // parent's layout if it places this node, else empty
    u32 handleAt(f32 x, f32 y) const;                                // resize handle of the selection under the mouse (edge bits)
    void startDrag(Drag kind, f32 x, f32 y);
    void dragMove(f32 dx, f32 dy, bool snap);
    void dragResize(f32 dx, f32 dy);
    void updateReorder(f32 lx, f32 ly);
    void finishReorder();
    bool dragForTest(const std::string& node, f32 dx, f32 dy);

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

    // Editing
    bool m_editing = false, m_editable = false;
    std::string m_path;           // asset path of the open screen ("ui/hud.ui")
    bool m_crlf = false;          // file used CRLF line ends (kept on save)
    ui::Source m_source;
    std::string m_live, m_committed, m_saved; // current text / last history point / file contents
    std::vector<std::string> m_undo, m_redo;
    std::vector<ui::SourcePath> m_map; // Document node → source node (preview boxes → selection)
    bool m_hasSelection = false;
    ui::SourcePath m_selection;
    std::string m_newProperty;
    int m_pendingScreen = -1; // screen to open once the save prompt is answered
    u64 m_fileTime = 0;

    // Mouse on the preview
    struct Guide {
        bool vertical;
        f32 at; // logical x (vertical) or y
    };
    struct InsertLine {
        bool valid = false;
        f32 x = 0, y = 0, w = 0, h = 0; // logical
    };
    Drag m_drag = Drag::None;
    bool m_dragArmed = false, m_dragOk = false;
    u32 m_dragHandle = 0, m_hoverHandle = 0;
    f32 m_dragStart[2] = {}, m_startBox[4] = {}, m_startPos[2] = {}, m_startSize[2] = {};
    std::vector<Guide> m_guides;
    InsertLine m_insert;
    u32 m_insertAt = 0;
    std::vector<ui::NodeBox> m_boxes; // last layout's boxes
    f32 m_originX = 0, m_originY = 0, m_scaleK = 1;
    int m_previewState = 0; // mkui::State shown on the selected node
    std::string m_testDragNode;
    f32 m_testDrag[2] = {};
};

} // namespace mealkit::editor
