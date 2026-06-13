// Unit tests for the snapshot-based undo/redo history used by the Recorder,
// Analyser, and Converter. The template is header-only and Qt-free, so these
// drive it with plain value types.

#include "scope/core/UndoStack.h"

#include <gtest/gtest.h>

#include <string>

using scope::core::UndoStack;

TEST(UndoStack, BasicUndoRedo) {
    std::string live = "a";
    UndoStack<std::string> us([&] { return live; },
                              [&](const std::string& s) { live = s; });
    us.reset();  // baseline "a"
    EXPECT_FALSE(us.canUndo());
    EXPECT_FALSE(us.canRedo());

    live = "b"; us.commit();  // s1
    live = "c"; us.commit();  // s2
    EXPECT_TRUE(us.canUndo());
    EXPECT_FALSE(us.canRedo());

    us.undo();
    EXPECT_EQ(live, "b");
    EXPECT_TRUE(us.canRedo());
    us.undo();
    EXPECT_EQ(live, "a");
    EXPECT_FALSE(us.canUndo());

    us.redo();
    EXPECT_EQ(live, "b");
    us.redo();
    EXPECT_EQ(live, "c");
    EXPECT_FALSE(us.canRedo());
}

TEST(UndoStack, CommitTruncatesRedoTail) {
    std::string live = "a";
    UndoStack<std::string> us([&] { return live; },
                              [&](const std::string& s) { live = s; });
    us.reset();
    live = "b"; us.commit();
    live = "c"; us.commit();
    us.undo();  // back at "b", redo would go to "c"
    EXPECT_TRUE(us.canRedo());

    live = "d"; us.commit();  // forks a new future; the "c" tail is dropped
    EXPECT_FALSE(us.canRedo());
    us.undo();
    EXPECT_EQ(live, "b");
    us.redo();
    EXPECT_EQ(live, "d");
}

TEST(UndoStack, ApplyingGuardBlocksReentrantCommit) {
    std::string live = "a";
    UndoStack<std::string>* self = nullptr;
    UndoStack<std::string> us(
        [&] { return live; },
        [&](const std::string& s) {
            live = s;
            // A real tool's restore re-triggers UI signals that call commit();
            // that must be a no-op while applying.
            EXPECT_TRUE(self->applying());
            self->commit();
        });
    self = &us;
    us.reset();
    live = "b"; us.commit();
    us.undo();
    EXPECT_EQ(live, "a");
    // The reentrant commit during the restore must not have recorded anything.
    EXPECT_TRUE(us.canRedo());
    us.redo();
    EXPECT_EQ(live, "b");
}

TEST(UndoStack, CappedDepthForgetsOldest) {
    int live = 0;
    UndoStack<int> us([&] { return live; }, [&](int v) { live = v; },
                      /*onChanged=*/{}, /*maxDepth=*/3);
    us.reset();             // [0]
    live = 1; us.commit();  // [0,1]
    live = 2; us.commit();  // [0,1,2]
    live = 3; us.commit();  // [0,1,2,3] -> capped to [1,2,3]
    EXPECT_EQ(us.depth(), 3u);

    us.undo(); EXPECT_EQ(live, 2);
    us.undo(); EXPECT_EQ(live, 1);
    EXPECT_FALSE(us.canUndo());  // the oldest entry (0) was forgotten
}
