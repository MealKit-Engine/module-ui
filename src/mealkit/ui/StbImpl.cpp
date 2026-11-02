// stb_image implementation (UI image PNG, model textures, sky HDRI). External code, so warnings are off (engine/CMakeLists.txt).
// stb_truetype (UI font SDF) is compiled in the mkui runtime (engine/libs/mkui).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // model textures (glTF uses PNG/JPEG, asset/TextureCache.cpp)
#if MEALKIT_DEBUG_UI
#define STBI_ONLY_HDR // sky HDRI baking (asset/Environment.cpp, dev builds only. Shipping builds only read the baked cache)
#endif
#define STBI_NO_STDIO
#include "stb_image.h"
