#include "mealkit/ui/Ui.h"

#include "mealkit/core/Log.h"
#include "mealkit/input/Actions.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/platform/Input.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Document.h"

#include "stb_image.h"

#include <cstddef>
#include <cstring>

namespace mealkit::ui {
namespace {

static_assert(sizeof(mkui::Quad) == sizeof(render::UiQuad) && offsetof(mkui::Quad, rect) == offsetof(render::UiQuad, rect) &&
                  offsetof(mkui::Quad, clip) == offsetof(render::UiQuad, clip) && offsetof(mkui::Quad, uv) == offsetof(render::UiQuad, uv) &&
                  offsetof(mkui::Quad, color0) == offsetof(render::UiQuad, color0) &&
                  offsetof(mkui::Quad, flags) == offsetof(render::UiQuad, flags) &&
                  offsetof(mkui::Quad, sdfScale) == offsetof(render::UiQuad, sdfScale),
              "mkui::Quad must match render::UiQuad (drawn as is)");
static_assert(mkui::kQuadShape == render::kUiShape && mkui::kQuadImage == render::kUiImage && mkui::kQuadGlyph == render::kUiGlyph &&
              mkui::kQuadShadow == render::kUiShadow && mkui::kQuadLinear == render::kUiLinear && mkui::kQuadRadial == render::kUiRadial);
static_assert(mkui::kAtlasSize == render::kUiAtlasSize);
static_assert(mkui::rgba(1, 2, 3, 4) == render::rgba(1, 2, 3, 4));

// The renderer has one pair of UI atlases, but each System packs its own glyphs from the corner. Only the System in use may
// write them (the editor's [UI] preview, then a play session, then the preview again): the next one rebakes when it takes over.
u64 g_atlasUser = 0;
u64 g_nextAtlasUser = 1;

// Pseudo-localization (editor preview): ASCII letters become accented look-alikes, {fields} and <n> span tags stay, about 40% more
// characters at the end, and brackets show where the text was cut
void pseudoize(const std::string& in, std::string& out) {
    // UTF-8 escapes (sources are read in the system code page): a b c d e f g h i j k l m n o p q r s t u v w x y z look-alikes
    static const char* const kLower[26] = {"\xC3\xA5", "\xC6\x80", "\xC3\xA7", "\xC3\xB0", "\xC3\xA9", "\xC6\x92", "\xC4\x9D", "\xC4\xA5", "\xC3\xAE",
                                           "\xC4\xB5", "\xC4\xB7", "\xC4\xBC", "m",        "\xC3\xB1", "\xC3\xB6", "\xC3\xBE", "q",        "\xC5\x95",
                                           "\xC5\xA1", "\xC5\xA3", "\xC3\xBB", "v",        "\xC5\xB5", "x",        "\xC3\xBD", "\xC5\xBE"};
    static const char* const kUpper[26] = {"\xC3\x85", "B",        "\xC3\x87", "\xC3\x90", "\xC3\x89", "F",        "\xC4\x9C", "\xC4\xA4", "\xC3\x8E",
                                           "\xC4\xB4", "\xC4\xB6", "\xC4\xBB", "M",        "\xC3\x91", "\xC3\x96", "\xC3\x9E", "Q",        "\xC5\x94",
                                           "\xC5\xA0", "\xC5\xA2", "\xC3\x9B", "V",        "\xC5\xB4", "X",        "\xC3\x9D", "\xC5\xBD"};
    out.clear();
    out += '[';
    u32 letters = 0;
    int depth = 0; // inside {field} or <tag>
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '{' || c == '<') ++depth;
        if (depth == 0 && c >= 'a' && c <= 'z') out += kLower[c - 'a'];
        else if (depth == 0 && c >= 'A' && c <= 'Z') out += kUpper[c - 'A'];
        else out += c;
        if (c == '}' || c == '>') depth = depth > 0 ? depth - 1 : 0;
        if (depth == 0 && (static_cast<unsigned char>(c) & 0xC0) != 0x80 && c != ' ') ++letters; // code points, not bytes
    }
    out += ' ';
    for (u32 i = 0, n = (letters * 2 + 4) / 5; i < n; ++i) out += '~';
    out += ']';
}

// Assets, logs, strings and atlases for the runtime
struct EngineHost final : mkui::Host {
    render::Renderer* renderer = nullptr;
    const loc::Strings* strings = nullptr;
    u64 atlasUser = g_nextAtlasUser++;

