// D3DFORMAT -> OpenGL format table.
#include "d3d9_internal.h"

#define SW_RGBA {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA}
#define SW_RGB1 {GL_RED, GL_GREEN, GL_BLUE, GL_ONE}

static const FmtInfo FMTS[] = {
    // d3d   internal               format              type                               bpp comp  depth stencil swizzle
    {21, GL_RGBA8,               GL_BGRA,            GL_UNSIGNED_INT_8_8_8_8_REV,       4, false, false, false, SW_RGBA},  // A8R8G8B8
    {22, GL_RGBA8,               GL_BGRA,            GL_UNSIGNED_INT_8_8_8_8_REV,       4, false, false, false, SW_RGB1},  // X8R8G8B8
    {32, GL_RGBA8,               GL_RGBA,            GL_UNSIGNED_INT_8_8_8_8_REV,       4, false, false, false, SW_RGBA},  // A8B8G8R8
    {33, GL_RGBA8,               GL_RGBA,            GL_UNSIGNED_INT_8_8_8_8_REV,       4, false, false, false, SW_RGB1},  // X8B8G8R8
    {23, GL_RGB565,              GL_RGB,             GL_UNSIGNED_SHORT_5_6_5,           2, false, false, false, SW_RGB1},  // R5G6B5
    {24, GL_RGB5_A1,             GL_BGRA,            GL_UNSIGNED_SHORT_1_5_5_5_REV,     2, false, false, false, SW_RGB1},  // X1R5G5B5
    {25, GL_RGB5_A1,             GL_BGRA,            GL_UNSIGNED_SHORT_1_5_5_5_REV,     2, false, false, false, SW_RGBA},  // A1R5G5B5
    {26, GL_RGBA4,               GL_BGRA,            GL_UNSIGNED_SHORT_4_4_4_4_REV,     2, false, false, false, SW_RGBA},  // A4R4G4B4
    {28, GL_R8,                  GL_RED,             GL_UNSIGNED_BYTE,                  1, false, false, false, {GL_ZERO, GL_ZERO, GL_ZERO, GL_RED}}, // A8
    {50, GL_R8,                  GL_RED,             GL_UNSIGNED_BYTE,                  1, false, false, false, {GL_RED, GL_RED, GL_RED, GL_ONE}},    // L8
    {51, GL_RG8,                 GL_RG,              GL_UNSIGNED_BYTE,                  2, false, false, false, {GL_RED, GL_RED, GL_RED, GL_GREEN}},  // A8L8
    {34, GL_RG16,                GL_RG,              GL_UNSIGNED_SHORT,                 4, false, false, false, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},  // G16R16
    {36, GL_RGBA16,              GL_RGBA,            GL_UNSIGNED_SHORT,                 8, false, false, false, SW_RGBA},  // A16B16G16R16
    {31, GL_RGB10_A2,            GL_RGBA,            GL_UNSIGNED_INT_2_10_10_10_REV,    4, false, false, false, SW_RGBA},  // A2B10G10R10
    {35, GL_RGB10_A2,            GL_BGRA,            GL_UNSIGNED_INT_2_10_10_10_REV,    4, false, false, false, SW_RGBA},  // A2R10G10B10
    {60, GL_RG8_SNORM,           GL_RG,              GL_BYTE,                           2, false, false, false, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},  // V8U8
    {63, GL_RGBA8_SNORM,         GL_RGBA,            GL_BYTE,                           4, false, false, false, SW_RGBA},  // Q8W8V8U8
    {64, GL_RG16_SNORM,          GL_RG,              GL_SHORT,                          4, false, false, false, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},  // V16U16
    {111, GL_R16F,               GL_RED,             GL_HALF_FLOAT,                     2, false, false, false, {GL_RED, GL_ONE, GL_ONE, GL_ONE}},
    {112, GL_RG16F,              GL_RG,              GL_HALF_FLOAT,                     4, false, false, false, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},
    {113, GL_RGBA16F,            GL_RGBA,            GL_HALF_FLOAT,                     8, false, false, false, SW_RGBA},
    {114, GL_R32F,               GL_RED,             GL_FLOAT,                          4, false, false, false, {GL_RED, GL_ONE, GL_ONE, GL_ONE}},
    {115, GL_RG32F,              GL_RG,              GL_FLOAT,                          8, false, false, false, {GL_RED, GL_GREEN, GL_ONE, GL_ONE}},
    {116, GL_RGBA32F,            GL_RGBA,            GL_FLOAT,                         16, false, false, false, SW_RGBA},
    {FOURCC('D','X','T','1'), GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0,                   8, true,  false, false, SW_RGBA},
    {FOURCC('D','X','T','2'), GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0,                  16, true,  false, false, SW_RGBA},
    {FOURCC('D','X','T','3'), GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0,                  16, true,  false, false, SW_RGBA},
    {FOURCC('D','X','T','4'), GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0,                  16, true,  false, false, SW_RGBA},
    {FOURCC('D','X','T','5'), GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0,                  16, true,  false, false, SW_RGBA},
    // depth formats (D16 uses 2 bytes in lockable memory, everything else 4)
    {80, GL_DEPTH_COMPONENT24,   GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,                   2, false, true,  false, SW_RGBA},  // D16
    {70, GL_DEPTH_COMPONENT24,   GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,                   2, false, true,  false, SW_RGBA},  // D16_LOCKABLE
    {71, GL_DEPTH_COMPONENT32F,  GL_DEPTH_COMPONENT, GL_FLOAT,                          4, false, true,  false, SW_RGBA},  // D32
    {77, GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8,              4, false, true,  true,  SW_RGBA},  // D24X8
    {75, GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8,              4, false, true,  true,  SW_RGBA},  // D24S8
    {79, GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8,              4, false, true,  true,  SW_RGBA},  // D24X4S4
    {73, GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8,              4, false, true,  true,  SW_RGBA},  // D15S1
    {82, GL_DEPTH_COMPONENT32F,  GL_DEPTH_COMPONENT, GL_FLOAT,                          4, false, true,  false, SW_RGBA},  // D32F_LOCKABLE
    {83, GL_DEPTH24_STENCIL8,    GL_DEPTH_STENCIL,   GL_UNSIGNED_INT_24_8,              4, false, true,  true,  SW_RGBA},  // D24FS8
    {FOURCC('I','N','T','Z'), GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, false, true, true, {GL_RED, GL_RED, GL_RED, GL_RED}},
    {FOURCC('D','F','2','4'), GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, false, true, true, {GL_RED, GL_RED, GL_RED, GL_RED}},
    {FOURCC('D','F','1','6'), GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, false, true, false, {GL_RED, GL_RED, GL_RED, GL_RED}},
};

const FmtInfo *fmt_info(uint32_t d3dfmt) {
    for (auto &f : FMTS) if (f.d3d == d3dfmt) return &f;
    return nullptr;
}
uint32_t fmt_pitch(const FmtInfo *f, uint32_t w) {
    if (f->compressed) return ((w + 3) / 4) * f->bpp;
    return w * f->bpp;
}
uint32_t fmt_rows(const FmtInfo *f, uint32_t h) { return f->compressed ? (h + 3) / 4 : h; }
