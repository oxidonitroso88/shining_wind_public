#ifndef PS2_TEXT_PITCH_H
#define PS2_TEXT_PITCH_H

class PS2Runtime;

namespace ps2_text_pitch
{
    // Shining Wind text engine (0x30B000-0x30CBD0): glyph quad builder 0x30BDF0. In "fixed pitch" windows (style byte 0x17 == 0) every glyph
    // is drawn as its whole font cell and advances by the cell width; with the byte set it is cropped to the FNT record's [crop_left,
    // crop_left+crop_width) and advances by crop_width. Latin text translated from Japanese needs the proportional form everywhere, so ASCII
    // glyphs (0x20-0x7E) are always drawn in the proportional form; Japanese glyphs are untouched. PS2X_ASCII_PITCH=fixed disables the hook.
    // docs/text-pitch-ascii-2026-10-17.md
    void installFromEnvironment(PS2Runtime &runtime);
}

#endif // PS2_TEXT_PITCH_H
