// The ws::app bridge's chrome views (see command_views.hpp): the
// chord hint text every view shares, the registry-as-menus builder
// and the toolbar row. The palette and the keymap editor are their own
// family files (command_palette.cpp, command_keymap.cpp), the
// one-family-per-file rule.

#include "widgets_detail.hpp"

#include <ws/ui/gui/command_views.hpp>

#include <utility>

namespace ws::ui::gui {

std::string command_hint(const ws::app::Command& command,
                         const ws::app::ShortcutResolver* resolver,
                         ws::app::KeyContext context) {
    if (resolver != nullptr) {
        // The live binding only: a removed binding must not fall back
        // to hinting the declared chord it no longer answers.
        const ws::app::Shortcut* bound =
            resolver->binding_for(command.id, context);
        return bound != nullptr ? ws::app::describe_chord(*bound)
                                 : std::string{};
    }
    return command.shortcut.key != 0
               ? ws::app::describe_chord(command.shortcut)
               : std::string{};
}

// --- CommandMenuBuilder -----------------------------------------------------

void CommandMenuBuilder::describe(std::vector<CommandMenuSpec> specs) {
    specs_ = std::move(specs);
    // The per-menu arrays resize here (assign: plain vectors of
    // move-friendly elements), once per describe - build() only
    // clears and refills them, so a steady registry costs no
    // allocation per frame.
    items_.assign(specs_.size(), {});
    ids_.assign(specs_.size(), {});
    menus_.clear();
    hints_.clear();
}

void CommandMenuBuilder::build(const ws::app::CommandRegistry& registry,
                               const ws::app::ShortcutResolver* resolver,
                               ws::app::KeyContext context) {
    hints_.clear();
    menus_.clear();
    for (std::size_t m = 0; m < specs_.size(); ++m) {
        std::vector<MenuItem>& items = items_[m];
        std::vector<std::string>& ids = ids_[m];
        items.clear();
        ids.clear();
        for (const ws::app::Command& cmd : registry.commands()) {
            if (cmd.category != specs_[m].category) {
                continue;
            }
            MenuItem item;
            item.label = cmd.label.c_str();
            std::string hint = command_hint(cmd, resolver, context);
            if (!hint.empty()) {
                // Into the deque: existing elements never move, so
                // the c_str() the items borrow stays put while the
                // frame runs (a vector would move them on regrow).
                hints_.emplace_back(std::move(hint));
                item.shortcut = hints_.back().c_str();
            }
            // A command carrying a checked predicate is a toggle row
            // (its box reads the predicate live); one without is a
            // plain action. disabled_reason stays null - the registry
            // carries no reasons, and the menus make the lock speak.
            item.kind = cmd.checked != nullptr ? MenuItemKind::check
                                               : MenuItemKind::action;
            item.checked = ws::app::CommandRegistry::is_checked(cmd);
            item.disabled = !ws::app::CommandRegistry::is_enabled(cmd);
            items.push_back(item);
            ids.push_back(cmd.id);
        }
        if (items.empty()) {
            continue;  // a category with no commands drops its menu
        }
        menus_.push_back(MenuDesc{specs_[m].title, items.data(),
                                  static_cast<int>(items.size())});
    }
}

std::string_view CommandMenuBuilder::command_of(const MenuItem* item) const {
    if (item == nullptr) {
        return {};
    }
    for (std::size_t m = 0; m < items_.size(); ++m) {
        for (std::size_t i = 0; i < items_[m].size(); ++i) {
            if (&items_[m][i] == item) {
                return ids_[m][i];
            }
        }
    }
    return {};
}

// --- command_toolbar --------------------------------------------------------

namespace {

// The toolbar tooltip's one line: the label (the icon button dropped
// it, the text button shows it - the hint carries it either way) plus
// the live chord in parens when one answers at `context`.
[[nodiscard]] std::string toolbar_hint(
    const ws::app::Command& cmd, const ws::app::ShortcutResolver* resolver,
    ws::app::KeyContext context) {
    std::string hint = cmd.label;
    const std::string chord = command_hint(cmd, resolver, context);
    if (!chord.empty()) {
        hint += " (";
        hint += chord;
        hint += ')';
    }
    return hint;
}

}  // namespace

std::string_view command_toolbar(GuiContext& gui, LayoutCursor& layout,
                                 const ChromeTheme& theme, const char* label,
                                 const ws::app::CommandRegistry& registry,
                                 const std::vector<std::string>& ids) {
    const ScopedId scope(gui.ids(), label);
    bool first = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const ws::app::Command* cmd = registry.find(ids[i]);
        if (cmd == nullptr) {
            continue;  // an unknown id lays nothing - the builder's skip rule
        }
        // The button fits its own label (the dialog-button width
        // formula) plus a few px of slack past the fitted advance -
        // the fit's rounding has no room to clip a glyph into "Ne...",
        // so the row reads as buttons, not as equal blocks.
        char scratch[128];
        const LabelFit fit = fit_text(cmd->label.c_str(), -1.0f, theme,
                                      scratch, sizeof scratch);
        const ScopedId row(gui.ids(), static_cast<int>(i));
        if (!first) {
            layout.same_line();
        }
        first = false;
        if (button(gui, layout, theme, cmd->label.c_str(),
                   fit.advance + 2.0f * theme.padding + 4.0f, {},
                   !ws::app::CommandRegistry::is_enabled(*cmd))) {
            return cmd->id;
        }
    }
    return {};
}

