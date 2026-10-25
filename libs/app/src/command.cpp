#include <utility>

#include <ws/app/command.hpp>

namespace ws::app {

ws::core::Status CommandRegistry::add(Command command) {
    if (command.id.empty()) {
        return fail(AppError::invalid_argument, "a command needs an id");
    }
    if (command.label.empty()) {
        return fail(AppError::invalid_argument,
                    "command '" + command.id + "' needs a label");
    }
    if (!command.execute) {
        return fail(AppError::invalid_argument,
                    "command '" + command.id + "' needs an execute callback");
    }
    if (find(command.id) != nullptr) {
        return fail(AppError::invalid_argument,
                    "command '" + command.id + "' is already registered");
    }
    commands_.push_back(std::move(command));
    return {};
}

ws::core::Status CommandRegistry::execute(std::string_view id,
                                           std::string_view argument) const {
    const Command* command = find(id);
    if (command == nullptr) {
        return fail(AppError::not_found, "no command '" + std::string{id} + "'");
    }
    command->execute(argument);
    return {};
}

const Command* CommandRegistry::find(std::string_view id) const {
    for (const Command& command : commands_) {
        if (command.id == id) {
            return &command;
        }
    }
    return nullptr;
}

bool CommandRegistry::is_enabled(const Command& command) {
    return !command.enabled || command.enabled();
}

bool CommandRegistry::is_checked(const Command& command) {
    return command.checked && command.checked();
}

}  // namespace ws::app
