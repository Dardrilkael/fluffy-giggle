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
        // formula), so the row reads as buttons, not as equal blocks.
        char scratch[128];
        const LabelFit fit = fit_text(cmd->label.c_str(), -1.0f, theme,
                                      scratch, sizeof scratch);
        const ScopedId row(gui.ids(), static_cast<int>(i));
        if (!first) {
            layout.same_line();
        }
        first = false;
        if (button(gui, layout, theme, cmd->label.c_str(),
                   fit.advance + 2.0f * theme.padding, {},
                   !ws::app::CommandRegistry::is_enabled(*cmd))) {
            return cmd->id;
        }
    }
    return {};
}

}  // namespace ws::ui::gui
