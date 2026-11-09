#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Document.h"
#include "mealkit/ui/Ui.h"

#include <string>
#include <vector>

namespace mealkit {
class Input;
}

// Editor [UI] workspace (a tab beside the Viewport, DECISIONS 2026-11-09): game screens (assets/ui/*.ui) edited on a canvas of their own.
// Layout: toolbar row (edit buttons, resolution, scale, language), left column screens + node tree, middle canvas, right column properties,
// preview values, focus and problems.
// Change resolution (aspect), UI scale, language (to check text overflow), and put fake values into the game's view models (MEALKIT_VIEWMODEL).
// Shows file errors, binding errors (missing fields), and node count (budget 500). Editing and saving a .ui rereads it immediately.
// [Edit screen]: WYSIWYG editing (ROADMAP "Game UI editor in the editor"). Edits the file's source (mkui/Source.h), the preview shows the unsaved text
// (ui::System::setDraft). Click the preview or the node tree to select (Ctrl+click: the box beneath), properties are its lines. Own undo history;
// the editor routes Ctrl+Z/Y/S and Delete here while editing. Penpot exports open read-only.
namespace mealkit::editor {

void toolbarGap(f32 width); // workspace toolbar: next group on this row, or on a new one when less than width is left
// A text field on a std::string with a grey hint while empty (filters, rename). true when the text changed (or Enter with EnterReturnsTrue)
bool uiInputText(const char* id, const char* hint, std::string& text, int flags = 0);

class UiPanel {
public:
    // When a project opens: default language (language in mealkit.project)
    void setProject(const std::string& defaultLanguage);
    // View model list of the game DLL (GameModule::viewModels, nullptr before unloading the DLL)
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types);
    // The whole workspace inside the current ImGui window (the editor's [UI] window, drawn without a background: the screen is
    // rendered on the back buffer under it, see render)
    void drawWorkspace();
    bool canvasHovered() const { return m_canvasHovered; } // the mouse is on the canvas (not on a column, the toolbar or a popup)
    // Maximized (Shift+Space, toolbar button): the editor gives the workspace its whole dock area and hides the other panels
    bool maximized() const { return m_maximized; }
    void toggleMaximized() { m_maximized = !m_maximized; }
    void toggleColumns(); // Ctrl+\: hide both side columns (canvas only), or show them again
    // Canvas view (editor commands Shift+0 / Shift+1 / Shift+2 and F)
    void zoomActual();     // 100%, board centered
    void zoomFit();        // the whole board in view
    void frameSelection(); // zoom so the selected box fills most of the canvas (fit when nothing is selected)
    // View toggles (view menu, and editor commands for the palette): "layout", "safe", "rulers", "grid", "snap", "pseudo", "interact"
    void toggleView(const std::string& which);
    void capturePng() { if (m_screen && m_captureStep == 0) m_captureStep = 1; } // the board as art/ui/captures/<screen>_<size>_<lang>.png
    // Code editor from the editor's preferences (empty = VS Code), for "Open in code editor"
    void setCodeEditor(const std::string* editor) { m_codeEditor = editor; }
    // Workspace state kept across restarts in build\editor\panels.txt: screen, resolution, language, scale, columns.
    // loadState takes "ui.<key>" lines before setProject, which applies them
    void saveState(std::string& text) const;
    bool loadState(const std::string& key, const std::string& value);
    void select(const std::string& uiFile); // automated tests: "hud.ui"
    void refresh();                         // reread the assets/ui file list
    void setPenpotUrl(const std::string& url) { m_penpotUrl = url; } // Penpot plugin install address (import server)
    void setInput(const Input* input) { m_input = input; }             // window input: the canvas's interact mode presses widgets with it
    // While playing (the workspace tab can't edit): the game's open screens with their live view model values, kept as preview values
    void drawPlayValues(ui::System& gameUi);
    // Draws the backdrop over the workspace and the screen on the canvas (after 3D, before editor UI), only while the workspace is visible
    void render(render::Renderer& renderer, f64 dt);

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
    // Components (ROADMAP "UI workspace window" 4): the selected frame → assets/ui/components/<name>.ui + `use` in its place
    // (the component file is written at once, the screen edit is unsaved like any edit). editComponent opens the selected use's
    // file in the workspace with a way back to this screen (the screen shows the saved component again)
    bool makeComponent(const std::string& name);
    bool canEditComponent() const; // the selection is a use of a file in the screens list
    void editComponent();
    // Mouse over the canvas every frame (screen pixels): press selects the topmost box (ctrl: the one beneath), dragging the
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

