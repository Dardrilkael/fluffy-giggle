#include <cstddef>
#include <utility>

#include <ws/app/undo.hpp>

namespace ws::app {
namespace {

// The unreachable save point (a push past it, an eviction, clear()).
constexpr int kSavePointGone = -1;

}  // namespace

UndoStack::UndoStack(std::size_t max_steps, std::size_t max_memory)
    : max_steps_(max_steps), max_memory_(max_memory) {}

ws::core::Status UndoStack::push(std::unique_ptr<UndoCommand> command) {
    if (command == nullptr) {
        return fail(AppError::invalid_argument, "push needs a command");
    }
    // The edit lands first; recording is bookkeeping around it.
    command->apply();
    drop_redo();

    // The coalescing target: the open transaction's step, or the
    // top of the undo branch. Never across a step boundary; null
    // when there is nothing to offer to.
    std::vector<std::unique_ptr<UndoCommand>>* target = nullptr;
    if (transaction_open_) {
        target = &open_step_->commands;
    } else if (index_ > 0) {
        target = &steps_[index_ - 1]->commands;
    }
    bool absorbed = false;
    if (target != nullptr && !target->empty()) {
        UndoCommand* top = target->back().get();
        const std::size_t before = top->memory_estimate();
        if (top->merge_with(*command)) {
            // The top now carries the absorbed payload too - its
            // estimate says how big that made it.
            memory_ = memory_ - before + top->memory_estimate();
            absorbed = true;  // the newer object dies here, fully applied
        }
    }
    if (!absorbed) {
        memory_ += command->memory_estimate();
        if (transaction_open_) {
            open_step_->commands.push_back(std::move(command));
            // No step lands yet - trim waits for end_transaction -
            // but memory grew and the redo branch may have dropped:
            // the listener reads run now.
            notify();
            return {};
        }
        std::string name = command->name();
        auto step = std::make_unique<Step>();
        step->name = std::move(name);
        step->commands.push_back(std::move(command));
        steps_.push_back(std::move(step));
        index_ = steps_.size();
    }
    // The bound is enforced on every landed change - a merge grew
    // the newest step's estimate too - but never mid-transaction:
    // the assembling step is not on steps_ yet (end_transaction
    // trims when it lands).
    if (!transaction_open_) {
        trim_to_limits();
    }
    notify();
    return {};
}

ws::core::Status UndoStack::begin_transaction(std::string name) {
    if (transaction_open_) {
        return fail(AppError::invalid_argument,
                    "a transaction is already open");
    }
    if (name.empty()) {
        return fail(AppError::invalid_argument, "a transaction needs a name");
    }
    transaction_open_ = true;
    open_step_ = std::make_unique<Step>();
    open_step_->name = std::move(name);
    return {};
}

ws::core::Status UndoStack::end_transaction() {
    if (!transaction_open_) {
        return fail(AppError::invalid_argument, "no transaction is open");
    }
    transaction_open_ = false;
    if (open_step_->commands.empty()) {
        open_step_.reset();  // nothing happened: no step, the redo branch stands
        return {};
    }
    steps_.push_back(std::move(open_step_));
    open_step_.reset();
    index_ = steps_.size();
    trim_to_limits();
    notify();
    return {};
}

ws::core::Status UndoStack::undo() {
    if (transaction_open_) {
        return fail(AppError::invalid_argument, "a transaction is open");
    }
    if (index_ == 0) {
        return fail(AppError::not_found, "nothing to undo");
    }
    Step& step = *steps_[index_ - 1];
    for (std::size_t i = step.commands.size(); i-- > 0;) {
        step.commands[i]->undo();  // reverse: the last edit reverts first
    }
    --index_;
    notify();
    return {};
}

ws::core::Status UndoStack::redo() {
    if (transaction_open_) {
        return fail(AppError::invalid_argument, "a transaction is open");
    }
    if (index_ == steps_.size()) {
        return fail(AppError::not_found, "nothing to redo");
    }
    Step& step = *steps_[index_];
    for (std::size_t i = 0; i < step.commands.size(); ++i) {
        step.commands[i]->apply();  // forward: the first edit lands first
    }
    ++index_;
    notify();
    return {};
}

bool UndoStack::can_undo() const noexcept {
    return !transaction_open_ && index_ > 0;
}

bool UndoStack::can_redo() const noexcept {
    return !transaction_open_ && index_ < steps_.size();
}

std::string UndoStack::name_to_undo() const {
    if (transaction_open_ || index_ == 0) {
        return {};
    }
    return steps_[index_ - 1]->name;
}

std::string UndoStack::name_to_redo() const {
    if (transaction_open_ || index_ == steps_.size()) {
        return {};
    }
    return steps_[index_]->name;
}

std::size_t UndoStack::undo_count() const noexcept {
    return index_;
}

std::size_t UndoStack::redo_count() const noexcept {
    return steps_.size() - index_;
}

ws::core::Status UndoStack::mark_saved() {
    if (transaction_open_) {
        return fail(AppError::invalid_argument, "a transaction is open");
    }
    saved_index_ = static_cast<int>(index_);
    notify();
    return {};
}

bool UndoStack::is_dirty() const noexcept {
    return saved_index_ == kSavePointGone ||
           saved_index_ != static_cast<int>(index_);
}

ws::core::Status UndoStack::clear() {
    if (transaction_open_) {
        return fail(AppError::invalid_argument, "a transaction is open");
    }
    steps_.clear();
    memory_ = 0;
    index_ = 0;
    saved_index_ = kSavePointGone;  // the old save position no longer exists
    notify();
    return {};
}

ws::core::Status UndoStack::set_limits(std::size_t max_steps,
                                        std::size_t max_memory) {
    if (transaction_open_) {
        return fail(AppError::invalid_argument, "a transaction is open");
    }
    max_steps_ = max_steps;
    max_memory_ = max_memory;
    trim_to_limits();
    notify();
    return {};
}

std::size_t UndoStack::memory_used() const noexcept {
    return memory_;
}

ws::core::Result<UndoStack::ListenerId> UndoStack::add_listener(
    std::function<void()> listener) {
    if (listener == nullptr) {
        return fail(AppError::invalid_argument, "a listener needs a callback");
    }
    const ListenerId id = next_listener_id_++;
    listeners_.push_back({id, std::move(listener)});
    return id;
}

bool UndoStack::remove_listener(ListenerId id) {
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->first == id) {
            listeners_.erase(it);
            return true;
        }
    }
    return false;
}

