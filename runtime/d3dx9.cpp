// The two D3DX helpers the game imports: surface copy with conversion/scaling, and screenshot saving.
#include "rt.h"
#include <vector>
#include <string>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

bool d3d9_get_rgba(uint32_t surf, int x0, int y0, int w, int h, std::vector<uint8_t> &out);
bool d3d9_put_rgba(uint32_t surf, int x0, int y0, int w, int h, const std::vector<uint8_t> &in);
bool d3d9_surface_size(uint32_t surf, uint32_t &w, uint32_t &h);

static void rect_of(uint32_t r, uint32_t surf, int &x, int &y, int &w, int &h) {
    uint32_t sw = 0, sh = 0;
    d3d9_surface_size(surf, sw, sh);
    if (r) { x = (int)R32(r); y = (int)R32(r + 4); w = (int)R32(r + 8) - x; h = (int)R32(r + 12) - y; }
    else { x = 0; y = 0; w = (int)sw; h = (int)sh; }
}

// D3DXLoadSurfaceFromSurface(dst, dstPal, dstRect, src, srcPal, srcRect, Filter, ColorKey)
API(d3dx9_37, D3DXLoadSurfaceFromSurface, 8) {
    uint32_t dst = ARG(0), src = ARG(3), filter = ARG(6), key = ARG(7);
    int sx, sy, sw, sh, dx, dy, dw, dh;
    rect_of(ARG(5), src, sx, sy, sw, sh);
    rect_of(ARG(2), dst, dx, dy, dw, dh);
    std::vector<uint8_t> a, b;
    if (!d3d9_get_rgba(src, sx, sy, sw, sh, a)) RETN(8, 0x8876086C);
    if (sw == dw && sh == dh) b = a;
    else {
        b.assign((size_t)dw * dh * 4, 0);
        bool lin = (filter & 0xFF) >= 3;   // D3DX_FILTER_LINEAR and better
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            float fx = (x + 0.5f) * sw / dw - 0.5f, fy = (y + 0.5f) * sh / dh - 0.5f;
            uint8_t *o = &b[((size_t)y * dw + x) * 4];
            if (!lin) {
                int ix = std::min(sw - 1, std::max(0, (int)(fx + 0.5f))), iy = std::min(sh - 1, std::max(0, (int)(fy + 0.5f)));
                memcpy(o, &a[((size_t)iy * sw + ix) * 4], 4);
            } else {
                int x0 = std::max(0, (int)fx), y0 = std::max(0, (int)fy);
                int x1 = std::min(sw - 1, x0 + 1), y1 = std::min(sh - 1, y0 + 1);
                float tx = std::max(0.0f, fx - x0), ty = std::max(0.0f, fy - y0);
                for (int k = 0; k < 4; k++) {
                    float v = a[((size_t)y0 * sw + x0) * 4 + k] * (1 - tx) * (1 - ty) + a[((size_t)y0 * sw + x1) * 4 + k] * tx * (1 - ty)
                            + a[((size_t)y1 * sw + x0) * 4 + k] * (1 - tx) * ty + a[((size_t)y1 * sw + x1) * 4 + k] * tx * ty;
                    o[k] = (uint8_t)(v + 0.5f);
                }
            }
        }
    }
    if (key) {
        uint8_t kr = (key >> 16) & 255, kg = (key >> 8) & 255, kb = key & 255;
        for (size_t i = 0; i < b.size(); i += 4)
            if (b[i] == kr && b[i + 1] == kg && b[i + 2] == kb) b[i] = b[i + 1] = b[i + 2] = b[i + 3] = 0;
    }
    if (!d3d9_put_rgba(dst, dx, dy, dw, dh, b)) RETN(8, 0x8876086C);
    RETN(8, 0);
}

// D3DXSaveSurfaceToFileA(pDestFile, DestFormat, pSrcSurface, pSrcPalette, pSrcRect)
API(d3dx9_37, D3DXSaveSurfaceToFileA, 5) {
    std::string path = host_path(GS(ARG(0)));
    uint32_t fmt = ARG(1), src = ARG(2);
    int x, y, w, h;
    rect_of(ARG(4), src, x, y, w, h);
    std::vector<uint8_t> px;
    if (!d3d9_get_rgba(src, x, y, w, h, px)) RETN(5, 0x8876086C);
    CFStringRef type = fmt == 1 ? CFSTR("public.jpeg") : fmt == 3 ? CFSTR("public.png") : fmt == 2 ? CFSTR("com.truevision.tga-image") : CFSTR("com.microsoft.bmp");
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8 *)path.c_str(), (CFIndex)path.size(), false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, type, 1, nullptr);
    bool ok = false;
    if (d) { CGImageDestinationAddImage(d, img, nullptr); ok = CGImageDestinationFinalize(d); CFRelease(d); }
    CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    rtlog("D3DXSaveSurfaceToFile %s -> %s", path.c_str(), ok ? "ok" : "failed");
    RETN(5, ok ? 0 : 0x80004005);
}