    // Bindings of the open screen that name no field of the chosen preview view model ("screen: node: visible foo")
    void collectBindingProblems(std::vector<std::string>& out) const;

private:
    struct Size {
        f32 w, h;
    };
    Size resolution() const; // logical board size (preset or custom)
    void handleKeys();       // workspace shortcuts while it has keyboard focus (nudge, parent/child, reorder, rename)
    void nudge(f32 dx, f32 dy);
    void moveInOrder(int step); // among siblings (Alt+arrows)
    void openSelected();
    void bindViewModel();
    std::string valuesFile(const reflect::TypeInfo& type) const; // art/ui/samples/<screen>.<type>.values
    void loadValues();  // view model with a saved values file for this screen, if there is one
    bool saveValues();
    void loadSource();          // reads the screen's file into m_source (edit history cleared)
    void syncSource();          // after edits: new text → preview draft, node map; commits history when no widget is active
    void setText(const std::string& text); // undo/redo: replace the source with this text
    void drawEditToolbar();     // toolbar row: screen name, save/undo/redo, add menu, selection actions (icon buttons)
    void drawViewToggles();     // toolbar row, right end: left column, right column, maximize
    void drawColumnStrip(bool left, f32 x, f32 y, f32 height); // a hidden column's thin strip: peek it in a popup, or show it again
    void drawAddMenu();         // items of the + menu (toolbar and node context menu)
    void drawNodeMenu(const ui::SourcePath& path); // right-click on a tree row or the canvas
    void drawBreadcrumb();      // canvas top strip: path of the selection, each part selects that node
    void drawCanvasBar();       // canvas bottom strip: resolution, language, UI scale, focus, zoom
    void handleCanvasView();    // wheel zooms around the mouse, middle/right drag pans
    void drawLeftColumn();      // screens, node tree
    void drawRightColumn();     // selection properties, preview values, focus, problems, Penpot
    void drawTree(ui::SourceNode& node, const ui::SourcePath& path);
    void drawProperties(ui::SourceNode& node, bool placedByLayout);
    void drawBindings(ui::SourceNode& node);
    const reflect::TypeInfo* bindingScope(const ui::SourcePath& path) const; // view model type a node's fields are read from (element type inside a list)
    void drawSavePrompt();
    void drawMakeComponent(); // name popup for makeComponent
    void openFile(const std::string& file); // a screens list entry, asking to save first
    void drawNewScreen(); // [+] in the screens header: name → assets/ui/<name>.ui with an empty board
    void drawScreenMenu(const std::string& file);
    void openInCodeEditor(const std::string& assetPath, int line);
    int nodeLine(const ui::SourcePath& path) const; // line of the node's head in the saved file (1 when not found)
    bool treeMatches(const ui::SourceNode& node) const; // node or a descendant matches the layer filter
    void collectNodeProblems();                         // m_nodeProblems for the tree badges (bindings against the preview view model)
    // Tools (UiPanelTools.cpp)
    void refreshDraft();          // preview text: unsaved edits plus editor-only hidden nodes (nullptr when neither)
    bool editorHidden(const ui::SourcePath& path) const; // the node or an ancestor is hidden in the editor only (eye)
    bool editorLocked(const ui::SourcePath& path) const; // ... locked: the canvas doesn't pick it
    void toggleName(std::vector<std::string>& names, const std::string& name);
    void drawViewMenu();          // canvas strip: outlines, safe area, rulers, grid, snapping, overflow marks, pseudo text
    void drawCanvasGuides();      // grid, rulers, safe area, layout outlines, overflow marks, Alt distances (overlay)
    void drawFieldList();         // left column: the preview view model's fields, dragged onto a layer or the canvas to bind
    bool bindField(const ui::SourcePath& path, const std::string& field); // writes the binding that fits the field's kind
    void drawThemeSection();      // right column: theme.ui tokens with pickers and use counts
    void loadTheme();
    bool tokenCombo(ui::SourceNode& node, const char* key, bool color); // $token picker beside a property
    void copyStyle();
    void pasteStyle();
    void drawHistoryMenu();       // toolbar: undo steps, click to go back to one
    void drawChanges();           // toolbar: the unsaved lines against the file, save or revert
    void drawScreenTools();       // duplicate and rename popups for the screens list
    void startScreenRename(const std::string& file);
    void duplicateScreen(const std::string& file);
    void notice(const std::string& text); // a line in the canvas path strip for a few seconds (and the log)
    void finishCapture();         // crops the captured back buffer to the board and saves art/ui/captures/<screen>.png
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
    int m_resolution = 0;           // index in the preset list; one past the end = custom
    i32 m_customRes[2] = {1600, 900};
    f32 m_scale = 1.0f;
    std::string m_language, m_defaultLanguage = "ko";
    bool m_showFocus = false;
    const std::vector<const reflect::TypeInfo*>* m_viewModels = nullptr;
    const reflect::TypeInfo* m_vmType = nullptr;
    reflect::Instance m_vm;
    f64 m_refreshTimer = 0.0;
    std::string m_penpotUrl;
    const std::string* m_codeEditor = nullptr;
    std::string m_restoreScreen; // from panels.txt, opened by setProject
    std::string m_screenFilter, m_layerFilter, m_newScreen;
    bool m_newScreenOpen = false;
    bool m_makeComponentOpen = false;
    std::string m_componentName;
    std::string m_returnScreen;          // screen a component was opened from ("hud.ui"), for the toolbar's back button
    ui::SourcePath m_returnSelection;    // the use node there, selected again on the way back
    bool m_restoreSelection = false;
    bool m_editComponentQueued = false; // editComponent from a menu or the properties: opened by drawMakeComponent (not under the tree loop)
    // Layers: rename in place (F2, double-click), hover (canvas or tree row) outlined on the canvas, binding problems per node
    bool m_renaming = false, m_renameFocus = false;
    ui::SourcePath m_renamePath;
    std::string m_renameText;
    bool m_hasHover = false, m_treeHovered = false;
    ui::SourcePath m_hover;
    std::vector<std::pair<ui::SourcePath, std::string>> m_nodeProblems;

