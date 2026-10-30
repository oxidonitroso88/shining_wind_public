// See include/extras/extras_style.h.
#include "extras/extras_style.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace pc_extras
{
    namespace
    {
        float g_scale = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
        Font g_font{};
        bool g_fontTried = false, g_fontOwned = false;
        ImageProvider *g_images = nullptr;

        constexpr float kFontBase = 64.0f; // glyphs are rasterised once at this size and drawn scaled

        Color toneColor(Tone t, unsigned char a = 255)
        {
            switch (t)
            {
            case Tone::Dim: return Color{150, 160, 178, a};
            case Tone::Accent: return Color{126, 208, 255, a};
            case Tone::Good: return Color{140, 224, 150, a};
            case Tone::Warn: return Color{255, 205, 110, a};
            case Tone::Bad: return Color{255, 130, 130, a};
            default: return Color{236, 240, 248, a};
            }
        }

        void loadFont()
        {
            if (g_fontTried)
                return;
            g_fontTried = true;
            const char *candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
            for (const char *path : candidates)
            {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    continue;
                Font f = LoadFontEx(path, static_cast<int>(kFontBase), nullptr, 0);
                if (f.texture.id != 0u)
                {
                    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                    g_font = f;
                    g_fontOwned = true;
                    return;
                }
            }
            g_font = GetFontDefault();
        }

        float sx(float v) { return g_ox + v * g_scale; }
        float sy(float v) { return g_oy + v * g_scale; }
        Rectangle toScreen(const Rect &r) { return Rectangle{sx(r.x), sy(r.y), r.w * g_scale, r.h * g_scale}; }
    }

    void Style::begin()
    {
        loadFont();
        const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
        g_scale = std::max(0.1f, std::min(w / kWidth, h / kHeight));
        g_ox = (w - kWidth * g_scale) * 0.5f;
        g_oy = (h - kHeight * g_scale) * 0.5f;
    }

    void Style::dimBackground(float alpha)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{6, 9, 18, static_cast<unsigned char>(255.0f * alpha)});
    }

    void Style::panel(const Rect &r, bool raised)
    {
        const Rectangle rr = toScreen(r);
        DrawRectangleRounded(rr, 0.04f, 8, raised ? Color{22, 30, 48, 238} : Color{14, 19, 32, 230});
        DrawRectangleRoundedLinesEx(rr, 0.04f, 8, std::max(1.0f, 1.5f * g_scale), Color{74, 96, 140, 255});
    }

    void Style::text(const std::string &s, float x, float y, float size, Tone tone)
    {
        DrawTextEx(g_font, s.c_str(), Vector2{sx(x), sy(y)}, size * g_scale, 0.0f, toneColor(tone));
    }

    float Style::textWidth(const std::string &s, float size)
    {
        return MeasureTextEx(g_font, s.c_str(), size, 0.0f).x;
    }

    void Style::textCentered(const std::string &s, float cx, float y, float size, Tone tone)
    {
        text(s, cx - textWidth(s, size) * 0.5f, y, size, tone);
    }

    void Style::header(const char *title, const char *subtitle)
    {
        text("PC EXTRAS", 64.0f, 40.0f, 20.0f, Tone::Accent);
        text(title, 64.0f, 64.0f, 44.0f, Tone::Normal);
        if (subtitle != nullptr)
            text(subtitle, 64.0f, 118.0f, 22.0f, Tone::Dim);
        DrawRectangle(static_cast<int>(sx(64.0f)), static_cast<int>(sy(152.0f)), static_cast<int>(1152.0f * g_scale), std::max(1, static_cast<int>(2.0f * g_scale)),
                      Color{74, 96, 140, 255});
    }

    void Style::footer(const char *hints)
    {
        DrawRectangle(static_cast<int>(sx(64.0f)), static_cast<int>(sy(654.0f)), static_cast<int>(1152.0f * g_scale), std::max(1, static_cast<int>(1.0f * g_scale)),
                      Color{74, 96, 140, 255});
        text(hints, 64.0f, 666.0f, 22.0f, Tone::Dim);
    }

    float Style::row(const std::string &label, const Rect &r, bool selected, bool enabled, const std::string &right, bool marked)
    {
        const Rectangle rr = toScreen(r);
        if (marked && !selected)
            DrawRectangleRounded(rr, 0.18f, 6, Color{30, 48, 82, 255});
        if (selected)
        {
            DrawRectangleRounded(rr, 0.18f, 6, Color{44, 84, 140, 255});
            DrawRectangleRounded(Rectangle{rr.x, rr.y, std::max(3.0f, 5.0f * g_scale), rr.height}, 0.5f, 4, toneColor(Tone::Accent));
        }
        const Tone t = !enabled ? Tone::Dim : (selected ? Tone::Normal : Tone::Normal);
        const float size = std::min(30.0f, r.h * 0.62f);
        text(label, r.x + 22.0f, r.y + (r.h - size) * 0.5f - 2.0f, size, t);
        if (!right.empty())
            text(right, r.x + r.w - 18.0f - textWidth(right, size * 0.8f), r.y + (r.h - size * 0.8f) * 0.5f - 2.0f, size * 0.8f, Tone::Dim);
        return r.h;
    }

    float Style::paragraph(const std::string &s, float x, float y, float width, float size, Tone tone)
    {
        std::string line, word;
        float used = 0.0f;
        const float step = size * 1.35f;
        auto flush = [&]() {
            if (!line.empty())
            {
                text(line, x, y + used, size, tone);
                used += step;
                line.clear();
            }
        };
        for (size_t i = 0; i <= s.size(); ++i)
        {
            if (i == s.size() || s[i] == ' ')
            {
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && textWidth(candidate, size) > width)
                {
                    flush();
                    line = word;
                }
                else
                    line = candidate;
                word.clear();
            }
            else
                word.push_back(s[i]);
        }
        flush();
        return used;
    }

    void Style::spinner(float cx, float cy, float radius, Tone tone)
    {
        const float a = static_cast<float>(GetTime()) * 300.0f;
        DrawRing(Vector2{sx(cx), sy(cy)}, (radius - 6.0f) * g_scale, radius * g_scale, a, a + 250.0f, 48, toneColor(tone));
    }

    void Style::message(const std::string &s, Tone tone, float y)
    {
        textCentered(s, kWidth * 0.5f, y, 26.0f, tone);
    }

    bool Style::image(const std::string &key, const Rect &where)
    {
        return g_images != nullptr && g_images->draw(key, where);
    }

    void Style::setImageProvider(ImageProvider *provider) { g_images = provider; }
}
