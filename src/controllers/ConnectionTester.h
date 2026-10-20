#pragma once

#include "domain/Account.h"

#include <qbucket/s3/Credentials.h>
#include <qbucket/s3/Session.h>

#include <QObject>
#include <QString>

#include <memory>

namespace qbucket {

// "Test connection" for the account dialog: a throw-away Session with the
// typed-in credentials, ListBuckets — or HeadBucket on the first external
// bucket when listing isn't permitted. Read-only; never blocks the GUI.
//
// Thread affinity: GUI thread.
class ConnectionTester : public QObject {
    Q_OBJECT

public:
    explicit ConnectionTester(QObject* parent = nullptr);
    ~ConnectionTester() override;

    // Cancels a test still running, then starts a new one.
    void start(const Account& account, const QString& secretAccessKey);
    void cancel();
    [[nodiscard]] bool isRunning() const { return m_session != nullptr; }

    // Session settings for an account: provider profile from the registry,
    // the provider's endpoint for the region unless one is set. Shared with
    // the session management of M2.
    [[nodiscard]] static s3::SessionOptions sessionOptionsFor(
        const Account& account, std::shared_ptr<s3::CredentialsProvider> credentials);

    // A sentence for the user, translated, by error category.
    [[nodiscard]] static QString describe(const s3::Error& error);

signals:
    void finished(bool ok, const QString& message);

private:
    void tryExternalBucket_(const QString& bucket);
    void finish_(bool ok, const QString& message);
    void stopSession_();

    std::unique_ptr<s3::Session> m_session;
    // Context of the current run's callbacks: deleting it drops every answer
    // still on its way — a cancelled test can't touch the next one.
    std::unique_ptr<QObject> m_run;
};

} // namespace qbucket
