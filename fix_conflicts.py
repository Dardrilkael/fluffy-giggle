import re

with open('apps/circsim_editor/src/side_panels.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# Fix palette panel conflict
old = '''void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
<<<<<<< HEAD
    ws::core::warn("palette: draw_palette_panel start");
    // The section header: the strong face, a step larger than the body
    // (the font hierarchy's loud rung - the tools list reads as a
    // section, not a longer label).
    gui::heading(layout, theme, "tools");
    if (gui::selectable(gui, layout, theme, "select",
=======
    gui::text(layout, theme, "Tools");
    if (gui::selectable(gui, layout, theme, "Select",
>>>>>>> graph-patch-testing
                        ed.tool == Tool::Select)) {'''

new = '''void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    // The section header: the strong face, a step larger than the body
    // (the font hierarchy's loud rung - the tools list reads as a
    // section, not a longer label).
    gui::heading(layout, theme, "Tools");
    if (gui::selectable(gui, layout, theme, "Select",
                        ed.tool == Tool::Select)) {'''

content = content.replace(old, new)
print("Fixed palette panel" if old in content else "Already fixed or not found")

# Fix simulation setup conflict
old2 = '''void draw_simulation_setup(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme) {
<<<<<<< HEAD
    gui::heading(layout, theme, "schematic");
    char grid_line[64];
    std::snprintf(grid_line, sizeof grid_line, "grid: %.12g",
                  ed.schem()->grid);
    gui::text(layout, theme, grid_line);
    gui::separator(layout, theme);

    // --- the analysis list
    gui::heading(layout, theme, "analyses (deck order)");
=======
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }
    gui::text(layout, theme, "Schematic");
    char grid[64];
    std::snprintf(grid, sizeof grid, "%.12g", ed.schem()->grid);
    label_line(layout, theme, "Grid", grid);

    // --- the analysis list
    panel_section(layout, theme, "Analyses (in deck order)");
    if (ed.analysis_index >=
        static_cast<int>(ed.schem()->analysis.entries.size())) {
        ed.analysis_index = -1;  // the list shrank under the selection
    }
    if (ed.schem()->analysis.entries.empty()) {
        hint_line(layout, theme, "None yet - add one below");
    }
>>>>>>> graph-patch-testing
    int index = 0;'''

new2 = '''void draw_simulation_setup(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }
    gui::heading(layout, theme, "Schematic")
    char grid[64];
    std::snprintf(grid, sizeof grid, "%.12g", ed.schem()->grid);
    label_line(layout, theme, "Grid", grid);
    gui::separator(layout, theme);

    // --- the analysis list
    gui::heading(layout, theme, "Analyses (in deck order)");
    if (ed.analysis_index >=
        static_cast<int>(ed.schem()->analysis.entries.size())) {
        ed.analysis_index = -1;  // the list shrank under the selection
    }
    if (ed.schem()->analysis.entries.empty()) {
        hint_line(layout, theme, "None yet - add one below");
    }
    int index = 0;'''

content = content.replace(old2, new2)
print("Fixed simulation setup" if old2 in content else "Already fixed or not found")

# Fix problems panel conflict
old3 = '''        const std::string prefix =
            d.severity == ws::circsim::Severity::Error
                ? "[E] "
                : (d.severity == ws::circsim::Severity::Warning ? "[W] "
                                                                : "[N] ");
<<<<<<< HEAD
        if (gui::selectable(gui, layout, rows_theme,
                            (prefix + ed.problems.lines[static_cast<std::size_t>(
                                           index)])
                                .c_str(),
=======
        // The rendered lines run parallel to the diags; fall back to the
        // diag's own rendering rather than index past a short list.
        const std::size_t at = static_cast<std::size_t>(index);
        const std::string text = at < ed.problems.lines.size()
                                     ? ed.problems.lines[at]
                                     : d.render();
        if (gui::selectable(gui, layout, theme, (prefix + text).c_str(),
>>>>>>> graph-patch-testing
                            false)) {'''

new3 = '''        const std::string prefix =
            d.severity == ws::circsim::Severity::Error
                ? "[E] "
                : (d.severity == ws::circsim::Severity::Warning ? "[W] "
                                                                : "[N] ");
        // The rendered lines run parallel to the diags; fall back to the
        // diag's own rendering rather than index past a short list.
        const std::size_t at = static_cast<std::size_t>(index);
        const std::string text = at < ed.problems.lines.size()
                                     ? ed.problems.lines[at]
                                     : d.render();
        if (gui::selectable(gui, layout, rows_theme, (prefix + text).c_str(),
                            false)) {'''

content = content.replace(old3, new3)
print("Fixed problems panel" if old3 in content else "Already fixed or not found")

# Fix netlist panel conflict
old4 = '''    // The lines, in the mono face when there is one (the theme's face
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
    int index = 0;
    for (const std::string& line : ed.netlist.lines) {
        gui.ids().push(index);
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
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
        ++index;
    }
}'''

new4 = '''    // The lines, in the mono face when there is one (the theme's face
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
        static_cast<float>(digits) * font->line_height() * 0.55f + 10.0f
    int index = 0;
    for (const std::string& line : ed.netlist.lines) {
        gui.ids().push(index);
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
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
        ++index;
    }
}'''

content = content.replace(old4, new4)
print("Fixed netlist panel" if old4 in content else "Already fixed or not found")

with open('apps/circsim_editor/src/side_panels.cpp', 'w', encoding='utf-8', newline='\r\n') as f:
    f.write(content)

print("Done!")