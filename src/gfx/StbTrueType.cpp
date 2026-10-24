// Single compilation unit for the stb_truetype implementation.
#include "GfxAlloc.h"

#define STBTT_malloc(x, u) ((void)(u), gfxAlloc(x))
#define STBTT_free(x, u) ((void)(u), gfxFree(x))
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"
