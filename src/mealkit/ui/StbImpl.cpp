// stb_truetype(UI 글꼴 SDF)와 stb_image(UI 이미지 PNG) 구현. 외부 코드라 경고를 끈다 (engine/CMakeLists.txt).
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // 모델 텍스처 (glTF는 PNG·JPEG, asset/TextureCache.cpp)
#define STBI_NO_STDIO
#include "stb_image.h"
