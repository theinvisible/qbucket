#include "core/Mailbox.h"

#include <QMetaObject>
#include <QThread>

namespace qbucket::s3 {

Mailbox::Mailbox()
    : m_receiver(new QObject)
{
}

Mailbox::~Mailbox()
{
    // Anything still queued for the receiver is dropped with it. On the
    // receiver's own thread delete right away: the owner is typically torn
    // down in a thread's final deferred-delete pass, and a deleteLater()
    // posted then is never processed — the receiver would leak and keep the
    // whole thread's data alive.
    if (m_receiver->thread() == QThread::currentThread())
        delete m_receiver;
    else
        m_receiver->deleteLater();
}

std::shared_ptr<Mailbox> Mailbox::create()
{
    return std::shared_ptr<Mailbox>(new Mailbox);
}

void Mailbox::post(const std::weak_ptr<Mailbox>& box, std::function<void()> fn)
{
    if (const std::shared_ptr<Mailbox> alive = box.lock())
        QMetaObject::invokeMethod(alive->m_receiver, std::move(fn), Qt::QueuedConnection);
}

} // namespace qbucket::s3