void UndoStack::drop_redo() {
    if (index_ == steps_.size()) {
        return;
    }
    // A save point that includes any dropped step has no way back -
    // but one AT the boundary (the current position) uses only
    // undo-branch steps and survives: push makes it dirty, one
    // undo reads clean again.
    if (saved_index_ != kSavePointGone &&
        saved_index_ > static_cast<int>(index_)) {
        saved_index_ = kSavePointGone;
    }
    for (std::size_t i = index_; i < steps_.size(); ++i) {
        for (const auto& command : steps_[i]->commands) {
            memory_ -= command->memory_estimate();
        }
    }
    steps_.resize(index_);
}

void UndoStack::trim_to_limits() {
    while (steps_.size() > 1 &&
           ((max_steps_ > 0 && steps_.size() > max_steps_) ||
            (max_memory_ > 0 && memory_ > max_memory_))) {
        drop_front();
    }
}

void UndoStack::drop_front() {
    for (const auto& command : steps_.front()->commands) {
        memory_ -= command->memory_estimate();
    }
    steps_.erase(steps_.begin());
    // The save point is a POSITION over steps 0..s-1. An APPLIED
    // dropped step becomes part of the permanent base, so every
    // position from 1 up shifts down one slot (position 0 - pure
    // base - is then unreachable). An UNAPPLIED dropped step is in
    // the redo branch: no state that included it can be rebuilt,
    // so only the untouched base at position 0 survives.
    if (saved_index_ != kSavePointGone) {
        if (index_ > 0) {
            if (saved_index_ > 0) {
                --saved_index_;
            } else {
                saved_index_ = kSavePointGone;
            }
        } else if (saved_index_ > 0) {
            saved_index_ = kSavePointGone;
        }
    }
    if (index_ > 0) {
        --index_;
    }
}

void UndoStack::notify() {
    if (listeners_.empty()) {
        return;
    }
    // The settings-observer idiom: walk a snapshot, liveness-check
    // each id against the live list so a removal mid-notify takes
    // effect immediately and an addition mid-notify waits for the
    // next change.
    const std::vector<std::pair<ListenerId, std::function<void()>>>
        snapshot = listeners_;
    for (const auto& entry : snapshot) {
        bool live = false;
        for (const auto& candidate : listeners_) {
            if (candidate.first == entry.first) {
                live = true;
                break;
            }
        }
        if (live) {
            entry.second();
        }
    }
}

}  // namespace ws::app
