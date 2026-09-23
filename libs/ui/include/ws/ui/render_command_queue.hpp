#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <vector>

namespace ws::ui {

// Hazel-style render command queue: submit() type-erases a callable (plus
// whatever it captured) into a byte arena; execute() invokes the commands
// in submission order, then clears the queue. The arena keeps its
// high-water capacity across frames, so a steady frame never allocates.
//
// This is the seam between app logic and GPU work. The shell keeps two
// queues and swaps them per frame: the app fills the write queue during
// on_update(), the shell swaps and executes the read queue while the
// command buffer is being recorded. Single-threaded today, but the split
// means moving app logic to its own thread later needs no API change.
class RenderCommandQueue {
public:
    RenderCommandQueue() = default;
    RenderCommandQueue(const RenderCommandQueue&) = delete;
    RenderCommandQueue& operator=(const RenderCommandQueue&) = delete;
    RenderCommandQueue(RenderCommandQueue&&) noexcept = default;
    RenderCommandQueue& operator=(RenderCommandQueue&&) noexcept = default;

    // Queues fn, copying it and its captured state into the arena. The
    // capture must be trivially copyable: commands are byte-copied when
    // the arena grows and run within the frame they were submitted in, so
    // they must not own resources (capture by value or by pointer).
    template <typename Fn>
    void submit(Fn fn) {
        static_assert(std::is_trivially_copyable_v<Fn>,
                      "render commands must capture only trivially copyable "
                      "state - capture by value or by pointer, never by "
                      "owning resources");
        static_assert(std::is_invocable_v<Fn&>,
                      "render commands must be callable with no arguments");
        static_assert(alignof(Fn) <= alignof(std::max_align_t),
                      "render commands may not require over-alignment");
        const std::size_t offset = allocate(sizeof(Fn), alignof(Fn));
        new (storage_.data() + offset) Fn(fn);
        commands_.push_back(
            Command{&invoke_as<Fn>, static_cast<std::uint32_t>(offset)});
    }

    // Runs every queued command in submission order, then clears the queue.
    // If a command throws, the remaining commands are dropped (never re-run
    // next frame) and the exception propagates.
    void execute() {
        try {
            for (const Command& command : commands_) {
                command.invoke(storage_.data() + command.offset);
            }
        } catch (...) {
            clear();
            throw;
        }
        clear();
    }

    void clear() {
        commands_.clear();
        offset_ = 0;
    }

    bool empty() const { return commands_.empty(); }

private:
    // A queued command: an invoke trampoline plus the offset of its storage
    // inside the arena. Offsets, not pointers, so growing the arena never
    // invalidates them.
    struct Command {
        void (*invoke)(unsigned char* storage);
        std::uint32_t offset;
    };

    template <typename Fn>
    static void invoke_as(unsigned char* storage) {
        (*reinterpret_cast<Fn*>(storage))();
    }

    // Bump-allocates `size` bytes aligned to `align` inside the arena,
    // growing it when needed. resize() preserves already-queued bytes, so
    // earlier offsets stay valid.
    std::size_t allocate(std::size_t size, std::size_t align) {
        const std::size_t start = (offset_ + align - 1) & ~(align - 1);
        const std::size_t needed = start + size;
        if (needed > storage_.size()) {
            storage_.resize(storage_.empty() ? needed
                                             : std::max(needed, storage_.size() * 2));
        }
        offset_ = start + size;
        return start;
    }

    std::vector<unsigned char> storage_;  // arena; capacity = high-water mark
    std::vector<Command> commands_;
    std::size_t offset_ = 0;               // bump cursor into storage_
};

}  // namespace ws::ui
