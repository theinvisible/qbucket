#pragma once

#include <QFile>
#include <QIODevice>

namespace qbucket::s3 {

// Read-only view of [offset, offset + length) of a file, as an upload body
// for QNAM: streamed from disk, nothing buffered beyond what QNAM reads
// ahead. A file that turns out shorter than the range (truncated after it
// was hashed) makes reads fail; failed() then says so.
//
// Thread affinity: like any QIODevice — the thread that uses it.
class FileRangeDevice final : public QIODevice {
public:
    FileRangeDevice(const QString& path, qint64 offset, qint64 length, QObject* parent = nullptr);

    // Opens the file read-only and positions at the range start.
    bool open(OpenMode mode) override;
    void close() override;

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override { return m_length; }
    bool seek(qint64 pos) override;
    [[nodiscard]] bool atEnd() const override;

    [[nodiscard]] bool failed() const { return m_failed; }
    [[nodiscard]] QString failure() const { return m_failure; }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    void fail(const QString& why);

    QFile m_file;
    qint64 m_offset;
    qint64 m_length;
    bool m_failed = false;
    QString m_failure;
};

} // namespace qbucket::s3
