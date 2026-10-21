#pragma once

#include "controllers/OperationController.h"

#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QUuid>

#include <functional>

namespace qbucket {

// Presigned links (PLAN.md §6 M4, §12.3). Signing sends nothing to the
// server, so it isn't a Job — but the policy lives here, not in the UI:
//   - GET links only read: allowed on read-only accounts; PUT links let
//     anyone holding them upload: only where the account may write;
//   - the expiry is capped by the provider (SigV4: 7 days);
//   - long-lived and PUT links come with a warning;
//   - a PUT link is journaled (key, expiry — never the URL).
// GUI thread only.
class PresignService : public QObject {
    Q_OBJECT

public:
    struct Request {
        QUuid account;
        QString bucket;
        QStringList keys;
        // GET: a version of each key (same order; empty: the current one).
        QStringList versionIds;
        QByteArray method = "GET";
        int expiresSeconds = 3600;
        // GET: the browser saves instead of showing (Content-Disposition
        // attachment with the object's name).
        bool download = false;
    };
    struct Link {
        QString key;
        QUrl url;           // invalid on error
        QString error;
    };
    static constexpr int kLongLivedSecs = 24 * 3600;

    explicit PresignService(OperationController* operations, QObject* parent = nullptr);

    [[nodiscard]] OperationController::Check check(const QUuid& account, const QString& bucket,
                                                   const QByteArray& method) const;
    [[nodiscard]] int maxExpirySeconds(const QUuid& account) const;
    [[nodiscard]] static QStringList warnings(const QByteArray& method, int expiresSeconds);
    // Every key signed (in order); `done` once, on the context's thread.
    void presign(const Request& request, QObject* context, std::function<void(const QList<Link>&)> done);

private:
    OperationController* m_ops;
};

} // namespace qbucket
