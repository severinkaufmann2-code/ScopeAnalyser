#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace scope::core {

// A small snapshot-based undo/redo history. `Snap` is a value type that
// captures a tool's full document state. It may hold serialized UI state and/or
// shared_ptr handles to heavy data (e.g. recorded Signals) so that restoring is
// cheap and never re-reads files from disk.
//
// Usage:
//   UndoStack<Snap> hist(
//       [this]{ return captureState(); },          // snapshot the live state
//       [this](const Snap& s){ restoreState(s); }, // restore into the UI
//       [this]{ refreshUndoButtons(); });          // enable/disable buttons
//   hist.reset();   // once the initial state exists, seed the baseline
//   ... after every settled edit: hist.commit();
//
// Re-entrancy: while undo()/redo() runs the apply callback, applying() returns
// true. Tools gate their commit() calls on `!applying()` so that restoring a
// snapshot doesn't record a fresh history entry.
//
// Header-only and deliberately not a QObject: it takes an onChanged callback
// (wire it to enable/disable the toolbar buttons) rather than emitting a Qt
// signal, so it can stay a template without moc.
template <class Snap>
class UndoStack {
public:
    UndoStack(std::function<Snap()> capture,
              std::function<void(const Snap&)> apply,
              std::function<void()> onChanged = {},
              int maxDepth = 50)
        : capture_(std::move(capture)),
          apply_(std::move(apply)),
          onChanged_(std::move(onChanged)),
          maxDepth_(maxDepth < 2 ? 2 : maxDepth) {}

    // Seed the baseline from the current live state and clear all history.
    void reset() {
        snaps_.clear();
        snaps_.push_back(capture_());
        cursor_ = 0;
        notify();
    }

    // Record the current live state as a new history entry, dropping any redo
    // tail. No-op while a restore is in progress (see applying()).
    void commit() {
        if (applying_) return;
        if (snaps_.empty()) { reset(); return; }
        // Drop the redo tail (everything past the cursor) — a fresh edit
        // forks a new future.
        snaps_.erase(snaps_.begin() + (cursor_ + 1), snaps_.end());
        snaps_.push_back(capture_());
        // Cap depth: forget the oldest entries so memory stays bounded.
        while (static_cast<int>(snaps_.size()) > maxDepth_)
            snaps_.erase(snaps_.begin());
        cursor_ = static_cast<int>(snaps_.size()) - 1;
        notify();
    }

    bool canUndo() const { return cursor_ > 0; }
    bool canRedo() const {
        return cursor_ >= 0 && cursor_ < static_cast<int>(snaps_.size()) - 1;
    }

    void undo() {
        if (!canUndo()) return;
        --cursor_;
        applyCurrent();
    }

    void redo() {
        if (!canRedo()) return;
        ++cursor_;
        applyCurrent();
    }

    bool applying() const { return applying_; }

    // Number of history entries (mostly for tests).
    std::size_t depth() const { return snaps_.size(); }

private:
    void applyCurrent() {
        applying_ = true;
        apply_(snaps_[static_cast<std::size_t>(cursor_)]);
        applying_ = false;
        notify();
    }
    void notify() { if (onChanged_) onChanged_(); }

    std::function<Snap()>            capture_;
    std::function<void(const Snap&)> apply_;
    std::function<void()>            onChanged_;
    int                              maxDepth_;
    std::vector<Snap>                snaps_;
    int                              cursor_{-1};
    bool                             applying_{false};
};

}  // namespace scope::core
