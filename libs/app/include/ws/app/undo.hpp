#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/app/errors.hpp>
#include <ws/core/result.hpp>

namespace ws::app {

// The host-defined unit of undoable work (the command pattern). The
// host subclasses this, captures the edit's inverse data at
// construction, and hands a unique_ptr to UndoStack::push - the
// stack owns the object from there and is the sole executor:
// apply() runs on push and on every redo, undo() on undo. Nothing
// here throws (the workspace rule); commands run on the owner
// thread.
class UndoCommand {
public:
    virtual ~UndoCommand() = default;

    // Apply the change. Runs when the stack adopts the command
    // (push - THE EDIT ITSELF: the host does not pre-apply) and on
    // every redo.
    virtual void apply() = 0;

    // Revert the change. Runs on undo, undoing exactly what apply()
    // did - including the coalesced payloads absorbed through
    // merge_with().
    virtual void undo() = 0;

    // The step's display name ("Paste", "Typing") - the views
    // compose it into "Undo Paste" / the dirty indicator's step
    // names.
    [[nodiscard]] virtual std::string name() const = 0;

    // The coalesce hook. When a command is pushed onto a non-empty
    // step, the stack offers the NEW command (already applied) to
    // the step's current last command; returning true absorbs it -
    // the newer object is destroyed and this one's undo() must
    // revert both. Type-check the argument (dynamic_cast or an id);
    // the default never merges. The offer never crosses a step
    // boundary: a transaction's first command never merges into an
    // earlier step.
    [[nodiscard]] virtual bool merge_with(const UndoCommand& next) {
        (void)next;
        return false;
    }

    // The history's memory gauge: the stack bounds itself by the
    // sum of these, so report the PAYLOAD (the captured strings,
    // the snapshot sizes). The default answers the BASE size -
    // sizeof(UndoCommand), a floor: the stack cannot see the
    // derived object's real size, so a host that carries anything
    // overrides this. After absorbing a merge_with() the estimate
    // should grow with the absorbed payload.
    [[nodiscard]] virtual std::size_t memory_estimate() const {
        return sizeof(*this);
    }
};

// The bounded undo/redo history over host-defined commands, with
// named steps, transactions, save-point dirty tracking and change
// listeners - the model underneath the Undo/Redo menu items and
// the window's dirty dot. Owner-thread state, no locks.
//
// The shape: steps_[0, index_) is the undo branch (the next undo
// runs steps_[index_-1], the next redo steps_[index_]), and
// [index_, end) is the redo branch. A push drops the redo branch
// (standard: no redo past a fork), merges when the top step
// accepts, and enforces the limits by dropping the OLDEST steps.
class UndoStack {
public:
    using ListenerId = std::int64_t;

    // Both bounds are step/memory caps; 0 = unbounded. Limits
    // never evict the newest step (an over-limit step stays until a
    // later push displaces it - an undoable present beats a
    // faithful bound).
    explicit UndoStack(std::size_t max_steps = 0,
                       std::size_t max_memory = 0);

    // Adopts the command: runs apply() (the edit lands), then
    // records it - offering it to the top step's last command for
    // coalescing, else appending a new step named by the command.
    // The redo branch is dropped, with an unreachable save point
    // (see mark_saved) going with it. The command must not be null
    // (invalid_argument).
    [[nodiscard]] ws::core::Status push(
        std::unique_ptr<UndoCommand> command);

    // Transactions group many pushes into ONE named step: begin,
    // push as often as needed, end. undo() runs the whole group's
    // commands in reverse, redo() forward. The first landed command
    // drops the redo branch (an empty transaction changes nothing
    // and leaves it standing); an empty transaction records no step
    // at all. Refuses an empty name, a begin while one is open, and
    // an end with none open (invalid_argument).
    [[nodiscard]] ws::core::Status begin_transaction(std::string name);
    [[nodiscard]] ws::core::Status end_transaction();

    // The gates the views read per frame; false while a transaction
    // is open (its step is still being assembled).
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;

    // One step back/forward. undo() runs the step's commands in
    // reverse, redo() forward through apply(). An empty branch
    // answers not_found; both refuse while a transaction is open
    // (invalid_argument - undoing mid-group would corrupt it).
    [[nodiscard]] ws::core::Status undo();
    [[nodiscard]] ws::core::Status redo();

    // The step the next undo/redo would run, "" when there is
    // none. The Undo/Redo menu items read these for their labels.
    [[nodiscard]] std::string name_to_undo() const;
    [[nodiscard]] std::string name_to_redo() const;

    [[nodiscard]] std::size_t undo_count() const noexcept;
    [[nodiscard]] std::size_t redo_count() const noexcept;

    // The save point: is_dirty() compares the current position with
    // it, so undoing back to the saved position reads clean again
    // and redoing past it reads dirty - the document's dirty flag
    // is this comparison. A push that drops the redo branch the
    // save point sits in, a limits eviction that drops it, or
    // clear() make the save point UNREACHABLE: dirty until the next
    // mark_saved() (there is no way back to the saved state, which
    // is exactly what dirty means). Refused while a transaction is
    // open (invalid_argument).
    [[nodiscard]] ws::core::Status mark_saved();
    [[nodiscard]] bool is_dirty() const noexcept;

    // Drops every step and the save point (is_dirty() is true
    // afterwards - the old save position no longer exists;
    // mark_saved() re-arms it). The usual flow on a document
    // switch: clear(), load, mark_saved(). Refused while a
    // transaction is open (invalid_argument).
    [[nodiscard]] ws::core::Status clear();

    // New bounds (0 = unbounded), enforced immediately by the same
    // oldest-first eviction as a push; the newest step always
    // survives. Refused while a transaction is open - the immediate
    // trim would bypass the assembling step (invalid_argument).
    [[nodiscard]] ws::core::Status set_limits(
        std::size_t max_steps, std::size_t max_memory);

    // The live estimates' sum - the opened transaction's commands
    // included.
    [[nodiscard]] std::size_t memory_used() const noexcept;

    // The state-change listener: fired after every push, undo,
    // redo, mark_saved, clear, set_limits and transaction that
    // changed something (can_undo/can_redo/is_dirty/counts are the
    // read side). Registration order; a listener removed during a
    // notification does not run again; one added during a
    // notification runs from the next change (the settings
    // observers' rule). An empty callback is refused
    // (invalid_argument). Listeners must not mutate the stack.
    [[nodiscard]] ws::core::Result<ListenerId> add_listener(
        std::function<void()> listener);
    bool remove_listener(ListenerId id);

private:
    struct Step {
        std::vector<std::unique_ptr<UndoCommand>> commands;
        std::string name;
    };

    // Erases the redo branch, marking a save point inside it
    // unreachable.
    void drop_redo();
    // Enforces both bounds, oldest first, keeping the newest step
    // (never called while a transaction's step is assembling -
    // push()/end_transaction() only).
    void trim_to_limits();
    void drop_front();
    void notify();

    std::vector<std::unique_ptr<Step>> steps_;
    std::unique_ptr<Step> open_step_;  // the assembling transaction
    std::size_t index_ = 0;            // steps_[0, index_) undo, [index_, end) redo
    std::size_t memory_ = 0;           // the sum of the live estimates
    std::size_t max_steps_ = 0;        // 0 = unbounded
    std::size_t max_memory_ = 0;       // 0 = unbounded
    int saved_index_ = 0;              // int: kSavePointGone is negative
    bool transaction_open_ = false;
    std::vector<std::pair<ListenerId, std::function<void()>>> listeners_;
    ListenerId next_listener_id_ = 1;
};

}  // namespace ws::app
