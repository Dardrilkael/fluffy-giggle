import re

with open('apps/circsim_editor/src/side_panels.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Fix the remaining netlist panel conflict (drawing loop)
old = '''        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
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
        gui.ids().pop();
        ++index'''

new = '''        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
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

content = content.replace(old, new)
print("Fixed netlist conflict" if old in content else "Conflict not found - trying alternative")

# Try alternative if not found
if old not in content:
    # Try with slightly different formatting
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
        gui.ids().pop();
        ++index'''
    content = content.replace(old2, new)
    print("Fixed with alternative" if old2 in content else "Alternative also not found")

with open('apps/circsim_editor/src/side_panels.cpp', 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(content)

print("Done!")