    bool readFile(const std::string& assetPath, std::string& out) override {
        const std::string path = findAssetFile(assetPath);
        return !path.empty() && readTextFile(path, out);
    }
    u64 fileTime(const std::string& assetPath) override {
        const std::string path = findAssetFile(assetPath);
        return path.empty() ? 0 : fileWriteTime(path);
    }
    std::vector<std::string> listFiles(const std::string& assetFolder, const char* extension) override {
        const std::string dir = findAsset("assets/" + assetFolder);
        return dir.empty() ? std::vector<std::string>{} : mealkit::listFiles(dir, extension);
    }
    void log(mkui::LogLevel level, const std::string& message) override {
        if (level == mkui::LogLevel::Warn) MEALKIT_WARN("%s", message.c_str());
        else MEALKIT_INFO("%s", message.c_str());
    }
    bool pseudo = false;
    std::string pseudoText;
    const std::string& text(const std::string& key) override {
        const std::string& t = strings ? strings->get(key) : key;
        if (!pseudo) return t;
        pseudoize(t, pseudoText);
        return pseudoText;
    }
    bool decodeImage(const std::string& bytes, u32& width, u32& height, std::vector<u8>& rgba) override {
        int w = 0, h = 0, channels = 0;
        u8* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
        if (!pixels) return false;
        width = static_cast<u32>(w);
        height = static_cast<u32>(h);
        rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }
    void uploadAtlas(mkui::AtlasKind atlas, u32 x, u32 y, u32 w, u32 h, const u8* pixels) override {
        if (renderer && atlasUser == g_atlasUser)
            renderer->updateUiAtlas(atlas == mkui::AtlasKind::Glyphs ? render::UiAtlas::Glyphs : render::UiAtlas::Images, x, y, w, h, pixels);
    }
};

mkui::ValueKind kindOf(reflect::Kind k) {
    switch (k) {
    case reflect::Kind::Bool: return mkui::ValueKind::Bool;
    case reflect::Kind::Int: return mkui::ValueKind::Int;
    case reflect::Kind::UInt: return mkui::ValueKind::UInt;
    case reflect::Kind::Float: return mkui::ValueKind::Float;
    case reflect::Kind::Ticks: return mkui::ValueKind::Ticks;
    case reflect::Kind::Color: return mkui::ValueKind::Color;
    case reflect::Kind::String: return mkui::ValueKind::String;
    case reflect::Kind::Array: return mkui::ValueKind::Array;
    case reflect::Kind::Struct: return mkui::ValueKind::Struct;
    default: return mkui::ValueKind::Other;
    }
}

// View models are Reflect types (MEALKIT_VIEWMODEL): Model::type is a reflect::TypeInfo
struct ReflectBinder final : mkui::Binder {
    bool resolve(const mkui::Model& model, const std::string& path, mkui::Value& out) const override {
        const reflect::ValueInfo* v = nullptr;
        void* data = nullptr;
        if (!reflect::resolvePath(*static_cast<const reflect::TypeInfo*>(model.type), model.data, path, v, data)) return false;
        out = {};
        out.kind = kindOf(v->kind);
        out.size = v->size;
        out.data = data;
        out.info = v;
        if (!v->enumNames.empty()) out.enumNames = &v->enumNames;
        out.ticksPerSecond = static_cast<f64>(reflect::tickRate());
        if (v->kind == reflect::Kind::Array && v->element) {
            out.count = v->count;
            out.elementSize = v->element->size;
            out.elementKind = kindOf(v->element->kind);
            out.elementType = v->element->type;
        }
        return true;
    }
    const char* typeName(const mkui::Model& model) const override { return static_cast<const reflect::TypeInfo*>(model.type)->name; }
    void format(const mkui::Value& value, std::string& out) const override {
        out += reflect::formatScalar(*static_cast<const reflect::ValueInfo*>(value.info), value.data);
    }
};

EngineHost& engineHost() { // document reading outside ui::System (no renderer or strings)
    static EngineHost host;
    return host;
}

mkui::Model model(reflect::View v) { return {v.type, const_cast<void*>(v.data)}; }

} // namespace

bool loadTheme(Theme& out, std::vector<std::string>* errors) { return mkui::loadTheme(out, engineHost(), errors); }
bool loadDocument(const std::string& assetPath, Document& out, const Theme* theme) {
    return mkui::loadDocument(assetPath, out, engineHost(), theme);
}
bool parseDocument(const std::string& text, const std::string& assetPath, Document& out, const Theme* theme) {
    return mkui::parseDocument(text, assetPath, out, engineHost(), theme);
}

struct System::Impl {
    EngineHost host;
    ReflectBinder binder;
    mkui::System ui{host, binder};

    void takeAtlas() {
        if (!host.renderer || host.atlasUser == g_atlasUser) return;
        clearAtlases(*host.renderer);
        g_atlasUser = host.atlasUser;
        ui.reload();
    }

    // The packer leaves gaps between entries and linear filtering reads them: leftovers from the previous user would show as specks around text
    static void clearAtlases(render::Renderer& renderer) {
        if (g_atlasUser == 0) return; // nothing written yet
        const std::vector<u8> zeros(static_cast<size_t>(render::kUiAtlasSize) * render::kUiAtlasSize * 4);
        renderer.updateUiAtlas(render::UiAtlas::Glyphs, 0, 0, render::kUiAtlasSize, render::kUiAtlasSize, zeros.data());
        renderer.updateUiAtlas(render::UiAtlas::Images, 0, 0, render::kUiAtlasSize, render::kUiAtlasSize, zeros.data());
    }
};

