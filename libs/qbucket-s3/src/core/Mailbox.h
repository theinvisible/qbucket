#pragma once

#include <qbucket/s3/Export.h>

#include <QObject>

#include <functional>
#include <memory>

namespace qbucket::s3 {

// Delivers functions into the thread a Mailbox was created in, from any
// thread, without ever touching a dangling QObject: posters hold only a
// weak_ptr, and while post() holds the lock the receiver cannot be deleted.
// Functions posted after the owner dropped the mailbox are discarded.
//
// Typical use: a callback that may arrive on a foreign thread (credentials
// from a keychain) must continue on the client's thread.
class QBUCKET_S3_EXPORT Mailbox {
public:
    // Receives in the calling thread.
    [[nodiscard]] static std::shared_ptr<Mailbox> create();

    // Thread-safe. Queues fn for the mailbox thread; no-op once the mailbox
    // is gone. fn must itself check that whatever it touches still exists.
    static void post(const std::weak_ptr<Mailbox>& box, std::function<void()> fn);

    ~Mailbox();
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

private:
    Mailbox();
    QObject* m_receiver;
};

} // namespace qbucket::s3
