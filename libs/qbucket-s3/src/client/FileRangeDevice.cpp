#include "client/FileRangeDevice.h"

#include <algorithm>

namespace qbucket::s3 {

FileRangeDevice::FileRangeDevice(const QString& path, qint64 offset, qint64 length, QObject* parent)
    : QIODevice(parent)
    , m_file(path)
    , m_offset(offset)
    , m_length(length)
{
}

bool FileRangeDevice::open(OpenMode mode)
{
    if ((mode & WriteOnly) != 0 || m_offset < 0 || m_length < 0) {
        fail(QStringLiteral("invalid file range"));
        return false;
    }
    if (!m_file.open(QIODevice::ReadOnly)) {
        fail(m_file.errorString());
        return false;
    }
    if (m_file.size() < m_offset + m_length) {
        fail(QStringLiteral("the file is shorter than expected (%1 bytes, need %2)")
                 .arg(m_file.size())
                 .arg(m_offset + m_length));
        m_file.close();
        return false;
    }
    if (!m_file.seek(m_offset)) {
        fail(m_file.errorString());
        m_file.close();
        return false;
    }
    return QIODevice::open(mode | Unbuffered);
}

void FileRangeDevice::close()
{
    QIODevice::close();
    m_file.close();
}

bool FileRangeDevice::seek(qint64 pos)
{
    if (pos < 0 || pos > m_length || !m_file.isOpen())
        return false;
    if (!m_file.seek(m_offset + pos)) {
        fail(m_file.errorString());
        return false;
    }
    return QIODevice::seek(pos);
}

bool FileRangeDevice::atEnd() const
{
    return pos() >= m_length;
}

qint64 FileRangeDevice::readData(char* data, qint64 maxSize)
{
    const qint64 left = m_length - pos();
    if (left <= 0)
        return 0;
    const qint64 n = m_file.read(data, std::min(maxSize, left));
    if (n < 0) {
        fail(m_file.errorString());
        return -1;
    }
    if (n == 0) {   // EOF before the range ended: the file shrank
        fail(QStringLiteral("the file became shorter while it was being sent"));
        return -1;
    }
    return n;
}

void FileRangeDevice::fail(const QString& why)
{
    m_failed = true;
    m_failure = QStringLiteral("%1: %2").arg(m_file.fileName(), why);
    setErrorString(m_failure);
}

} // namespace qbucket::s3
