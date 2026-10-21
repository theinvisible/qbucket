#include <qbucket/s3/AwsConfigFiles.h>

#include "core/AwsIni.h"

#include <QDir>

namespace qbucket::s3 {

namespace detail {

QList<AwsIniSection> parseAwsIni(const QByteArray& content, bool configSpelling)
{
    QList<AwsIniSection> out;
    AwsIniSection* current = nullptr;
    QByteArray nestedKey;   // set while indented lines belong to "key ="
    for (const QByteArray& raw : content.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        const bool indented = raw.startsWith(' ') || raw.startsWith('\t');
        if (line.startsWith('[') && line.endsWith(']')) {
            nestedKey.clear();
            const QString header = QString::fromUtf8(line.mid(1, line.size() - 2)).simplified();
            AwsIniSection s;
            const qsizetype space = header.indexOf(u' ');
            const QString first = space < 0 ? header : header.left(space);
            const QString rest = space < 0 ? QString() : header.mid(space + 1);
            if (first == QLatin1String("profile") && !rest.isEmpty()) {
                s.kind = QStringLiteral("profile");
                s.name = rest;
            } else if (!configSpelling || header == QLatin1String("default")) {
                s.kind = QStringLiteral("profile");
                s.name = header;
            } else if (!rest.isEmpty()) {
                s.kind = first;                       // services, sso-session, …
                s.name = rest;
            } else {
                s.kind = QStringLiteral("other");
                s.name = header;
            }
            out.append(s);
            current = &out.last();
            continue;
        }
        if (!current)
            continue;
        const qsizetype eq = line.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray name = line.left(eq).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(eq + 1).trimmed());
        if (indented && !nestedKey.isEmpty()) {
            current->nested[nestedKey].insert(name, value);
            continue;
        }
        nestedKey.clear();
        if (value.isEmpty()) {
            nestedKey = name;                         // "s3 =" opens a nested block
            current->nested[nestedKey];
            continue;
        }
        current->values.insert(name, value);
    }
    return out;
}

} // namespace detail

namespace {

AwsProfileEntry* entryFor(QList<AwsProfileEntry>& entries, const QString& name)
{
    for (AwsProfileEntry& e : entries) {
        if (e.name == name)
            return &e;
    }
    AwsProfileEntry e;
    e.name = name;
    entries.append(e);
    return &entries.last();
}

void takeKeys(AwsProfileEntry* e, const detail::AwsIniSection& s, bool overwrite)
{
    const auto take = [&](QString* field, const char* key) {
        const QString v = s.values.value(QByteArray(key));
        if (!v.isEmpty() && (overwrite || field->isEmpty()))
            *field = v;
    };
    take(&e->accessKeyId, "aws_access_key_id");
    take(&e->secretAccessKey, "aws_secret_access_key");
    take(&e->sessionToken, "aws_session_token");
}

} // namespace

QString AwsConfigFiles::credentialsPath()
{
    QString fromEnv = qEnvironmentVariable("AWS_SHARED_CREDENTIALS_FILE");
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QDir::homePath() + QStringLiteral("/.aws/credentials");
}

QString AwsConfigFiles::configPath()
{
    QString fromEnv = qEnvironmentVariable("AWS_CONFIG_FILE");
    if (!fromEnv.isEmpty())
        return fromEnv;
    return QDir::homePath() + QStringLiteral("/.aws/config");
}

QList<AwsProfileEntry> AwsConfigFiles::parse(const QByteArray& credentials, const QByteArray& config)
{
    QList<AwsProfileEntry> out;
    for (const detail::AwsIniSection& s : detail::parseAwsIni(credentials, false))
        takeKeys(entryFor(out, s.name), s, true);

    const QList<detail::AwsIniSection> sections = detail::parseAwsIni(config, true);
    const auto servicesS3Endpoint = [&](const QString& name) {
        for (const detail::AwsIniSection& s : sections) {
            if (s.kind == QLatin1String("services") && s.name == name)
                return s.nested.value("s3").value("endpoint_url");
        }
        return QString();
    };
    static const QList<QByteArray> kIndirect = {"credential_process", "role_arn", "source_profile",
                                                "web_identity_token_file", "sso_session", "sso_start_url",
                                                "sso_account_id", "sso_role_name"};
    for (const detail::AwsIniSection& s : sections) {
        if (s.kind != QLatin1String("profile"))
            continue;
        AwsProfileEntry* e = entryFor(out, s.name);
        takeKeys(e, s, false);
        e->region = s.values.value("region");
        const QMap<QByteArray, QString> s3 = s.nested.value("s3");
        QString endpoint = s3.value("endpoint_url");
        if (endpoint.isEmpty() && s.values.contains("services"))
            endpoint = servicesS3Endpoint(s.values.value("services"));
        if (endpoint.isEmpty())
            endpoint = s.values.value("endpoint_url");
        e->endpointUrl = endpoint;
        e->addressingStyle = s3.value("addressing_style").toLower();
        for (const QByteArray& key : kIndirect) {
            if (s.values.contains(key))
                e->indirectCredentials.append(QString::fromLatin1(key));
        }
    }
    return out;
}

} // namespace qbucket::s3
