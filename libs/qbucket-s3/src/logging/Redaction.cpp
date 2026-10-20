#include "logging/Redaction.h"

#include <QRegularExpression>

namespace qbucket::s3::redact {

QString shortenAccessKeyId(const QString& id)
{
    if (id.size() >= 12)
        return id.left(4) + QChar(0x2026) + id.right(4);
    return id.left(2) + QChar(0x2026);
}

QByteArray headerValue(const QByteArray& name, const QByteArray& value)
{
    const QByteArray n = name.toLower();
    if (n == "authorization") {
        // AWS4-HMAC-SHA256 Credential=AKID/scope, SignedHeaders=…, Signature=hex
        QString v = QString::fromLatin1(value);
        static const QRegularExpression signature(QStringLiteral("Signature=[0-9a-fA-F]+"));
        v.replace(signature, QStringLiteral("Signature=") + QLatin1String(kRedacted));
        static const QRegularExpression credential(QStringLiteral("Credential=([^/,\\s]+)"));
        const auto m = credential.match(v);
        if (m.hasMatch()) {
            v.replace(m.capturedStart(1), m.capturedLength(1),
                      shortenAccessKeyId(m.captured(1)));
        }
        return v.toUtf8();
    }
    if (n == "x-amz-security-token"
        || n == "x-amz-server-side-encryption-customer-key"
        || n == "x-amz-copy-source-server-side-encryption-customer-key"
        || n == "proxy-authorization" || n == "cookie" || n == "set-cookie") {
        return kRedacted;
    }
    return value;
}

QString url(const QUrl& in)
{
    if (!in.hasQuery())
        return in.toDisplayString();

    QStringList parts = in.query(QUrl::FullyEncoded).split(QLatin1Char('&'));
    for (QString& part : parts) {
        const qsizetype eq = part.indexOf(QLatin1Char('='));
        const QString name = eq < 0 ? part : part.left(eq);
        if (name.compare(QLatin1String("X-Amz-Signature"), Qt::CaseInsensitive) == 0
            || name.compare(QLatin1String("X-Amz-Security-Token"), Qt::CaseInsensitive) == 0) {
            part = name + u'=' + QLatin1String(kRedacted);
        } else if (name.compare(QLatin1String("X-Amz-Credential"), Qt::CaseInsensitive) == 0
                   && eq >= 0) {
            // AKID%2F20130524%2F… — shorten the key ID part.
            const QString value = part.mid(eq + 1);
            const qsizetype slash = value.indexOf(QLatin1String("%2F"), 0, Qt::CaseInsensitive);
            const QString keyId = slash < 0 ? value : value.left(slash);
            part = name + u'=' + shortenAccessKeyId(keyId) + (slash < 0 ? QString() : value.mid(slash));
        }
    }
    // Assembled by hand: a QUrl round trip would percent-encode the marker.
    return in.adjusted(QUrl::RemoveQuery).toDisplayString() + u'?' + parts.join(QLatin1Char('&'));
}

} // namespace qbucket::s3::redact
