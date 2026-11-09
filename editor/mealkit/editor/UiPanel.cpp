#include "mealkit/editor/UiPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/ui/Document.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

namespace mealkit::editor {
namespace {

struct Resolution {
    const char* label;
    f32 w, h;
};
constexpr Resolution kResolutions[] = {
    {"1920x1080 (16:9)", 1920, 1080}, {"1280x720 (16:9)", 1280, 720}, {"2560x1080 (21:9)", 2560, 1080},
    {"1024x768 (4:3)", 1024, 768},    {"1080x1920 (9:16)", 1080, 1920},
};
constexpr u32 kNodeBudget = 500;
constexpr text::Format kValuesFormat{"uivalues", 1};

} // namespace

void UiPanel::setProject(const std::string& defaultLanguage) {
    m_defaultLanguage = defaultLanguage.empty() ? "ko" : defaultLanguage;
    m_language.clear();
    m_strings.load(m_defaultLanguage, m_defaultLanguage);
    m_ui.closeAll();
    m_screen = ui::kNoScreen;
    m_selected = -1;
    m_ready = false;
    refresh();
}

void UiPanel::setViewModels(const std::vector<const reflect::TypeInfo*>* types) {
    m_viewModels = types;
    if (!types || std::find(types->begin(), types->end(), m_vmType) == types->end()) { // the type becomes invalid when the DLL changes
        if (m_screen) m_ui.bind(m_screen, reflect::View{});
        m_vm = {};
        m_vmType = nullptr;
        if (types) loadValues();
    }
}

std::string UiPanel::valuesFile(const reflect::TypeInfo& type) const {
    if (m_selected < 0 || m_selected >= static_cast<int>(m_files.size())) return {};
    std::string screen = m_files[static_cast<size_t>(m_selected)];
    if (const size_t dot = screen.rfind('.'); dot != std::string::npos) screen.resize(dot);
    return projectRoot() + "/art/ui/samples/" + screen + "." + type.name + ".values";
}

void UiPanel::loadValues() {
    if (!m_viewModels || !m_screen) return;
    for (const reflect::TypeInfo* t : *m_viewModels) {
        const std::string file = valuesFile(*t);
        if (file.empty() || !fileExists(file)) continue;
        m_vmType = t;
        m_vm = reflect::Instance(*t);
        std::string error;
        if (reflect::loadTextFile(*t, m_vm.data(), file, kValuesFormat, &error)) MEALKIT_INFO("UI preview: values from %s", file.c_str());
        else MEALKIT_WARN("UI preview: %s: %s", file.c_str(), error.c_str());
        bindViewModel();
        return;
    }
}

bool UiPanel::saveValues() {
    if (!m_vmType) return false;
    const std::string file = valuesFile(*m_vmType);
    if (file.empty()) return false;
    createDirectories(projectRoot() + "/art/ui/samples");
    return reflect::saveTextFile(*m_vmType, m_vm.data(), file, kValuesFormat, "# Preview values for the editor [UI] panel (not shipped)");
}

void UiPanel::refresh() {
    m_files.clear();
    const std::string dir = findAsset("assets/ui");
    if (dir.empty()) return;
    for (const std::string& f : listFiles(dir, ".ui"))
        if (std::string("ui/") + f != ui::kThemeFile) m_files.push_back(f);
}

void UiPanel::select(const std::string& uiFile) {
    refresh();
    for (size_t i = 0; i < m_files.size(); ++i)
        if (m_files[i] == uiFile) m_selected = static_cast<int>(i);
    openSelected();
}

void UiPanel::openSelected() {
    if (m_screen) m_ui.close(m_screen);
    m_screen = ui::kNoScreen;
    m_path.clear();
    if (m_selected >= 0 && m_selected < static_cast<int>(m_files.size())) {
        m_path = "ui/" + m_files[static_cast<size_t>(m_selected)];
        m_screen = m_ui.open(m_path);
        bindViewModel();
        loadValues();
    }
    loadSource();
}

void UiPanel::bindViewModel() {
    if (m_screen) m_ui.bind(m_screen, m_vmType ? reflect::View{m_vmType, m_vm.data()} : reflect::View{});
}

// The next toolbar group on this row if `width` pixels are left, else on a new row (a narrow tab wraps instead of clipping)
void toolbarGap(f32 width) {
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    if (ImGui::GetContentRegionAvail().x < width) {
        ImGui::NewLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    }
}

void UiPanel::drawWorkspace() {
    if (m_files.empty()) refresh();
    const ImVec2 areaPos = ImGui::GetCursorScreenPos(), areaSize = ImGui::GetContentRegionAvail();
    m_area = {areaPos.x, areaPos.y, std::max(areaSize.x, 1.0f), std::max(areaSize.y, 1.0f)};

    // Toolbar row: editing on the left, preview settings after it
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6.0f, ImGui::GetCursorPosY() + 4.0f));
    if (m_screen) {
        drawEditToolbar();
        toolbarGap(ImGui::GetFontSize() * 22.0f);
    }
    drawPreviewSettings();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    drawSavePrompt();

    // Three resizable columns below it; the middle one is the canvas (no background: render draws the screen under it)
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX;
    const f32 height = std::max(ImGui::GetContentRegionAvail().y, 1.0f);
    m_canvasHovered = false;
    if (ImGui::BeginTable("##uiWorkspace", 3, flags, ImVec2(0.0f, height))) {
        // Shares of the width, so the canvas keeps the middle half however wide the tab is
        ImGui::TableSetupColumn("screens", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("properties", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("##uiLeft", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding)) drawLeftColumn();
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        const f32 canvasWidth = ImGui::GetContentRegionAvail().x;
        m_canvas = {canvasPos.x, canvasPos.y, std::max(canvasWidth, 1.0f), height};
        m_canvasHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(canvasPos, ImVec2(canvasPos.x + canvasWidth, canvasPos.y + height));
        if (!m_screen) {
            ImGui::SetCursorScreenPos(ImVec2(canvasPos.x + 16.0f, canvasPos.y + 16.0f));
            ImGui::TextDisabled("%s", m_files.empty() ? tr("No .ui files in assets/ui.") : tr("Pick a screen on the left."));
        }
        ImGui::TableSetColumnIndex(2);
        if (ImGui::BeginChild("##uiRight", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding)) drawRightColumn();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    if (m_screen && m_editable) syncSource();
    drawOverlay();
}

void UiPanel::drawPreviewSettings() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(font * 9.5f);
    if (beginCombo("##resolution", kResolutions[m_resolution].label)) {
        for (int i = 0; i < static_cast<int>(std::size(kResolutions)); ++i)
            if (ImGui::Selectable(kResolutions[i].label, m_resolution == i)) m_resolution = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Resolution"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(font * 5.0f);
    if (ImGui::SliderFloat("##uiScale", &m_scale, 0.5f, 2.0f, "x%.2f")) m_ui.setScale(m_scale);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("UI scale"));
    const std::vector<std::string> languages = m_strings.languages();
    if (!languages.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 4.0f);
        if (beginCombo("##uiLanguage", m_strings.language().c_str())) {
            for (const std::string& lang : languages)
                if (ImGui::Selectable(lang.c_str(), lang == m_strings.language())) {
                    m_language = lang;
                    m_strings.load(lang, m_defaultLanguage);
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Language"));
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Show focus"), &m_showFocus)) m_ui.setFocusVisible(m_showFocus);
}

void UiPanel::drawLeftColumn() {
    ImGui::SeparatorText(tr("Screens"));
    // Drag its bottom edge to see more screens at once
    if (ImGui::BeginChild("##screens", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * std::min<f32>(6.0f, static_cast<f32>(m_files.size()) + 1.0f)),
                          ImGuiChildFlags_ResizeY | ImGuiChildFlags_FrameStyle)) {
        for (size_t i = 0; i < m_files.size(); ++i)
            if (ImGui::Selectable(m_files[i].c_str(), m_selected == static_cast<int>(i)) && m_selected != static_cast<int>(i)) {
                if (dirty()) {
                    m_pendingScreen = static_cast<int>(i); // ask first (drawSavePrompt)
                } else {
                    m_selected = static_cast<int>(i);
                    openSelected();
                }
            }
    }
    ImGui::EndChild();
    if (ImGui::SmallButton(tr("Refresh"))) refresh();
    if (!m_screen) return;
    ImGui::SeparatorText(tr("Nodes"));
    if (ImGui::BeginChild("##uiTree", ImVec2(-1.0f, -1.0f), ImGuiChildFlags_Borders))
        if (ui::SourceNode* board = ui::findNode(m_source, {})) drawTree(*board, {});
    ImGui::EndChild();
}

void UiPanel::drawRightColumn() {
    if (m_screen && m_editable) {
        if (m_hasSelection) {
            if (ui::SourceNode* n = ui::findNode(m_source, m_selection)) {
                // Inside a flex/grid layout the parent places the node (pos is ignored unless the node is absolute)
                std::string layout, unused;
                const ui::SourceNode* parent =
                    m_selection.empty() ? nullptr : ui::findNode(m_source, ui::SourcePath(m_selection.begin(), m_selection.end() - 1));
                const bool placed = parent && ui::getProperty(*parent, "layout", layout) && !ui::getProperty(*n, "absolute", unused);
                drawProperties(*n, placed);
            }
        } else {
            ImGui::SeparatorText(tr("Properties"));
            ImGui::TextDisabled("%s", tr("Select a node on the canvas or in the tree."));
        }
    }

    ImGui::Spacing();
    if (sectionHeader(tr("Preview values"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* vmName = m_vmType ? m_vmType->name : tr("(none)");
        if (beginCombo(propertyRow(tr("View model")).c_str(), vmName)) {
            if (ImGui::Selectable(tr("(none)"), !m_vmType)) {
                m_vmType = nullptr;
                m_vm = {};
                bindViewModel();
            }
            if (m_viewModels)
                for (const reflect::TypeInfo* t : *m_viewModels)
                    if (ImGui::Selectable(t->name, t == m_vmType)) {
                        m_vmType = t;
                        m_vm = reflect::Instance(*t);
                        bindViewModel();
                    }
            ImGui::EndCombo();
        }
        if (!m_viewModels) ImGui::TextDisabled("%s", tr("View models appear when the game code is loaded."));
        if (m_vmType) {
            drawFields(*m_vmType, m_vm.data());
            if (ImGui::SmallButton(tr("Save values"))) saveValues();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("Kept in art/ui/samples, loaded with this screen."));
        }
    }
    if (m_screen && sectionHeader(tr("Focus"))) {
        // Keyboard/gamepad focus: one move like the ui.* actions
        const char* const moves[] = {tr("Up"), tr("Down"), tr("Left"), tr("Right")};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::SmallButton(moves[i])) m_ui.moveFocus(m_screen, i);
        }
        const std::string focus = m_ui.info(m_screen).focus;
        ImGui::TextDisabled("%s", focus.empty() ? tr("(none)") : focus.c_str());
    }
    if (m_screen && sectionHeader(tr("Problems"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const ui::ScreenInfo info = m_ui.info(m_screen);
        ImGui::TextDisabled("%s", info.exported ? tr("Exported from Penpot (do not edit by hand)") : tr("Hand-written"));
        ImGui::Text("%s", trf("Nodes: %u (budget %u)", info.nodes, kNodeBudget).c_str());
        if (info.nodes > kNodeBudget) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("Over the node budget"));
        if (info.errors.empty()) ImGui::TextDisabled("%s", tr("No problems"));
        for (const std::string& e : info.errors) ImGui::TextWrapped("%s", e.c_str());
    }
    // Penpot plugin: paste this address into Penpot's plugin manager (Ctrl+Alt+P). Changes every time the editor starts
    if (sectionHeader("Penpot")) {
        if (m_penpotUrl.empty()) {
            ImGui::TextDisabled("%s", tr("The import server is not running (see the log)."));
        } else {
            ImGui::TextWrapped("%s", tr("Plugin address (paste it into the plugin manager in Penpot, Ctrl+Alt+P). It changes every time the editor starts."));
            ImGui::SetNextItemWidth(-60.0f);
            ImGui::InputText("##penpotUrl", m_penpotUrl.data(), m_penpotUrl.size() + 1, ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button(tr("Copy"))) ImGui::SetClipboardText(m_penpotUrl.c_str());
        }
    }
}

void UiPanel::render(render::Renderer& renderer, f64 dt) {
    if (!m_ready) {
        m_ui.init(&renderer, &m_strings);
        m_ready = true;
    }
    if (m_strings.language().empty()) m_strings.load(m_defaultLanguage, m_defaultLanguage);
    m_strings.update(dt);
    m_refreshTimer += dt;
    if (m_refreshTimer > 1.0) {
        m_refreshTimer = 0.0;
        refresh();
        // The file changed outside the editor (text editor, Penpot export) while nothing is unsaved here: read it again
        if (!m_path.empty() && !dirty())
            if (const std::string file = findAssetFile(m_path); !file.empty() && fileWriteTime(file) != m_fileTime) loadSource();
    }
    // Backdrop over the whole workspace: the 3D scene on the back buffer must not show through the canvas or between the columns
    render::UiQuad backdrop;
    backdrop.rect[0] = m_area.x;
    backdrop.rect[1] = m_area.y;
    backdrop.rect[2] = m_area.width;
    backdrop.rect[3] = m_area.height;
    backdrop.color0 = render::rgba(24, 25, 28, 255);
    renderer.drawUi(&backdrop, 1);
    if (!m_screen) return;
    // Fit inside the canvas (with a margin) at the chosen resolution's aspect, at that exact pixel size if it fits
    const Resolution& r = kResolutions[m_resolution];
    constexpr f32 kMargin = 24.0f;
    const render::Viewport sceneView{m_canvas.x + kMargin, m_canvas.y + kMargin, m_canvas.width - kMargin * 2.0f, m_canvas.height - kMargin * 2.0f};
    const f32 vw = std::max(1.0f, sceneView.width), vh = std::max(1.0f, sceneView.height);
    f32 w = r.w, h = r.h;
    if (w > vw || h > vh) {
        const f32 k = std::min(vw / w, vh / h);
        w *= k;
        h *= k;
    }
    const render::Viewport view{sceneView.x + (vw - w) * 0.5f, sceneView.y + (vh - h) * 0.5f, w, h};
    render::UiQuad frame; // preview area indicator
    frame.rect[0] = view.x;
    frame.rect[1] = view.y;
    frame.rect[2] = view.width;
    frame.rect[3] = view.height;
    frame.color0 = render::rgba(36, 37, 41, 255); // the board area, a shade lighter than the backdrop
    frame.strokeWidth = 1.0f;
    frame.strokeColor = render::rgba(255, 255, 255, 40);
    renderer.drawUi(&frame, 1);
    // State of the selected node shown as if it were real (hover, pressed ...)
    i32 previewNode = -1;
    if (editing() && m_hasSelection && m_previewState > 0)
        for (size_t i = 0; i < m_map.size() && previewNode < 0; ++i)
            if (m_map[i] == m_selection) previewNode = static_cast<i32>(i);
    m_ui.setPreviewState(m_screen, previewNode, static_cast<mkui::State>(m_previewState));
    m_ui.update({}, view, dt);
    m_ui.render(renderer);
    if (!m_testDragNode.empty()) { // --ui-drag: after the first layout
        const std::string node = m_testDragNode;
        m_testDragNode.clear();
        std::string pos = "-", size = "-";
        if (dragForTest(node, m_testDrag[0], m_testDrag[1]))
            if (const ui::SourceNode* n = ui::findNode(m_source, m_selection)) {
                ui::getProperty(*n, "pos", pos);
                ui::getProperty(*n, "size", size);
            }
        MEALKIT_INFO("UI drag test: %s pos %s size %s index %d", node.c_str(), pos.c_str(), size.c_str(),
                     m_selection.empty() ? -1 : static_cast<int>(m_selection.back()));
    }
}

} // namespace mealkit::editor
