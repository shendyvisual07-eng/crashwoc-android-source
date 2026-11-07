#include "gfx/gl.h"

#include <SDL3/SDL.h>

#define NW_GL_DEFINE(ret, name, params) PFN_nw_##name nw_##name;
NW_GL_FUNCTIONS(NW_GL_DEFINE)
#undef NW_GL_DEFINE

bool nw_gl_load(void)
{
    bool ok = true;
#define NW_GL_LOAD(ret, name, params) \
    const char *symbol = #name; \
    if (SDL_strcmp(symbol, "glClearDepth") == 0) { \
        symbol = "glClearDepthf"; \
    } \
    nw_##name = (PFN_nw_##name)SDL_GL_GetProcAddress(symbol); \
#if defined(__ANDROID__) \
    if (SDL_strcmp(symbol, "glPolygonMode") == 0) { \
        nw_##name = NULL; /* Not part of OpenGL ES; wireframe is disabled on Android. */ \
        ok = true; \
    } \
#endif \
    if (!nw_##name) { \
        SDL_Log("GL function %s is missing", #name); \
        ok = false; \
    }
    NW_GL_FUNCTIONS(NW_GL_LOAD)
#undef NW_GL_LOAD
    return ok;
}
