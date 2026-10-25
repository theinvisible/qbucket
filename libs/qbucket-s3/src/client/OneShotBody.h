#pragma once

#include <QByteArray>
#include <QIODevice>

namespace qbucket::s3 {

// In-memory upload body of a request that must reach the server at most
// once (CopyObject, CompleteMultipartUpload, CreateBucket).
//
// QNAM sends a request again by itself when the connection closes before
// the response — any method (QHttpNetworkConnectionChannel; seen with
// Qt 6.12 on Windows) — and rewinds the body first. This body refuses that
// rewind, so QNAM gives up with ContentReSendError instead of sending the
// request a second time: a rewind once anything was read, and any rewind of
// an empty body (never read from; QNAM rewinds it only for the resend). The
// library's own retries build a fresh body for every attempt.
//
// Thread affinity: like any QIODevice — the thread that uses it.
class OneShotBody final : public QIODevice {
public:
    explicit OneShotBody(QByteArray data, QObject* parent = nullptr);

    [[nodiscard]] bool isSequential() const override { return false; }
    [[nodiscard]] qint64 size() const override { return m_data.size(); }
    bool seek(qint64 pos) override;
    [[nodiscard]] bool atEnd() const override { return pos() >= m_data.size(); }

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QByteArray m_data;
    bool m_sent = false;
};

} // namespace qbucket::s3
