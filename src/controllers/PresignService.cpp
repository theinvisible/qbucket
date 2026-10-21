#include "controllers/PresignService.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"
#include "controllers/Journal.h"

#include <qbucket/s3/Presigner.h>
#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <memory>

namespace qbucket {

PresignService::PresignService(OperationController* operations, QObject* parent)
    : QObject(parent),
      m_ops(operations)
{
}

OperationController::Check PresignService::check(const QUuid& account, const QString& bucket,
                                                 const QByteArray& method) const
{
    // A PUT link lets its holder write: like a write of the account's.
    return m_ops->check(account, bucket, {s3::Feature::Presign}, false, method != "GET");
}

int PresignService::maxExpirySeconds(const QUuid& account) const
{
    const std::optional<Account> a = m_ops->accounts()->account(account);
    const auto profile = s3::ProviderRegistry::builtin().findOrGeneric(a ? a->providerId : QString());
    return std::min(profile->maxPresignExpirySeconds(), s3::Presigner::kMaxExpirySeconds);
}

QStringList PresignService::warnings(const QByteArray& method, int expiresSeconds)
{
    QStringList out;
    if (method != "GET")
        out.append(tr("Anyone holding a PUT link can upload to that key — and replace what is there — until it "
                      "expires. It can't be revoked except by changing the account's keys."));
    if (expiresSeconds > kLongLivedSecs)
        out.append(tr("The link stays valid for more than a day; anyone it reaches can use it all that time."));
    return out;
}

void PresignService::presign(const Request& request, QObject* context, std::function<void(const QList<Link>&)> done)
{
    struct State {
        QList<Link> links;
        int pending = 0;
        std::function<void(const QList<Link>&)> done;
    };
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    for (const QString& key : request.keys)
        state->links.append({key, {}, {}});
    const auto finishOne = [state] {
        if (--state->pending == 0)
            state->done(state->links);
    };
    const OperationController::Check gate = check(request.account, request.bucket, request.method);
    const s3::Result<s3::Session*> session = m_ops->sessions()->session(request.account);
    if (!gate.allowed || !session || request.keys.isEmpty()) {
        const QString why = !gate.allowed ? gate.reason
                            : !session    ? ConnectionTester::describe(session.error())
                                          : tr("Nothing selected.");
        for (Link& l : state->links)
            l.error = why;
        QMetaObject::invokeMethod(context, [state] { state->done(state->links); }, Qt::QueuedConnection);
        return;
    }
    state->pending = int(request.keys.size());
    for (qsizetype i = 0; i < request.keys.size(); ++i) {
        s3::PresignRequest p;
        p.method = request.method;
        p.bucket = request.bucket;
        p.key = request.keys[i];
        p.expiresSeconds = request.expiresSeconds;
        if (request.download && request.method == "GET") {
            const QString name = p.key.mid(p.key.lastIndexOf(u'/') + 1);
            QString ascii;
            for (const QChar c : name)
                ascii += (c.unicode() >= 0x20 && c.unicode() < 0x7F && c != u'"' && c != u'\\') ? c : QChar(u'_');
            p.responseContentDisposition = QStringLiteral("attachment; filename=\"%1\"").arg(ascii);
        }
        (*session)->presign(p, context, [state, i, finishOne](const s3::Result<QUrl>& url) {
            if (url)
                state->links[i].url = *url;
            else
                state->links[i].error = ConnectionTester::describe(url.error());
            finishOne();
        });
    }
    if (request.method != "GET") {
        // Who handed out write access, and until when (never the URL).
        const std::optional<Account> a = m_ops->accounts()->account(request.account);
        QList<Journal::Record> records;
        for (const QString& key : request.keys) {
            Journal::Record r;
            r.account = request.account;
            r.accountName = a ? a->name : QString();
            r.environment = a ? a->environment : Environment::Development;
            r.bucket = request.bucket;
            r.key = key;
            r.operation = QStringLiteral("PresignPut");
            r.message = QStringLiteral("valid for %1 s").arg(request.expiresSeconds);
            records.append(r);
        }
        m_ops->journal()->append(records);
    }
}

} // namespace qbucket