System::System() : m(new Impl()) {}
System::~System() { delete m; }

void System::init(render::Renderer* renderer, const loc::Strings* strings) {
    m->host.renderer = renderer;
    m->host.strings = strings;
    if (renderer) {
        Impl::clearAtlases(*renderer);
        g_atlasUser = m->host.atlasUser;
    }
    m->ui.reload();
}

ScreenId System::open(const std::string& uiPath, i32 layer) { return m->ui.open(uiPath, layer); }
ScreenId System::openWorld(const std::string& uiPath) { return m->ui.openWorld(uiPath); }
void System::close(ScreenId screen) { m->ui.close(screen); }
bool System::isOpen(ScreenId screen) const { return m->ui.isOpen(screen); }
void System::closeAll() { m->ui.closeAll(); }
void System::bind(ScreenId screen, reflect::View viewModel) { m->ui.bind(screen, model(viewModel)); }
void System::setScale(f32 uiScale) { m->ui.setScale(uiScale); }
f32 System::scale() const { return m->ui.scale(); }

void System::update(const FrameInput& in, const render::Viewport& viewport, f64 dt) {
    m->takeAtlas();
    mkui::FrameInput f;
    const Input* input = in.input;
    if (input && in.mouseAllowed) {
        f.mouse = true;
        f.mouseX = input->mouseX();
        f.mouseY = input->mouseY();
        f.mouseDown = input->mouseDown(MouseButton::Left);
        f.mousePressed = input->mousePressed(MouseButton::Left);
        f.mouseMoved = input->mouseMoved();
        f.wheel = input->wheelDelta();
    }
    if (input && in.keyboardAllowed) {
        f.typing = true;
        f.text = &input->text();
        f.backspaces = input->backspaces();
        f.enter = input->keyPressed(key::Enter);
        f.escape = input->keyPressed(key::Escape);
    }
    if (in.actions && in.keyboardAllowed) {
        const input::Actions& a = *in.actions;
        f.navigation = true;
        f.up = a.pressed("ui.up");
        f.down = a.pressed("ui.down");
        f.left = a.pressed("ui.left");
        f.right = a.pressed("ui.right");
        f.confirm = a.pressed("ui.confirm");
        f.back = a.pressed("ui.back");
    }
    m->ui.update(f, {viewport.x, viewport.y, viewport.width, viewport.height}, dt);
}

const std::vector<Action>& System::actions() const { return m->ui.actions(); }
bool System::wantsMouse() const { return m->ui.wantsMouse(); }
bool System::wantsKeyboard() const { return m->ui.wantsKeyboard(); }

void System::setCamera(const Mat4& viewProj) { m->ui.setCamera(&viewProj.m[0][0]); }
void System::drawAt(ScreenId templateScreen, Vec3 p, reflect::View viewModel, bool clampToEdges) {
    m->ui.drawAt(templateScreen, p.x, p.y, p.z, model(viewModel), clampToEdges);
}

void System::render(render::Renderer& renderer) {
    const std::vector<mkui::Quad>& quads = m->ui.render();
    renderer.drawUi(reinterpret_cast<const render::UiQuad*>(quads.data()), static_cast<u32>(quads.size()));
}

void System::layout(const render::Viewport& viewport) {
    m->takeAtlas();
    m->ui.layout({viewport.x, viewport.y, viewport.width, viewport.height});
}

ScreenInfo System::info(ScreenId screen) const { return m->ui.info(screen); }
std::vector<NodeBox> System::boxes(ScreenId screen) const { return m->ui.boxes(screen); }
void System::setDraft(ScreenId screen, const std::string* text) { m->ui.setDraft(screen, text); }
void System::setPreviewState(ScreenId screen, i32 node, mkui::State state) { m->ui.setPreviewState(screen, node, state); }
void System::moveFocus(ScreenId screen, int direction) { m->ui.moveFocus(screen, direction); }
void System::setFocusVisible(bool always) { m->ui.setFocusVisible(always); }
u32 System::quadCount() const { return m->ui.quadCount(); }
void System::setPseudoText(bool on) {
    if (m->host.pseudo == on) return;
    m->host.pseudo = on;
    m->ui.reload();
}
bool System::pseudoText() const { return m->host.pseudo; }
std::vector<ScreenId> System::screens() const { return m->ui.screens(); }
reflect::View System::viewModel(ScreenId screen) const {
    const mkui::Model vm = m->ui.model(screen);
    return vm ? reflect::View{static_cast<const reflect::TypeInfo*>(vm.type), vm.data} : reflect::View{};
}
bool System::press(ScreenId screen, const std::string& nodeName) { return m->ui.press(screen, nodeName); }
bool System::pressAny(const std::string& nodeName) { return m->ui.pressAny(nodeName); }
bool System::focusText(ScreenId screen, const std::string& nodeName) { return m->ui.focusText(screen, nodeName); }
bool System::editingText() const { return m->ui.editingText(); }

} // namespace mealkit::ui
