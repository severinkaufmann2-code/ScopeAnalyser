#include "scope/analyser/FormulaEngine.h"
#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

TEST(FormulaEngine, Phase1Stub) {
    scope::core::SignalStore store;
    scope::analyser::FormulaEngine engine(store);
    QString err;
    EXPECT_FALSE(engine.evaluate("Out = Ch1 + 1", &err));
    EXPECT_FALSE(err.isEmpty());
}
