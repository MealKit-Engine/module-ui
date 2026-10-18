#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/loc/Strings.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/ui/Ui.h"

#include <string>
#include <vector>

// 에디터 [UI] 패널: 게임 화면(assets/ui/*.ui) 미리보기. 고른 화면을 편집 모드의 씬 뷰 위에 그린다.
// 해상도(비율)·UI 배율·언어(글자 넘침 확인)를 바꾸고, 게임의 뷰 모델(MEALKIT_VIEWMODEL)에 가짜 값을 넣어 본다.
// 파일 오류·바인딩 오류(없는 필드)와 노드 수(예산 500)를 보여 준다. .ui를 고쳐 저장하면 바로 다시 읽는다.
namespace mealkit::editor {

class UiPanel {
public:
    // 프로젝트를 열 때: 기본 언어 (mealkit.project의 language)
    void setProject(const std::string& defaultLanguage);
    // 게임 DLL의 뷰 모델 목록 (GameModule::viewModels, DLL을 내리기 전에 nullptr)
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types);
    void draw(bool* open);
    void select(const std::string& uiFile); // 자동 테스트: "hud.ui"
    // 편집 모드에서 패널이 열려 있으면 씬 뷰 위에 그린다 (3D 다음, 에디터 UI 전)
    void render(render::Renderer& renderer, const render::Viewport& sceneView, f64 dt);

private:
    void refresh();
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
};

} // namespace mealkit::editor
