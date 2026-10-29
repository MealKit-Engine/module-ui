#include "mealkit/editor/UiCheck.h"

#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"
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
    std::string text; // text node: Penpot's text (\n kept as two characters)
    bool hasText = false;
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
        PenpotBox b;
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            b.text = line.substr(tab + 1);
            b.hasText = true;
            line.resize(tab);
        }
        std::vector<std::string> t;
        for (size_t from = 0; from < line.size();) {
            size_t to = line.find(' ', from);
            if (to == std::string::npos) to = line.size();
            if (to > from) t.push_back(line.substr(from, to - from));
            from = to + 1;
        }
        if (t.size() < 5) continue;
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
        // Nodes to skip: text with bindings, inside lists
        ui::Document doc;
        ui::loadDocument("ui/" + file, doc);
        std::vector<std::string> skip;
        for (size_t i = 0; i < doc.nodes.size(); ++i) {
            const ui::Node& n = doc.nodes[i];
            bool inList = false;
            for (i32 p = n.parent; p >= 0; p = doc.nodes[static_cast<size_t>(p)].parent)
                if (doc.nodes[static_cast<size_t>(p)].widget == ui::WidgetKind::List) inList = true;
            bool differentText = false;
            if (n.kind == ui::NodeKind::Text) {
                // Keys in the table draw the table's text (translation): sizes are compared only when it's the same text as Penpot
                const std::string drawn = n.textKey.empty() ? n.textContent : std::string(strings.get(n.textKey));
                const auto box = std::find_if(boxes.begin(), boxes.end(), [&](const PenpotBox& b) { return b.id == n.id; });
                const std::string penpot = box != boxes.end() && box->hasText ? box->text : drawn;
                std::string plain; // text without style tags (<1>…</1>)
                for (size_t c = 0; c < drawn.size(); ++c) {
                    if (drawn[c] == '<') {
                        const size_t close = drawn.find('>', c);
                        if (close != std::string::npos && close - c <= 6) {
                            c = close;
                            continue;
                        }
                    }
                    if (drawn[c] == '\n') plain += "\\n"; // box files write newlines as the two characters \n
                    else plain += drawn[c];
                }
                differentText = plain != penpot || drawn.find('{') != std::string::npos;
            }
            if (inList || differentText) skip.push_back(n.id);
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
            const f32 tolerance = e->text ? std::max(kTextTolerance, kTextToleranceRatio * std::max(p.w, p.h)) : kBoxTolerance;
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

int runUiRender(const std::string& projectDir, const std::string& language, const std::string& uiFile, const std::string& pngPath) {
    setProjectRoot(projectDir);
    ui::Document doc;
    if (!ui::loadDocument(uiFile, doc)) {
        std::printf("UI-RENDER cannot read %s\n", uiFile.c_str());
        for (const std::string& e : doc.errors) std::printf("  %s\n", e.c_str());
        return 1;
    }
    const u32 w = static_cast<u32>(doc.boardW * 0.5f + 0.5f), h = static_cast<u32>(doc.boardH * 0.5f + 0.5f);
    Window window;
    if (!window.create({"MealKit UI render", w, h})) return 1;
    render::Renderer renderer;
    if (!renderer.init({window.nativeHandle(), window.width(), window.height(), false, false})) {
        std::printf("UI-RENDER renderer: %s\n", renderer.initError());
        return 1;
    }
    loc::Strings strings;
    strings.load(language, language);
    ui::System sys;
    sys.init(&renderer, &strings);
    sys.open(uiFile);
    const render::Viewport view{0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
    // A few frames: save the last frame after the text/image atlases are uploaded to the GPU
    for (int frame = 0; frame < 6 && window.pumpMessages(); ++frame) {
        if (frame == 5) renderer.requestCapture(pngPath.c_str());
        sys.update({}, view, 1.0 / 60.0);
        render::FrameParams params;
        params.viewport = view;
        params.fogColor = {0.0f, 0.0f, 0.0f};
        if (renderer.beginFrame(params)) {
            renderer.beginOverlay();
            sys.render(renderer);
            renderer.endFrame();
        }
    }
    renderer.shutdown();
    const bool ok = fileExists(pngPath);
    std::printf("UI-RENDER %s %ux%u -> %s\n", uiFile.c_str(), window.width(), window.height(), ok ? pngPath.c_str() : "failed");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

} // namespace mealkit::editor
