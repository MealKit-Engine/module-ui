#include "mealkit/editor/UiPanel.h"

#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <filesystem>
#include <imgui.h>

namespace mealkit::editor {

void UiPanel::refresh() {
    m_files.clear();
    const std::string dir = findAsset("assets/ui");
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
    const std::filesystem::path root(dir);
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ui") continue;
        const std::u8string rel = std::filesystem::relative(it->path(), root, ec).generic_u8string();
        if (rel == u8"theme.ui") continue; // tokens, not a screen
        m_files.push_back("ui/" + std::string(rel.begin(), rel.end()));
    }
    std::sort(m_files.begin(), m_files.end());
}

void UiPanel::draw(ui::System* gameUi) {
    ImGui::BeginGroup();
    if (!gameUi) {
        ImGui::TextUnformatted(tr("Game screens are edited in the MkUI Designer."));
        ImGui::TextDisabled("%s", tr("Saved files reload in the game while it runs."));
        auto open = [&](const std::string& file) {
            if (m_open && m_open(file)) m_status.clear();
            else m_status = tr("MkUI is not installed: add its build folder in [Settings] > Tools.");
        };
        if (ImGui::Button(tr("Open MkUI"))) open({});
        if (!m_status.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", m_status.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (m_files.empty()) ImGui::TextDisabled("%s", tr("No screens in assets/ui yet (MkUI: File > New screen)"));
        for (const std::string& f : m_files) {
            ImGui::PushID(f.c_str());
            if (ImGui::SmallButton(tr("Open in MkUI"))) open(f);
            ImGui::SameLine();
            ImGui::TextUnformatted(f.c_str() + 3); // without "ui/"
            ImGui::PopID();
        }
        if (!m_penpotUrl.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", tr("Penpot plugin (import from Penpot):"));
            ImGui::SameLine();
            ImGui::TextUnformatted(m_penpotUrl.c_str());
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(m_penpotUrl.c_str());
            ImGui::SetItemTooltip("%s", tr("Click to copy. In Penpot: Plugins > Install, paste this address"));
        }
        ImGui::EndGroup();
        return;
    }
    // Playing: what the game shows now, read-only (the Designer's Values > From a running game takes the same values)
    ImGui::TextDisabled("%s", tr("The game's open screens and their values:"));
    bool any = false;
    for (const ui::ScreenId id : gameUi->screens()) {
        const ui::ScreenInfo info = gameUi->info(id);
        const reflect::View vm = gameUi->viewModel(id);
        any = true;
        ImGui::PushID(static_cast<int>(id));
        const std::string title = info.path + (vm.type ? std::string("  (") + vm.type->name + ")" : std::string());
        if (sectionHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const std::string& e : info.errors) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", e.c_str());
            if (vm.type) drawFieldsReadOnly(*vm.type, vm.data);
            if (m_open && ImGui::SmallButton(tr("Open in MkUI"))) m_open(info.path);
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("%s", tr("(no screens open)"));
    ImGui::EndGroup();
}

void UiPanel::collectBindingProblems(ui::System* gameUi, std::vector<std::string>& out) {
    if (!gameUi) return;
    for (const ui::ScreenId id : gameUi->screens())
        for (const std::string& e : gameUi->info(id).errors) out.push_back(gameUi->info(id).path + ": " + e);
}

} // namespace mealkit::editor
