#include "scope/core/SignalStore.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

using namespace scope::core;

namespace {
Signal::Meta makeMeta(QString name) {
    Signal::Meta m;
    m.name = std::move(name);
    m.dataType = DataType::Float64;
    return m;
}
}

TEST(SignalStore, AddEmitsChannelAdded) {
    SignalStore store;
    QSignalSpy spy(&store, &SignalStore::channelAdded);
    store.add(std::make_shared<Signal>(makeMeta("ch1")));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(store.contains("ch1"));
}

TEST(SignalStore, RemoveEmitsChannelRemoved) {
    SignalStore store;
    store.add(std::make_shared<Signal>(makeMeta("ch1")));
    QSignalSpy spy(&store, &SignalStore::channelRemoved);
    store.remove("ch1");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_FALSE(store.contains("ch1"));
}

TEST(SignalStore, AddReplacesEmitsBothSignals) {
    SignalStore store;
    store.add(std::make_shared<Signal>(makeMeta("ch1")));
    QSignalSpy addedSpy(&store, &SignalStore::channelAdded);
    QSignalSpy removedSpy(&store, &SignalStore::channelRemoved);
    store.add(std::make_shared<Signal>(makeMeta("ch1")));
    EXPECT_EQ(removedSpy.count(), 1);
    EXPECT_EQ(addedSpy.count(), 1);
}

TEST(SignalStore, ListsChannelNames) {
    SignalStore store;
    store.add(std::make_shared<Signal>(makeMeta("a")));
    store.add(std::make_shared<Signal>(makeMeta("b")));
    auto names = store.channelNames();
    EXPECT_EQ(names.size(), 2);
}