    // View options (canvas strip, view menu)
    bool m_showLayout = false, m_showSafeArea = false, m_showRulers = false, m_showGrid = false, m_snapGrid = false, m_showOverflow = true;
    f32 m_gridSize = 8.0f;
    // Interact mode: the canvas gets the mouse like the game, actions are listed instead of editing
    bool m_interact = false;
    const Input* m_input = nullptr;
    std::vector<std::string> m_actionLog;
    // Editor-only hide and lock, by node name (names are unique in a screen); cleared when another screen opens
    std::vector<std::string> m_hiddenNodes, m_lockedNodes;
    std::string m_draft;
    int m_openAll = 0;               // layers: 1 expands, -1 collapses every row this frame
    bool m_dragNode = false;         // a layer row is being dragged (m_dragFrom)
    ui::SourcePath m_dragFrom;
    std::string m_dragField;         // a view model field is being dragged
    // Tree edits found while drawing the tree, applied after it (the tree mustn't change under the loop)
    bool m_moveQueued = false, m_bindQueued = false;
    ui::SourcePath m_moveFrom, m_moveParent, m_bindPath;
    u32 m_moveAt = 0;
    std::string m_bindField;
    std::vector<std::string> m_styleClipboard; // style lines (fill, stroke, radius, font ...)
    std::string m_preset, m_newPreset; // preview value set ("" = the default one)
    // Theme tokens (assets/ui/theme.ui)
    struct Token {
        bool color = true;
        std::string name, value;
        u32 uses = 0;
    };
    std::vector<Token> m_tokens;
    std::string m_themeText;       // the file as read (other lines kept on save)
    bool m_themeDirty = false;
    f64 m_themeTimer = 0.0;
    // Screens: duplicate / rename
    bool m_screenRenameOpen = false;
    std::string m_screenFrom, m_screenTo;
    std::vector<std::string> m_screenRefs; // project files naming ui/<from>.ui
    // Capture, notices, stats
    int m_captureStep = 0;
    std::string m_notice;
    f64 m_noticeTime = 0.0;
    f64 m_layoutMs = 0.0;

    // Editing
    bool m_editing = true, m_editable = false; // the workspace always edits; m_editable false for Penpot exports and unreadable files
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
    render::Viewport m_area{}, m_canvas{}; // workspace content and the canvas area between its strips (screen pixels, last drawWorkspace)
    bool m_canvasHovered = false;
    f32 m_zoom = 0.0f;       // board pixels per logical unit; 0 = fit the canvas
    bool m_fitWidth = false; // fit mode fits the width only (a tall screen starts at its top)
    bool m_maximized = false, m_leftOpen = true, m_rightOpen = true;
    f32 m_pan[2] = {};       // board center offset from the canvas center (screen pixels)
    render::Viewport m_view{}; // where the board was drawn last frame (zoom around the mouse, the % shown)

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
