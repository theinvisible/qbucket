#include "client/OneShotBody.h"

#include <algorithm>
#include <cstring>

namespace qbucket::s3 {

OneShotBody::OneShotBody(QByteArray data, QObject* parent)
    : QIODevice(parent)
    , m_data(std::move(data))
{
}

bool OneShotBody::seek(qint64 pos)
{
    // QNAM rewinds only to send again. An empty body is never read from:
    // its first rewind already is that.
    if (m_sent || m_data.isEmpty() || pos < 0 || pos > m_data.size()) {
        setErrorString(QStringLiteral("the request must not be sent twice"));
        return false;
    }
    return QIODevice::seek(pos);
}

qint64 OneShotBody::readData(char* data, qint64 maxSize)
{
    m_sent = true;
    const qint64 n = std::min(maxSize, m_data.size() - pos());
    if (n <= 0)
        return 0;
    std::memcpy(data, m_data.constData() + pos(), size_t(n));
    return n;
}

} // namespace qbucket::s3