std::string_view command_toolbar(
    GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
    const char* label, const ws::app::CommandRegistry& registry,
    const std::vector<std::string>& ids, const ToolbarIcon* icons,
    std::size_t icon_count, const ws::app::ShortcutResolver* resolver,
    ws::app::KeyContext context) {
    const ScopedId scope(gui.ids(), label);
    bool first = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const ws::app::Command* cmd = registry.find(ids[i]);
        if (cmd == nullptr) {
            continue;  // an unknown id lays nothing - the builder's skip rule
        }
        const bool disabled = !ws::app::CommandRegistry::is_enabled(*cmd);

        // The mapped icon, when the theme can serve its sprite: a
        // square icon button. A command without a mapping - or one
        // whose sprite came back the sentinel (a fontless theme, a
        // skin that answers nothing) - keeps the text button, so a
        // toolbar never lays a blank square.
        const ToolbarIcon* mapped = nullptr;
        for (std::size_t m = 0; m < icon_count; ++m) {
            if (icons[m].command == ids[i]) {
                mapped = &icons[m];
                break;
            }
        }
        const r2d::Sprite sprite =
            mapped != nullptr ? icon_sprite_of(theme, mapped->icon)
                              : r2d::Sprite{};
        const bool as_icon = mapped != nullptr && sprite.page != 0;

        // Both branches compose the button from the split halves (the
        // composed call's own internals) so the laid rect stays in
        // hand for the glyph centering and the tooltip's anchor. A
        // "##"-only label ids the icon button but draws no text
        // (label_display's cut) - the name moves into the tooltip.
        const char* const button_label =
            as_icon ? "##icon" : cmd->label.c_str();
        float width = theme.control_height;  // the icon button is square
        if (!as_icon) {
            char scratch[128];
            const LabelFit fit = fit_text(cmd->label.c_str(), -1.0f, theme,
                                          scratch, sizeof scratch);
            width = fit.advance + 2.0f * theme.padding + 4.0f;
        }

        const ScopedId row(gui.ids(), static_cast<int>(i));
        if (!first) {
            layout.same_line();
        }
        first = false;
        const Rect rect = layout.next(width, theme.control_height);
        const WidgetId id = gui.ids().id(button_label);
        ButtonResult result = button_behavior(gui, id, rect, disabled);
        fold_hover_t(gui, id, result, theme);
        paint(theme, &WidgetPainters::button, &button_draw, rect, result,
              theme, button_label, r2d::Sprite{});
        if (as_icon) {
            // The glyph: the height minus the corner insets (the
            // button's own icon room), centered, wearing the text
            // color the label would draw - a coverage mask tinted
            // white would vanish on the light theme - and dimmed with
            // the dead lock.
            const float inset =
                std::min(theme.corner_radius, rect.height * 0.5f);
            const float side = std::max(1.0f, rect.height - 2.0f * inset);
            draw_icon(rect.x + (rect.width - side) * 0.5f,
                      rect.y + (rect.height - side) * 0.5f, side, sprite,
                      dim(theme.text, dim_factor(result.state, theme)));
        }
        if (result.activated()) {
            return cmd->id;
        }

        // The tooltip every button wears, after the theme's rest
        // delay: the label (the icon button dropped it from the face)
        // plus the live chord. tooltip_text keys the hover clock on
        // the button's own id under this row's scope.
        const std::string hint = toolbar_hint(*cmd, resolver, context);
        tooltip_text(gui, layout, theme, button_label, rect, hint.c_str());
    }
    return {};
}

}  // namespace ws::ui::gui
