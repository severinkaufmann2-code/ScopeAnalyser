#include "scope/core/SignalStore.h"

namespace scope::core {

SignalStore::SignalStore(QObject* parent) : QObject(parent) {}

void SignalStore::add(std::shared_ptr<Signal> signal) {
    if (!signal) return;
    const QString name = signal->meta().name;
    bool replaced = false;
    {
        std::unique_lock lock(mtx_);
        if (auto it = map_.find(name); it != map_.end()) {
            replaced = true;
        }
        map_.insert_or_assign(name, std::move(signal));
    }
    if (replaced) emit channelRemoved(name);
    emit channelAdded(name);
}

bool SignalStore::rename(const QString& from, const QString& to) {
    if (from == to || to.trimmed().isEmpty()) return false;
    std::shared_ptr<Signal> copy;
    {
        std::unique_lock lock(mtx_);
        auto it = map_.find(from);
        if (it == map_.end() || map_.count(to) > 0) return false;
        const auto view = it->second->snapshotForRead();
        auto meta = it->second->meta();
        meta.name = to;
        copy = std::make_shared<Signal>(meta);
        if (view.count > 0) copy->append(view.timestamps, view.values, view.count);
        map_.erase(it);
        map_.insert_or_assign(to, copy);
    }
    emit channelRenamed(from, to);
    return true;
}

void SignalStore::remove(const QString& name) {
    bool existed = false;
    {
        std::unique_lock lock(mtx_);
        existed = map_.erase(name) > 0;
    }
    if (existed) emit channelRemoved(name);
}

void SignalStore::clear() {
    QStringList removed;
    {
        std::unique_lock lock(mtx_);
        removed.reserve(static_cast<int>(map_.size()));
        for (auto& [k, _] : map_) removed.push_back(k);
        map_.clear();
    }
    for (const auto& name : removed) emit channelRemoved(name);
}

std::shared_ptr<Signal> SignalStore::get(const QString& name) const {
    std::shared_lock lock(mtx_);
    auto it = map_.find(name);
    return it == map_.end() ? nullptr : it->second;
}

bool SignalStore::contains(const QString& name) const {
    std::shared_lock lock(mtx_);
    return map_.find(name) != map_.end();
}

QStringList SignalStore::channelNames() const {
    std::shared_lock lock(mtx_);
    QStringList names;
    names.reserve(static_cast<int>(map_.size()));
    for (const auto& [k, _] : map_) names.push_back(k);
    return names;
}

std::size_t SignalStore::size() const {
    std::shared_lock lock(mtx_);
    return map_.size();
}

}  // namespace scope::core
