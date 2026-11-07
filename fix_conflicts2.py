import re

with open('apps/circsim_editor/src/side_panels.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Fix first netlist panel conflict (font handling)
old1 = '''    // The lines, in the mono face when there is one (the theme's face
<<<<<<< HEAD
    // otherwise - readable, just not column-aligned). A fontless
    // theme with no mono face falls back to plain text rows, the
    // value rows' own ladder - never a null deref on the way down.
    const ws::ui::r2d::Font* font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? ed.font_mono
                                                          : theme.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        for (const std::string& line : ed.netlist.lines) {
            gui::text(layout, theme, line.c_str());
        }
        return;
    }
    const float line_h = font->line_height() + 2.0f;
=======
    // otherwise - readable, just not column-aligned). A dim line-number
    // gutter on the left (the pad is generous: the number is left-aligned,
    // so a proportional face only costs a little extra space); comments
    // recede and dot-commands (.tran, .model ...) take the accent.
    const ws::ui::r2d::Font& font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? *ed.font_mono
                                                          : *theme.font;
    const float line_h = font.line_height() + 2.0f;
    const std::size_t digits =
        std::to_string(ed.netlist.lines.size()).size();
    const float gutter =
        static_cast<float>(digits) * font.line_height() * 0.55f + 10.0f;
>>>>>>> graph-patch-testing
    int index = 0;'''

new1 = '''    // The lines, in the mono face when there is one (the theme's face
    // otherwise - readable, just not column-aligned). A fontless
    // theme with no mono face falls back to plain text rows, the
    // value rows' own ladder - never a null deref on the way down.
    // A dim line-number gutter on the left; comments recede and
    // dot-commands (.tran, .model ...) take the accent.
    const ws::ui::r2d::Font* font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? ed.font_mono
                                                          : theme.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        for (const std::string& line : ed.netlist.lines) {
            gui::text(layout, theme, line.c_str());
        }
        return;
    }
    const float line_h = font->line_height() + 2.0f;
    const std::size_t digits =
        std::to_string(ed.netlist.lines.size()).size();
    const float gutter =
        static_cast<float>(digits) * font->line_height() * 0.55f + 10.0f;
    int index = 0;'''

content = content.replace(old1, new1)
print("Fixed first netlist conflict" if old1 in content else "First netlist conflict not found")

# Fix second netlist panel conflict (drawing loop)
old2 = '''        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
<<<<<<< HEAD
        (void)ws::ui::r2d::draw_text(row.x, row.y, line.c_str(), *font,
                                     gui::dim(theme.text, 0.85f));
=======
        char number[16];
        std::snprintf(number, sizeof number, "%d", index + 1);
        (void)ws::ui::r2d::draw_text(row.x, row.y, number, font,
                                     gui::dim(theme.text, 0.4f));
        std::uint32_t color = gui::dim(theme.text, 0.85f);
        if (!line.empty() && line[0] == '*') {
            color = gui::dim(theme.text, 0.5f);
        } else if (!line.empty() && line[0] == '.') {
            color = theme.control_on;
        }
        (void)ws::ui::r2d::draw_text(row.x + gutter, row.y, line.c_str(),
                                     font, color);
>>>>>>> graph-patch-testing
        gui.ids().pop()
        ++index'''

new2 = '''        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
        char number[16];
        std::snprintf(number, sizeof number, "%d", index + 1);
        (void)ws::ui::r2d::draw_text(row.x, row.y, number, *font,
                                     gui::dim(theme.text, 0.4f));
        std::uint32_t color = gui::dim(theme.text, 0.85f);
        if (!line.empty() && line[0] == '*') {
            color = gui::dim(theme.text, 0.5f);
        } else if (!line.empty() && line[0] == '.') {
            color = theme.control_on;
        }
        (void)ws::ui::r2d::draw_text(row.x + gutter, row.y, line.c_str(),
                                     *font, color);
        gui.ids().pop();
        ++index'''

content = content.replace(old2, new2)
print("Fixed second netlist conflict" if old2 in content else "Second netlist conflict not found")

with open('apps/circsim_editor/src/side_panels.cpp', 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(content)

print("Done!")