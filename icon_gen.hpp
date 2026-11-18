#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace miearphone {

// Generates a crisp anti-aliased RGBA HICON (e.g., 32x32 or 64x64) with a rounded squircle
// background in `bg_rgb` (0xRRGGBB) and a white wireless earbuds silhouette.
inline HICON create_earbud_icon(int size, uint32_t bg_rgb) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hColor || !bits) return nullptr;

    uint32_t* px = static_cast<uint32_t*>(bits);
    const float br = static_cast<float>((bg_rgb >> 16) & 0xFF);
    const float bg = static_cast<float>((bg_rgb >> 8) & 0xFF);
    const float bb = static_cast<float>(bg_rgb & 0xFF);

    // Super-sampled SDF rendering in normalized [-1, 1] coordinates
    auto sdf_round_box = [](float x, float y, float hx, float hy, float r) {
        float qx = std::fabs(x) - hx + r;
        float qy = std::fabs(y) - hy + r;
        float out = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f));
        float in = std::min(std::max(qx, qy), 0.0f);
        return out + in - r;
    };

    auto sdf_circle = [](float x, float y, float cx, float cy, float r) {
        return std::hypot(x - cx, y - cy) - r;
    };

    auto sdf_capsule = [](float px_, float py_, float ax, float ay, float bx_, float by_, float r) {
        float pax = px_ - ax, pay = py_ - ay;
        float bax = bx_ - ax, bay = by_ - ay;
        float h = std::clamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
        return std::hypot(pax - bax * h, pay - bay * h) - r;
    };

    const float aa = 2.2f / static_cast<float>(size);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float u = ((x + 0.5f) / static_cast<float>(size)) * 2.0f - 1.0f;
            float v = ((y + 0.5f) / static_cast<float>(size)) * 2.0f - 1.0f;

            // Squircle background
            float d_bg = sdf_round_box(u, v, 0.88f, 0.88f, 0.42f);
            float a_bg = std::clamp(0.5f - d_bg / aa, 0.0f, 1.0f);

            // Subtle top-left highlight gradient
            float grad = std::clamp(1.08f - 0.16f * (u + v), 0.88f, 1.12f);
            float r = std::clamp(br * grad, 0.0f, 255.0f);
            float g = std::clamp(bg * grad, 0.0f, 255.0f);
            float b = std::clamp(bb * grad, 0.0f, 255.0f);

            // Left earbud (head + stem)
            float dl_head = sdf_circle(u, v, -0.30f, -0.20f, 0.25f);
            float dl_tip  = sdf_circle(u, v, -0.14f, -0.20f, 0.14f);
            float dl_stem = sdf_capsule(u, v, -0.34f, -0.10f, -0.34f, 0.46f, 0.105f);
            float d_left  = std::min({dl_head, dl_tip, dl_stem});

            // Right earbud (mirrored)
            float dr_head = sdf_circle(u, v,  0.30f, -0.20f, 0.25f);
            float dr_tip  = sdf_circle(u, v,  0.14f, -0.20f, 0.14f);
            float dr_stem = sdf_capsule(u, v,  0.34f, -0.10f,  0.34f, 0.46f, 0.105f);
            float d_right = std::min({dr_head, dr_tip, dr_stem});

            float d_fg = std::min(d_left, d_right);
            float a_fg = std::clamp(0.5f - d_fg / aa, 0.0f, 1.0f);

            // Inner acoustic vent dot cutout
            float d_vent = std::min(sdf_circle(u, v, -0.33f, -0.21f, 0.065f),
                                    sdf_circle(u, v,  0.33f, -0.21f, 0.065f));
            float a_vent = std::clamp(0.5f - d_vent / aa, 0.0f, 1.0f);
            a_fg *= (1.0f - 0.75f * a_vent);

            float out_r = r * (1.0f - a_fg) + 255.0f * a_fg;
            float out_g = g * (1.0f - a_fg) + 255.0f * a_fg;
            float out_b = b * (1.0f - a_fg) + 255.0f * a_fg;

            uint8_t A = static_cast<uint8_t>(std::round(a_bg * 255.0f));
            // Premultiplied or straight BGRA (Win32 CreateIconIndirect uses straight BGRA with 32bpp alpha)
            uint8_t R = static_cast<uint8_t>(std::round(out_r));
            uint8_t G = static_cast<uint8_t>(std::round(out_g));
            uint8_t B = static_cast<uint8_t>(std::round(out_b));

            px[y * size + x] = (static_cast<uint32_t>(A) << 24) |
                               (static_cast<uint32_t>(R) << 16) |
                               (static_cast<uint32_t>(G) << 8)  |
                               static_cast<uint32_t>(B);
        }
    }

    HBITMAP hMask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hColor);
    DeleteObject(hMask);
    return hIcon;
}

} // namespace miearphone
