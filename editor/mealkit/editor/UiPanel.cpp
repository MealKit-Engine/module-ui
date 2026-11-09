#include "mealkit/editor/UiPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/FuzzyMatch.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/ui/Document.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
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
constexpr int kCustomResolution = static_cast<int>(std::size(kResolutions));
constexpr u32 kNodeBudget = 500;
constexpr text::Format kValuesFormat{"uivalues", 1};

} // namespace

UiPanel::Size UiPanel::resolution() const {
    if (m_resolution >= 0 && m_resolution < kCustomResolution) return {kResolutions[m_resolution].w, kResolutions[m_resolution].h};
    return {static_cast<f32>(std::max(m_customRes[0], 16)), static_cast<f32>(std::max(m_customRes[1], 16))};
}

void UiPanel::setProject(const std::string& defaultLanguage) {
    m_defaultLanguage = defaultLanguage.empty() ? "ko" : defaultLanguage;
    m_strings.load(m_language.empty() ? m_defaultLanguage : m_language, m_defaultLanguage); // m_language: kept from panels.txt, if any
    m_ui.closeAll();
    m_ui.setScale(m_scale);
    m_screen = ui::kNoScreen;
    m_selected = -1;
    m_ready = false;
    refresh();
    if (!m_restoreScreen.empty()) { // opened once the renderer is there (render), like select()
        for (size_t i = 0; i < m_files.size(); ++i)
            if (m_files[i] == m_restoreScreen) m_selected = static_cast<int>(i);
        m_restoreScreen.clear();
    }
}

void UiPanel::saveState(std::string& text) const {
    const auto add = [&text](const char* key, const std::string& value) { text += std::string("ui.") + key + " " + value + "\n"; };
    if (m_selected >= 0 && m_selected < static_cast<int>(m_files.size())) add("screen", m_files[static_cast<size_t>(m_selected)]);
    add("resolution", std::to_string(m_resolution));
    add("custom", std::to_string(m_customRes[0]) + " " + std::to_string(m_customRes[1]));
    if (!m_language.empty()) add("language", m_language);
    char scale[16];
    std::snprintf(scale, sizeof scale, "%.2f", static_cast<double>(m_scale));
    add("scale", scale);
    add("columns", std::to_string((m_leftOpen ? 1 : 0) | (m_rightOpen ? 2 : 0)));
}

bool UiPanel::loadState(const std::string& key, const std::string& value) {
    if (key.rfind("ui.", 0) != 0) return false;
    const std::string k = key.substr(3);
    if (k == "screen") m_restoreScreen = value;
    else if (k == "resolution") m_resolution = std::clamp(std::atoi(value.c_str()), 0, kCustomResolution);
    else if (k == "custom") {
        char* end = nullptr;
        m_customRes[0] = static_cast<i32>(std::strtol(value.c_str(), &end, 10));
        m_customRes[1] = static_cast<i32>(std::strtol(end, nullptr, 10));
    }
    else if (k == "language") m_language = value;
    else if (k == "scale") m_scale = std::clamp(static_cast<f32>(std::atof(value.c_str())), 0.5f, 2.0f);
    else if (k == "columns") {
        const int bits = std::atoi(value.c_str());
        m_leftOpen = (bits & 1) != 0;
        m_rightOpen = (bits & 2) != 0;
    } else return false;
    return true;
}

void UiPanel::zoomActual() {
    m_zoom = 1.0f;
    m_pan[0] = m_pan[1] = 0.0f;
}

void UiPanel::zoomFit() {
    m_zoom = 0.0f;
    m_fitWidth = false;
    m_pan[0] = m_pan[1] = 0.0f;
}

void UiPanel::frameSelection() {
    ui::NodeBox b;
    if (!m_hasSelection || m_selection.empty() || !boxOf(m_selection, b) || b.w <= 0.0f || b.h <= 0.0f) {
        zoomFit();
        return;
    }
    // Box at 70% of the canvas (at most 400%, so a small label doesn't fill the view), its center on the canvas center
    // (the pan is the board center's offset from it)
    const Size r = resolution();
    const f32 k = std::clamp(std::min(m_canvas.width / b.w, m_canvas.height / b.h) * 0.7f, 0.05f, 4.0f);
    m_zoom = k;
    m_pan[0] = r.w * k * 0.5f - (b.x + b.w * 0.5f) * k;
    m_pan[1] = r.h * k * 0.5f - (b.y + b.h * 0.5f) * k;
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
    return projectRoot() + "/art/ui/samples/" + screen + "." + type.name + (m_preset.empty() ? "" : "." + m_preset) + ".values";
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
    for (const std::string& f : listFiles(dir + "/components", ".ui")) m_files.push_back("components/" + f); // made with makeComponent
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
    m_hiddenNodes.clear(); // editor-only hide/lock and the value set belong to one screen
    m_lockedNodes.clear();
    m_preset.clear();
    m_actionLog.clear();
    if (m_selected >= 0 && m_selected < static_cast<int>(m_files.size())) {
        m_path = "ui/" + m_files[static_cast<size_t>(m_selected)];
        m_screen = m_ui.open(m_path);
        bindViewModel();
        loadValues();
    }
    loadSource();
    if (m_restoreSelection && ui::findNode(m_source, m_returnSelection)) { // back from a component: the use node again
        m_selection = m_returnSelection;
        m_hasSelection = true;
    }
    m_restoreSelection = false;
}

