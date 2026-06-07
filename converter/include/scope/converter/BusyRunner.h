#pragma once

#include <QEventLoop>
#include <QFutureWatcher>
#include <QObject>
#include <QProgressDialog>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QtConcurrent>

#include <type_traits>
#include <utility>

namespace scope::converter::ui {

// Run a blocking `work()` on a worker thread while a modal, indeterminate
// "busy" dialog keeps the GUI painting — so a slow file open / save looks
// like progress rather than a frozen window. Returns work()'s result on the
// GUI thread once it finishes.
//
// Shared by the Analyser and the Converter so the two tools stay in lockstep:
// both drive the same SignalIO entry points (loadFile / saveChartFromStore),
// and now both wrap them the same way. Running IO off the GUI thread is safe —
// the data layer is built for concurrent reads (SignalStore and Signal are
// independently thread-safe), and loadFile only ever touches local file data
// plus freshly-created Signals. No Cancel button: a half-written .h5 / .mf4
// isn't safely abortable.
template <typename Work>
auto runWithBusyDialog(QWidget* parent, const QString& label, Work work)
    -> std::invoke_result_t<Work> {
    using Result = std::invoke_result_t<Work>;

    QProgressDialog dlg(label, QString(), 0, 0, parent);  // min==max==0 → busy
    dlg.setWindowTitle(QObject::tr("Please wait"));
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setCancelButton(nullptr);
    // Suppress QProgressDialog's own value-driven auto-show; we decide when to
    // surface it (after a short grace period) so quick ops don't flash.
    dlg.setMinimumDuration(100000);

    QFutureWatcher<Result> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<Result>::finished,
                     &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run(std::move(work)));

    if (!watcher.isFinished()) {
        // Surface the dialog only if the work is still running after a brief
        // grace period; otherwise a fast open/save never shows anything. The
        // finished signal (delivered on this thread's event loop) is what
        // actually ends loop.exec() — the timer is purely cosmetic.
        QTimer::singleShot(200, &dlg, [&dlg, &watcher] {
            if (!watcher.isFinished()) dlg.show();
        });
        loop.exec();
    }
    return watcher.result();
}

}  // namespace scope::converter::ui
