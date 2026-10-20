#pragma once

#include <QObject>
#include <QPointer>
#include <QThreadPool>

#include <functional>
#include <utility>

namespace qbucket::s3::transfer::detail {

// Runs `work` on `pool` and `done(result)` on `context`'s thread, dropped
// when the context is gone. A relay object in the context's thread carries
// the result and deletes itself there — nobody else deletes it, so the
// worker can always post to it. Call on the context's thread.
template <typename T>
void runAsync(QThreadPool* pool, QObject* context, std::function<T()> work, std::function<void(T)> done)
{
    auto* relay = new QObject;
    relay->moveToThread(context->thread());
    const QPointer<QObject> guard(context);
    pool->start([relay, guard, work = std::move(work), done = std::move(done)]() mutable {
        T result = work();
        QMetaObject::invokeMethod(
            relay,
            [relay, guard, done = std::move(done), result = std::move(result)]() mutable {
                if (guard)
                    done(std::move(result));
                relay->deleteLater();
            },
            Qt::QueuedConnection);
    });
}

// One thread, tasks in submission order: resume-store writes must not
// overtake each other (a part record before its transfer's record).
QThreadPool* serialStorePool();

} // namespace qbucket::s3::transfer::detail
