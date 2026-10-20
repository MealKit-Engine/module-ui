#include "mealkit/editor/UiCheck.h"

#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Document.h"
#include "mealkit/ui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace mealkit::editor {
namespace {

struct PenpotBox {
    std::string id, name;
    f32 x = 0, y = 0, w = 0, h = 0;
};

std::vector<PenpotBox> readBoxes(const std::string& path) {
    std::vector<PenpotBox> out;
    std::string text;
    if (!readTextFile(path, text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> t;
        for (size_t a = 0; a < line.size();) {
            size_t b = line.find(' ', a);
            if (b == std::string::npos) b = line.size();
            if (b > a) t.push_back(line.substr(a, b - a));
            a = b + 1;
        }
        if (t.size() < 5) continue;
        PenpotBox b;
        b.id = t[0];
        b.x = static_cast<f32>(std::atof(t[1].c_str()));
        b.y = static_cast<f32>(std::atof(t[2].c_str()));
        b.w = static_cast<f32>(std::atof(t[3].c_str()));
        b.h = static_cast<f32>(std::atof(t[4].c_str()));
        b.name = t.size() > 5 ? t[5] : "";
        out.push_back(b);
    }
    return out;
}

} // namespace

int runUiCheck(const std::string& projectDir, const std::string& language) {
    setProjectRoot(projectDir);
    loc::Strings strings;
    strings.load(language, language);
    ui::System sys;
    sys.init(nullptr, &strings);
    u32 screens = 0, failedScreens = 0;
    for (const std::string& file : listFiles(projectDir + "/assets/ui", ".ui")) {
        if (file == "theme.ui") continue;
        std::string head;
        if (!readTextFile(projectDir + "/assets/ui/" + file, head) || head.rfind("# MealKit UI exported from Penpot", 0) != 0) continue;
        const std::string screen = file.substr(0, file.size() - 3);
        const std::vector<PenpotBox> boxes = readBoxes(projectDir + "/art/ui/boxes/" + screen + ".boxes");
        if (boxes.empty()) {
            std::printf("UI-CHECK ui/%s: no art/ui/boxes/%s.boxes (export it again from Penpot)\n", file.c_str(), screen.c_str());
            ++failedScreens;
            continue;
        }
        ++screens;
        // 건너뛸 노드: 바인딩이 있는 글자, 목록 안
        ui::Document doc;
        ui::loadDocument("ui/" + file, doc);
        std::vector<std::string> skip;
        for (size_t i = 0; i < doc.nodes.size(); ++i) {
            const ui::Node& n = doc.nodes[i];
            bool inList = false;
            for (i32 p = n.parent; p >= 0; p = doc.nodes[static_cast<size_t>(p)].parent)
                if (doc.nodes[static_cast<size_t>(p)].widget == ui::WidgetKind::List) inList = true;
            const std::string text = n.textKey.empty() ? n.textContent : std::string(strings.get(n.textKey));
            if (inList || (n.kind == ui::NodeKind::Text && text.find('{') != std::string::npos)) skip.push_back(n.id);
        }
        const ui::ScreenId id = sys.open("ui/" + file);
        sys.layout({0.0f, 0.0f, doc.boardW, doc.boardH});
        const std::vector<ui::NodeBox> engine = sys.boxes(id);
        u32 compared = 0, skipped = 0, bad = 0;
        f32 worst = 0.0f;
        std::string details;
        for (const PenpotBox& p : boxes) {
            const auto e = std::find_if(engine.begin(), engine.end(), [&](const ui::NodeBox& b) { return b.id == p.id; });
            if (e == engine.end() || !e->visible || std::find(skip.begin(), skip.end(), p.id) != skip.end()) {
                ++skipped;
                continue;
            }
            const f32 diff = std::max({std::fabs(e->x - p.x), std::fabs(e->y - p.y), std::fabs(e->w - p.w), std::fabs(e->h - p.h)});
            const f32 tolerance = e->text ? kTextTolerance : kBoxTolerance;
            ++compared;
            worst = std::max(worst, diff);
            if (diff > tolerance) {
                ++bad;
                char line[320];
                std::snprintf(line, sizeof(line), "  %s %s (%s): Penpot %.1f %.1f %.1f x %.1f, engine %.1f %.1f %.1f x %.1f\n", e->text ? "text" : "box",
                              e->name.c_str(), p.id.c_str(), p.x, p.y, p.w, p.h, e->x, e->y, e->w, e->h);
                details += line;
            }
        }
        for (const std::string& err : sys.info(id).errors) details += "  error: " + err + "\n";
        const bool ok = bad == 0 && sys.info(id).errors.empty();
        std::printf("UI-CHECK ui/%s: %u compared, %u skipped, max difference %.2f -> %s\n%s", file.c_str(), compared, skipped, worst,
                    ok ? "ok" : "FAIL", details.c_str());
        if (!ok) ++failedScreens;
        sys.close(id);
    }
    if (screens == 0 && failedScreens == 0) std::printf("UI-CHECK none (no screens exported from Penpot)\n");
    std::fflush(stdout);
    return failedScreens > 0 ? 2 : 0;
}

} // namespace mealkit::editor