void UiPanel::bindViewModel() {
    if (m_screen) m_ui.bind(m_screen, m_vmType ? reflect::View{m_vmType, m_vm.data()} : reflect::View{});
}

bool uiInputText(const char* id, const char* hint, std::string& text, int flags) {
    return ImGui::InputTextWithHint(id, hint, text.data(), text.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize,
                                    [](ImGuiInputTextCallbackData* d) {
                                        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                                            std::string& s = *static_cast<std::string*>(d->UserData);
                                            s.resize(static_cast<size_t>(d->BufTextLen));
                                            d->Buf = s.data();
                                        }
                                        return 0;
                                    },
                                    &text);
}

// The next toolbar group on this row if `width` pixels are left, else on a new row (a narrow tab wraps instead of clipping)
void toolbarGap(f32 width) {
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.75f);
    if (ImGui::GetContentRegionAvail().x < width) {
        ImGui::NewLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
        return;
    }
    // Thin divider between groups
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const f32 h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + h * 0.2f), ImVec2(p.x, p.y + h * 0.8f), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1.0f, h));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.75f);
}

// Workspace layout, after UI Builder / Figma: a toolbar row; screens and layers on the left; the canvas in the middle with the selection's
// path above it and view settings below it; the inspector on the right. Columns are resizable shares of the width.
void UiPanel::drawWorkspace() {
    if (m_files.empty()) refresh();
    // Keys go to the workspace while it (or one of its columns) has focus and no text field is being typed in
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()) handleKeys();
    if (!m_deferred.empty()) { // menu actions from the last frame (drawNodeMenu)
        std::vector<std::function<void()>> actions;
        actions.swap(m_deferred);
        for (const std::function<void()>& a : actions) a();
    }
    collectNodeProblems();
    m_treeHovered = false;
    const ImVec2 areaPos = ImGui::GetCursorScreenPos(), areaSize = ImGui::GetContentRegionAvail();
    m_area = {areaPos.x, areaPos.y, std::max(areaSize.x, 1.0f), std::max(areaSize.y, 1.0f)};
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Toolbar row on the panel color (the window has no background: the canvas shows the back buffer)
    const f32 rowTop = areaPos.y;
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 8.0f, ImGui::GetCursorPosY() + 5.0f));
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    drawEditToolbar();
    drawViewToggles();
    ImGui::EndGroup();
    const f32 rowBottom = ImGui::GetItemRectMax().y + 5.0f;
    draw->ChannelsSetCurrent(0);
    draw->AddRectFilled(ImVec2(areaPos.x, rowTop), ImVec2(areaPos.x + areaSize.x, rowBottom), ImGui::GetColorU32(ImGuiCol_WindowBg));
    draw->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(areaPos.x, rowBottom));
    drawSavePrompt();
    drawNewScreen();
    drawMakeComponent();
    drawScreenTools();

    // Ctrl+\ and the toolbar hide the side columns (as in Figma): a hidden one leaves a thin strip that peeks it in a popup
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX |
                                  ImGuiTableFlags_NoPadInnerX;
    const f32 height = std::max(areaPos.y + areaSize.y - rowBottom, 1.0f);
    m_canvasHovered = false;
    if (ImGui::BeginTable("##uiWorkspace", 3, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupColumn("screens", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide, 0.52f);
        ImGui::TableSetupColumn("properties", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetColumnEnabled(0, m_leftOpen);
        ImGui::TableSetColumnEnabled(2, m_rightOpen);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        if (ImGui::TableSetColumnIndex(0) && m_leftOpen) {
            if (ImGui::BeginChild("##uiLeft", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding)) drawLeftColumn();
            ImGui::EndChild();
        }

        // Canvas column: hidden columns' strips at its sides; path strip, the canvas, view strip
        ImGui::TableSetColumnIndex(1);
        const f32 sideStrip = ImGui::GetFrameHeight() + 8.0f;
        const ImVec2 cellPos = ImGui::GetCursorScreenPos();
        const f32 cellWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        const f32 leftStrip = m_leftOpen ? 0.0f : sideStrip, rightStrip = m_rightOpen ? 0.0f : sideStrip;
        if (!m_leftOpen) drawColumnStrip(true, cellPos.x, cellPos.y, height);
        if (!m_rightOpen) drawColumnStrip(false, cellPos.x + cellWidth - sideStrip, cellPos.y, height);
        const ImVec2 colPos(cellPos.x + leftStrip, cellPos.y);
        const f32 colWidth = std::max(cellWidth - leftStrip - rightStrip, 1.0f);
        ImGui::SetCursorScreenPos(colPos);
        const f32 strip = ImGui::GetFrameHeight() + 8.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
        if (ImGui::BeginChild("##uiPath", ImVec2(colWidth, strip), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar)) drawBreadcrumb();
        ImGui::EndChild();
        const f32 canvasTop = colPos.y + strip, canvasHeight = std::max(height - strip * 2.0f, 1.0f);
        m_canvas = {colPos.x, canvasTop, colWidth, canvasHeight};
        m_canvasHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(ImVec2(colPos.x, canvasTop), ImVec2(colPos.x + colWidth, canvasTop + canvasHeight));
        ImGui::SetCursorScreenPos(ImVec2(colPos.x, canvasTop));
        ImGui::Dummy(ImVec2(colWidth, canvasHeight));
        // A view model field dropped on the canvas binds the topmost box under the mouse
        if (editing() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MK_UI_WIDGET"); p && m_scaleK > 0.0f) { // from the library
                const ImVec2 mouse = ImGui::GetMousePos();
                dropWidgetOnCanvas(*static_cast<const int*>(p->Data), (mouse.x - m_originX) / m_scaleK, (mouse.y - m_originY) / m_scaleK);
            }
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("MK_UI_FIELD"); p && m_scaleK > 0.0f) {
                const ImVec2 mouse = ImGui::GetMousePos();
                const f32 lx = (mouse.x - m_originX) / m_scaleK, ly = (mouse.y - m_originY) / m_scaleK;
                const ui::NodeBox* best = nullptr;
                for (const ui::NodeBox& b : m_boxes)
                    if (b.visible && b.node < m_map.size() && lx >= b.x && ly >= b.y && lx < b.x + b.w && ly < b.y + b.h && (!best || b.depth >= best->depth)) best = &b;
                if (best) {
                    m_bindQueued = true;
                    m_bindPath = m_map[best->node];
                    m_bindField = static_cast<const char*>(p->Data);
                }
            }
            ImGui::EndDragDropTarget();
        }
        drawEmptyGuide();
        if (!m_screen) {
            const char* hint = m_files.empty() ? tr("No .ui files in assets/ui.") : tr("Pick a screen on the left.");
            const ImVec2 size = ImGui::CalcTextSize(hint);
            draw->AddText(ImVec2(colPos.x + (colWidth - size.x) * 0.5f, canvasTop + (canvasHeight - size.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        }
        if (m_canvasHovered && editing() && m_hasSelection && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f))
            ImGui::OpenPopup("##canvasMenu");
        if (ImGui::BeginPopup("##canvasMenu")) {
            drawNodeMenu(m_selection);
            ImGui::EndPopup();
        }
        handleCanvasView();
        ImGui::SetCursorScreenPos(ImVec2(colPos.x, canvasTop + canvasHeight));
        if (ImGui::BeginChild("##uiView", ImVec2(colWidth, strip), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar)) drawCanvasBar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (ImGui::TableSetColumnIndex(2) && m_rightOpen) {
            if (ImGui::BeginChild("##uiRight", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding)) drawRightColumn();
            ImGui::EndChild();
        }
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    if (m_screen && m_editable) syncSource();
    if (m_bindQueued) { // a field dropped on a layer row or the canvas
        m_bindQueued = false;
        bindField(m_bindPath, m_bindField);
    }
    drawOverlay();
}

void UiPanel::toggleColumns() {
    const bool show = !m_leftOpen && !m_rightOpen; // any column showing: hide both; none: bring both back
    m_leftOpen = m_rightOpen = show;
}

// Right end of the toolbar row: the column toggles and maximize (after the edit buttons, never over them)
void UiPanel::drawViewToggles() {
    const f32 button = ImGui::GetFrameHeight(), gap = 2.0f;
    const f32 width = button * 3.0f + gap * 2.0f;
    const f32 afterLast = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetFontSize();
    ImGui::SameLine(std::max(afterLast, m_area.x + m_area.width - ImGui::GetWindowPos().x - width - 16.0f)); // the offset counts from the group start (8 px in)
    if (iconButton("\xEE\xA4\x8C", tr("Screens and layers (Ctrl+\\ hides both columns)"), m_leftOpen)) m_leftOpen = !m_leftOpen; // U+E90C dock left
    ImGui::SameLine(0.0f, gap);
    if (iconButton("\xEE\xA4\x8D", tr("Inspector (Ctrl+\\ hides both columns)"), m_rightOpen)) m_rightOpen = !m_rightOpen; // U+E90D dock right
    ImGui::SameLine(0.0f, gap);
    if (iconButton(m_maximized ? "\xEE\x9C\xBF" : "\xEE\x9D\x80", m_maximized ? tr("Restore (Shift+Space)") : tr("Maximize (Shift+Space)"), m_maximized))
        toggleMaximized(); // U+E73F back to window / U+E740 full screen
}

// A hidden column's strip: its button peeks the column in a popup beside the strip (closes on a click outside), Keep open shows it again
void UiPanel::drawColumnStrip(bool left, f32 x, f32 y, f32 height) {
    const f32 width = ImGui::GetFrameHeight() + 8.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), ImGui::GetColorU32(ImGuiCol_WindowBg));
    ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, y + 4.0f));
    ImGui::PushID(left ? "leftStrip" : "rightStrip");
    const char* popup = left ? "##peekLeft" : "##peekRight";
    if (iconButton(left ? "\xEE\xA4\x8C" : "\xEE\xA4\x8D", left ? tr("Screens and layers") : tr("Inspector"))) ImGui::OpenPopup(popup);
    const f32 peekWidth = std::max(ImGui::GetFontSize() * (left ? 18.0f : 22.0f), m_area.width * (left ? 0.2f : 0.28f));
    ImGui::SetNextWindowPos(ImVec2(left ? x + width : x - peekWidth, y));
    ImGui::SetNextWindowSize(ImVec2(peekWidth, height));
    if (ImGui::BeginPopup(popup)) {
        if (ImGui::SmallButton(tr("Keep open"))) {
            (left ? m_leftOpen : m_rightOpen) = true;
            ImGui::CloseCurrentPopup();
        }
        if (left) drawLeftColumn();
        else drawRightColumn();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void UiPanel::handleCanvasView() {
    if (!m_canvasHovered || !m_screen) return;
    ImGuiIO& io = ImGui::GetIO();
    const Size r = resolution();
    const f32 k = m_view.width > 0.0f ? m_view.width / r.w : 1.0f; // current pixels per logical unit
    if (io.MouseWheel != 0.0f) { // zoom around the mouse
        const f32 nk = std::clamp(k * std::pow(1.15f, io.MouseWheel), 0.05f, 8.0f);
        const f32 cx = m_canvas.x + m_canvas.width * 0.5f, cy = m_canvas.y + m_canvas.height * 0.5f;
        const f32 lx = (io.MousePos.x - m_view.x) / k, ly = (io.MousePos.y - m_view.y) / k; // logical point under the mouse
        const f32 w = r.w * nk, h = r.h * nk;
        // New top left keeps that point under the mouse; the pan is relative to the canvas center
        m_pan[0] = (io.MousePos.x - lx * nk) + w * 0.5f - cx;
        m_pan[1] = (io.MousePos.y - ly * nk) + h * 0.5f - cy;
        m_zoom = nk;
    }
    for (ImGuiMouseButton b : {ImGuiMouseButton_Middle, ImGuiMouseButton_Right})
        if (ImGui::IsMouseDragging(b, 2.0f)) {
            if (m_zoom <= 0.0f) m_zoom = k; // leave fit mode where it is
            m_pan[0] += io.MouseDelta.x;
            m_pan[1] += io.MouseDelta.y;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
}

void UiPanel::drawBreadcrumb() {
    if (!m_screen) return;
    ImGui::AlignTextToFramePadding();
    if (!m_notice.empty() && ImGui::GetTime() - m_noticeTime < 4.0) { // a recent notice (saved, reloaded, bound ...)
        ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", m_notice.c_str());
        return;
    }
    if (m_interact) { // interact mode: the actions the screen sent, newest last
        std::string log;
        for (const std::string& a : m_actionLog) log += (log.empty() ? "" : ",  ") + a;
        ImGui::TextDisabled("%s", log.empty() ? tr("Interact: click the screen like a player. Actions it sends appear here.") : trf("Actions: %s", log.c_str()).c_str());
        return;
    }
    if (!m_hasSelection) {
        ImGui::TextDisabled("%s", tr("Click a box to select it, Ctrl+click for the one beneath. Wheel zooms, middle or right drag moves the view."));
        return;
    }
    // board > frame main > text title: each part selects that node
    ui::SourcePath path;
    for (size_t depth = 0; depth <= m_selection.size(); ++depth) {
        if (depth > 0) path.push_back(m_selection[depth - 1]);
        const ui::SourceNode* n = ui::findNode(m_source, path);
        if (!n) break;
        if (depth > 0) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled(">");
            ImGui::SameLine(0.0f, 4.0f);
        }
        ImGui::PushID(static_cast<int>(depth));
        const bool last = depth == m_selection.size();
        if (!last) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::SmallButton(n->name.empty() ? n->kind.c_str() : n->name.c_str())) m_selection = path;
        if (!last) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void UiPanel::drawCanvasBar() {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(font * 9.0f);
    const bool custom = m_resolution == kCustomResolution;
    if (beginCombo("##resolution", custom ? tr("Custom") : kResolutions[m_resolution].label)) {
        for (int i = 0; i <= kCustomResolution; ++i)
            if (ImGui::Selectable(i == kCustomResolution ? tr("Custom") : kResolutions[i].label, m_resolution == i)) {
                m_resolution = i;
                zoomFit(); // a new size starts fitted
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Resolution"));
    if (custom) { // width and height typed or dragged
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 6.5f);
        if (ImGui::DragInt2("##customRes", m_customRes, 1.0f, 16, 8192, "%d", ImGuiSliderFlags_AlwaysClamp)) zoomFit();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Width and height (drag or double-click to type)"));
    }
    const std::vector<std::string> languages = m_strings.languages();
    if (!languages.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 3.5f);
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
    ImGui::SetNextItemWidth(font * 4.5f);
    if (ImGui::DragFloat("##uiScale", &m_scale, 0.01f, 0.5f, 2.0f, "UI x%.2f", ImGuiSliderFlags_AlwaysClamp)) m_ui.setScale(m_scale);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("UI scale (players' setting): drag or double-click to type"));
    ImGui::SameLine();
    if (iconButton(icon::kFocus, tr("Show focus"), m_showFocus)) {
        m_showFocus = !m_showFocus;
        m_ui.setFocusVisible(m_showFocus);
    }
    ImGui::SameLine();
    const bool guides = m_showLayout || m_showSafeArea || m_showRulers || m_showGrid || m_ui.pseudoText();
    if (iconButton("\xEE\xA2\x90", tr("View: outlines, safe area, rulers, grid, pseudo-localization"), guides)) ImGui::OpenPopup("##viewMenu"); // U+E890 view
    if (ImGui::BeginPopup("##viewMenu")) {
        drawViewMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_screen || m_captureStep != 0);
    if (iconButton("\xEE\x9C\xA2", tr("Save the screen as a PNG (art/ui/captures)"))) m_captureStep = 1; // U+E722 camera
    ImGui::EndDisabled();

    // Zoom at the right end: - 75% + Fit
    const f32 k = m_view.width > 0.0f ? m_view.width / resolution().w : 1.0f;
    char percent[16];
    std::snprintf(percent, sizeof percent, "%.0f%%", k * 100.0f);
    const f32 button = ImGui::GetFrameHeight();
    const f32 zoomWidth = button * 4.0f + ImGui::CalcTextSize("000%").x + ImGui::GetStyle().ItemSpacing.x * 4.0f;
    const f32 afterLast = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetFontSize(); // never over the items before it
    ImGui::SameLine(std::max(afterLast, ImGui::GetWindowWidth() - zoomWidth - 8.0f));
    if (iconButton("\xEE\x9C\x9F", tr("Zoom out"))) m_zoom = std::max(0.05f, k / 1.25f); // U+E71F
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(percent);
    if (ImGui::IsItemClicked()) m_zoom = 1.0f; // 100%
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Click for 100%"));
    ImGui::SameLine();
    if (iconButton("\xEE\xA2\xA3", tr("Zoom in"))) m_zoom = std::min(8.0f, k * 1.25f); // U+E8A3
    ImGui::SameLine();
    if (iconButton("\xEE\xA6\xA6", tr("Fit (whole screen in view)"), m_zoom <= 0.0f && !m_fitWidth)) { // U+E9A6
        m_zoom = 0.0f;
        m_fitWidth = false;
        m_pan[0] = m_pan[1] = 0.0f;
    }
    ImGui::SameLine();
    if (iconButton("\xE2\x86\x94", tr("Fit width"), m_zoom <= 0.0f && m_fitWidth)) { // U+2194
        m_zoom = 0.0f;
        m_fitWidth = true;
        m_pan[0] = m_pan[1] = 0.0f;
    }
}

void UiPanel::drawLeftColumn() {
    // Header: "Screens" with a new-screen button at its end
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Screens"));
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight()));
    if (iconButton(icon::kAdd, tr("New screen (assets/ui/<name>.ui)"))) {
        m_newScreen.clear();
        m_newScreenOpen = true;
    }
    // Filter once there are more screens than fit (FuseTag has dozens)
    if (m_files.size() > 8 || !m_screenFilter.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        uiInputText("##screenFilter", tr("Filter screens"), m_screenFilter);
    }
    // Drag its bottom edge to see more screens at once. The list rereads assets/ui every second. Right-click: open, reveal, copy path
    if (ImGui::BeginChild("##screens", ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * std::min<f32>(6.0f, static_cast<f32>(m_files.size()) + 0.5f)),
                          ImGuiChildFlags_ResizeY | ImGuiChildFlags_FrameStyle)) {
        for (size_t i = 0; i < m_files.size(); ++i) {
            const bool current = m_selected == static_cast<int>(i);
            std::string label = m_files[i];
            if (label.size() > 3 && label.compare(label.size() - 3, 3, ".ui") == 0) label.resize(label.size() - 3);
            if (!current && fuzzyScore(label, m_screenFilter) < 0) continue;
            if (current && dirty()) label += "  *";
            if (ImGui::Selectable((label + "##" + m_files[i]).c_str(), current) && !current) {
                m_returnScreen.clear(); // picked by hand: no way back
                if (dirty()) {
                    m_pendingScreen = static_cast<int>(i); // ask first (drawSavePrompt)
                } else {
                    m_selected = static_cast<int>(i);
                    zoomFit();
                    openSelected();
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                drawScreenMenu(m_files[i]);
                ImGui::EndPopup();
            }
        }
    }
    ImGui::EndChild();
    if (!m_screen) return;
    drawLibrary();      // ready-made widgets
    drawFieldList(); // the preview view model's fields, to drag onto layers
    ImGui::SeparatorText(tr("Layers"));
    // Filter, then expand all / collapse all at its end
    const f32 button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(-(button + 2.0f) * 2.0f);
    uiInputText("##layerFilter", tr("Find layer (name or kind)"), m_layerFilter);
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("\xEE\x9C\x8D", tr("Expand all"))) m_openAll = 1; // U+E70D chevron down
    ImGui::SameLine(0.0f, 2.0f);
    if (iconButton("\xEE\x9C\x8E", tr("Collapse all"))) m_openAll = -1; // U+E70E chevron up
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, ImGui::GetFontSize() * 0.9f);
    if (ImGui::BeginChild("##uiTree", ImVec2(-1.0f, -1.0f)))
        if (ui::SourceNode* board = ui::findNode(m_source, {})) drawTree(*board, {});
    ImGui::EndChild();
    ImGui::PopStyleVar();
    m_openAll = 0;
    if (!ImGui::GetDragDropPayload()) m_dragNode = false;
    // Edits found while drawing the tree (a row dropped on another, a field dropped on a row)
    if (m_moveQueued) {
        m_moveQueued = false;
        ui::SourcePath moved;
        if (ui::moveNode(m_source, m_moveFrom, m_moveParent, m_moveAt, &moved)) {
            m_selection = moved;
            m_hasSelection = true;
            syncSource();
        }
    }
}

