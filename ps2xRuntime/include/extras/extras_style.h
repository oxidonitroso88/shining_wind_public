#ifndef PC_EXTRAS_STYLE_H
#define PC_EXTRAS_STYLE_H

#include <string>

// PC Extras - presentation (raylib). Everything is laid out on a virtual 1280x720 canvas that is scaled to the window, so features never deal with
// pixels. Colours come from a small palette (Tone); fonts are loaded once (Segoe UI when the system has it, raylib's default otherwise).
namespace pc_extras
{
    enum class Tone { Normal, Dim, Accent, Good, Warn, Bad };

    struct Rect
    {
        float x = 0, y = 0, w = 0, h = 0;
    };

    // Optional source of original game artwork (portraits, backdrops, ...). Nothing registers one yet; features ask for an image by key
    // ("portrait:seena") and simply draw nothing when no provider has it, so artwork can be added later without touching the features.
    class ImageProvider
    {
    public:
        virtual ~ImageProvider() = default;
        // Draws `key` into the virtual-canvas rectangle (keeping its aspect ratio); false when the provider does not have it.
        virtual bool draw(const std::string &key, const Rect &where) = 0;
    };

    class Style
    {
    public:
        static constexpr float kWidth = 1280.0f, kHeight = 720.0f;

        // One frame: begin() computes the scale for the current window, end() restores nothing (stateless between frames).
        void begin();
        void dimBackground(float alpha = 0.78f);
        void panel(const Rect &r, bool raised = true);
        void header(const char *title, const char *subtitle = nullptr);
        void footer(const char *hints);
        void text(const std::string &s, float x, float y, float size = 28.0f, Tone tone = Tone::Normal);
        void textCentered(const std::string &s, float cx, float y, float size = 28.0f, Tone tone = Tone::Normal);
        float textWidth(const std::string &s, float size = 28.0f);
        // A selectable row; `right` is optional right aligned text. Returns the row height.
        // `marked`: the row is the current choice of a list that does not have the focus (subtle highlight).
        float row(const std::string &label, const Rect &r, bool selected, bool enabled = true, const std::string &right = std::string(), bool marked = false);
        // Word-wrapped text inside `width`; returns the height used.
        float paragraph(const std::string &s, float x, float y, float width, float size = 22.0f, Tone tone = Tone::Dim);
        void spinner(float cx, float cy, float radius, Tone tone = Tone::Accent);
        void message(const std::string &s, Tone tone, float y);
        bool image(const std::string &key, const Rect &where);

        static void setImageProvider(ImageProvider *provider);
    };
}

#endif // PC_EXTRAS_STYLE_H
