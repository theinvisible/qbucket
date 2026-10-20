#include "core/Mailbox.h"

#include <QMetaObject>

namespace qbucket::s3 {

Mailbox::Mailbox()
    : m_receiver(new QObject)
{
}

Mailbox::~Mailbox()
{
    // Thread-safe; anything still queued for the receiver is dropped with it.
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