// New screen: a name (letters, digits, _ -) → assets/ui/<name>.ui with an empty 1920x1080 board, then opened
void UiPanel::drawNewScreen() {
    if (m_newScreenOpen) {
        ImGui::OpenPopup("##newScreen");
        m_newScreenOpen = false;
    }
    if (!ImGui::BeginPopup("##newScreen")) return;
    ImGui::TextUnformatted(tr("New screen"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    const bool enter = uiInputText("##newScreenName", tr("name (assets/ui/<name>.ui)"), m_newScreen, ImGuiInputTextFlags_EnterReturnsTrue);
    bool valid = !m_newScreen.empty();
    for (char c : m_newScreen) valid = valid && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
    const std::string dir = findAsset("assets/ui");
    const std::string file = dir + "/" + m_newScreen + ".ui";
    const bool exists = valid && !dir.empty() && fileExists(file);
    if (!m_newScreen.empty() && !valid) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("Letters, digits, _ and - only"));
    if (exists) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("A screen with this name exists"));
    ImGui::BeginDisabled(!valid || exists || dir.empty());
    if (ImGui::Button(tr("Create")) || (enter && valid && !exists && !dir.empty())) {
        const std::string text = "# MealKit UI (hand-written): " + m_newScreen + "\n@format ui 1\nboard " + m_newScreen + " 1920 1080\n";
        if (writeTextFile(file, text)) {
            MEALKIT_INFO("UI: new screen %s", file.c_str());
            if (!dirty()) select(m_newScreen + ".ui"); // with unsaved edits the new screen just appears in the list
            ImGui::CloseCurrentPopup();
        } else {
            MEALKIT_ERROR("UI: cannot write %s", file.c_str());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void UiPanel::drawScreenMenu(const std::string& file) {
    const std::string assetPath = "ui/" + file;
    if (ImGui::MenuItem(tr("Open in code editor"))) openInCodeEditor(assetPath, 1);
    if (ImGui::MenuItem(tr("Show in Explorer"))) openInExplorer(findAsset("assets/ui"));
    if (ImGui::MenuItem(tr("Copy path"))) ImGui::SetClipboardText(assetPath.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem(tr("Duplicate"))) duplicateScreen(file);
    if (ImGui::MenuItem(tr("Rename..."))) startScreenRename(file);
}

void UiPanel::openInCodeEditor(const std::string& assetPath, int line) {
    const std::string file = findAssetFile(assetPath);
    if (file.empty()) return;
    editor::openInCodeEditor(m_codeEditor ? *m_codeEditor : std::string(), file, line);
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
            ImGui::Spacing();
            ImGui::TextDisabled("%s", tr("Select a node on the canvas or in the layers."));
        }
    } else if (m_screen) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", m_source.exported ? tr("Exported from Penpot: edit it in Penpot and export again.")
                                                    : tr("The file could not be read for editing (see the log)."));
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    if (sectionHeader(tr("Preview values"))) {
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
            // Value sets: art/ui/samples/<screen>.<Type>[.<set>].values (empty, full, long names ...)
            std::vector<std::string> sets{std::string()};
            if (m_selected >= 0 && m_selected < static_cast<int>(m_files.size())) {
                std::string screen = m_files[static_cast<size_t>(m_selected)];
                screen.resize(screen.size() - 3);
                const std::string prefix = screen + "." + m_vmType->name + ".";
                for (const std::string& f : listFiles(projectRoot() + "/art/ui/samples", ".values"))
                    if (f.rfind(prefix, 0) == 0 && f.size() > prefix.size() + 7) sets.push_back(f.substr(prefix.size(), f.size() - prefix.size() - 7));
            }
            if (beginCombo(propertyRow(tr("Value set"), tr("Saved sets of preview values for this screen")).c_str(), m_preset.empty() ? tr("(default)") : m_preset.c_str())) {
                for (const std::string& set : sets)
                    if (ImGui::Selectable(set.empty() ? tr("(default)") : set.c_str(), set == m_preset)) {
                        m_preset = set;
                        m_vm = reflect::Instance(*m_vmType);
                        std::string error;
                        if (!reflect::loadTextFile(*m_vmType, m_vm.data(), valuesFile(*m_vmType), kValuesFormat, &error) && !set.empty())
                            MEALKIT_WARN("UI preview: %s", error.c_str());
                        bindViewModel();
                    }
                ImGui::EndCombo();
            }
            drawFields(*m_vmType, m_vm.data());
            if (ImGui::SmallButton(tr("Save values"))) saveValues();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tr("Kept in art/ui/samples, loaded with this screen."));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            const bool enter = uiInputText("##newSet", tr("new set name"), m_newPreset, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            bool validSet = !m_newPreset.empty();
            for (char c : m_newPreset) validSet = validSet && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-');
            ImGui::BeginDisabled(!validSet);
            if (ImGui::SmallButton(tr("Save as")) || (enter && validSet)) {
                m_preset = m_newPreset;
                m_newPreset.clear();
                if (saveValues()) notice(trf("Value set '%s' saved", m_preset.c_str()));
            }
            ImGui::EndDisabled();
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
    if (m_screen) {
        const ui::ScreenInfo info = m_ui.info(m_screen);
        // Text that doesn't fit its box (cut by maxLines/ellipsis or larger than the box) at this resolution, language and scale
        std::vector<std::string> overflow;
        for (const ui::NodeBox& b : m_boxes)
            if (b.overflow && std::find(overflow.begin(), overflow.end(), b.name) == overflow.end()) overflow.push_back(b.name.empty() ? std::string("text") : b.name);
        // Problems open by themselves when there are any
        const size_t count = info.errors.size() + overflow.size();
        const std::string title = count == 0 ? std::string(tr("Problems")) : trf("Problems (%zu)", count);
        if (sectionHeader((title + "###problems").c_str(), count == 0 ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const std::string& name : overflow) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", trf("Text doesn't fit: %s", name.c_str()).c_str());
            ImGui::TextDisabled("%s", info.exported ? tr("Exported from Penpot (do not edit by hand)") : tr("Hand-written"));
            ImGui::Text("%s", trf("Nodes: %u (budget %u)", info.nodes, kNodeBudget).c_str());
            ImGui::TextDisabled("%s", trf("%u quads, layout and drawing %.2f ms", m_ui.quadCount(), m_layoutMs).c_str());
            if (info.nodes > kNodeBudget) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", tr("Over the node budget"));
            if (count == 0) ImGui::TextDisabled("%s", tr("No problems"));
            for (const std::string& e : info.errors) ImGui::TextWrapped("%s", e.c_str());
        }
        drawThemeSection();
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
        m_ui.setScale(m_scale);
        m_ready = true;
    }
    if (m_selected >= 0 && !m_screen && m_path.empty()) openSelected(); // the screen kept from the last session (setProject)
    if (m_strings.language().empty()) m_strings.load(m_defaultLanguage, m_defaultLanguage);
    m_strings.update(dt);
    m_refreshTimer += dt;
    if (m_refreshTimer > 1.0) {
        m_refreshTimer = 0.0;
        refresh();
        // The file changed outside the editor (text editor, Penpot export) while nothing is unsaved here: read it again
        if (!m_path.empty() && !dirty())
            if (const std::string file = findAssetFile(m_path); !file.empty() && fileWriteTime(file) != m_fileTime) {
                loadSource();
                MEALKIT_INFO("UI: %s changed outside the editor, reloaded", m_path.c_str());
                notice(trf("%s changed outside the editor: reloaded", m_path.c_str()));
            }
    }
    // Theme tokens and their use counts are read again every few seconds (another screen may start using one)
    m_themeTimer += dt;
    if (m_themeTimer > 3.0 && !m_themeDirty) {
        m_themeTimer = 0.0;
        m_tokens.clear();
        m_themeText.clear();
    }
    // Capture: step 1 hid the overlay this frame, so the back buffer at this endFrame has the board alone; step 2 crops it next frame
    if (m_captureStep == 2) {
        finishCapture();
        m_captureStep = 0;
    } else if (m_captureStep == 1 && m_screen) {
        createDirectories(projectRoot() + "/build/editor");
        renderer.requestCapture((projectRoot() + "/build/editor/ui_capture.png").c_str());
        m_captureStep = 2;
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
    // Fit mode: the whole board (or its width) inside the canvas with a thin margin; above 100% only whole multiples, so text stays
    // sharp. Otherwise the chosen zoom, moved by the pan
    const Size r = resolution();
    constexpr f32 kMargin = 12.0f;
    const f32 vw = std::max(1.0f, m_canvas.width - kMargin * 2.0f), vh = std::max(1.0f, m_canvas.height - kMargin * 2.0f);
    f32 k = m_zoom;
    if (k <= 0.0f) {
        k = m_fitWidth ? vw / r.w : std::min(vw / r.w, vh / r.h);
        if (k > 1.0f) k = std::floor(k);
    }
    const f32 w = r.w * k, h = r.h * k;
    const f32 cx = m_canvas.x + m_canvas.width * 0.5f + (m_zoom > 0.0f ? m_pan[0] : 0.0f);
    f32 cy = m_canvas.y + m_canvas.height * 0.5f + (m_zoom > 0.0f ? m_pan[1] : 0.0f);
    if (m_zoom <= 0.0f && h > vh) cy = m_canvas.y + kMargin + h * 0.5f; // fit width, taller than the canvas: from its top (drag to see the rest)
    const render::Viewport view{std::round(cx - w * 0.5f), std::round(cy - h * 0.5f), w, h};
    m_view = view;
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
    // Interact mode: the mouse goes to the screen like in the game (hover, press, sliders); the actions are listed
    ui::FrameInput input;
    if (m_interact && m_input) {
        input.input = m_input;
        input.mouseAllowed = m_canvasHovered;
        input.keyboardAllowed = false;
    }
    const auto start = std::chrono::steady_clock::now();
    m_ui.update(input, view, dt);
    m_ui.render(renderer);
    m_layoutMs = m_layoutMs * 0.9 + std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count() * 0.1;
    if (m_interact)
        for (const ui::Action& a : m_ui.actions()) {
            m_actionLog.push_back(a.name);
            if (m_actionLog.size() > 8) m_actionLog.erase(m_actionLog.begin());
            MEALKIT_INFO("UI interact: %s", a.name.c_str());
        }
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
