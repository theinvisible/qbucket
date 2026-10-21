#include "accounts/RcloneConfigReader.h"

#include <QDir>
#include <QMap>

namespace qbucket {

RcloneConfigReader::Result RcloneConfigReader::parse(const QByteArray& content)
{
    Result out;
    if (content.trimmed().startsWith("RCLONE_ENCRYPT_V0:")) {
        out.encrypted = true;
        return out;
    }
    struct Section {
        QString name;
        QMap<QByteArray, QString> values;
    };
    QList<Section> sections;
    for (const QByteArray& raw : content.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('[') && line.endsWith(']')) {
            sections.append({QString::fromUtf8(line.mid(1, line.size() - 2)).trimmed(), {}});
            continue;
        }
        const qsizetype eq = line.indexOf('=');
        if (eq < 0 || sections.isEmpty())
            continue;
        sections.last().values.insert(line.left(eq).trimmed().toLower(), QString::fromUtf8(line.mid(eq + 1).trimmed()));
    }
    const auto truth = [](const QString& v) -> std::optional<bool> {
        const QString t = v.trimmed().toLower();
        if (t == QLatin1String("true") || t == QLatin1String("1"))
            return true;
        if (t == QLatin1String("false") || t == QLatin1String("0"))
            return false;
        return std::nullopt;
    };
    for (const Section& s : std::as_const(sections)) {
        if (s.values.value("type").toLower() != QLatin1String("s3"))
            continue;
        RcloneRemote r;
        r.name = s.name;
        r.provider = s.values.value("provider");
        r.accessKeyId = s.values.value("access_key_id");
        r.secretAccessKey = s.values.value("secret_access_key");
        r.region = s.values.value("region");
        r.endpoint = s.values.value("endpoint");
        if (s.values.contains("force_path_style"))
            r.forcePathStyle = truth(s.values.value("force_path_style"));
        r.envAuth = truth(s.values.value("env_auth")).value_or(false);
        out.remotes.append(r);
    }
    return out;
}

QStringList RcloneConfigReader::candidatePaths()
{
    // rclone's own order: $RCLONE_CONFIG, then the per-user locations
    // ($APPDATA only exists on Windows), then the legacy file.
    QStringList out;
    const auto add = [&out](const QString& base, const QString& rest) {
        if (!base.isEmpty())
            out.append(base + rest);
    };
    add(qEnvironmentVariable("RCLONE_CONFIG"), QString());
    add(qEnvironmentVariable("XDG_CONFIG_HOME"), QStringLiteral("/rclone/rclone.conf"));
    add(QDir::homePath(), QStringLiteral("/.config/rclone/rclone.conf"));
    add(qEnvironmentVariable("APPDATA"), QStringLiteral("/rclone/rclone.conf"));
    add(QDir::homePath(), QStringLiteral("/.rclone.conf"));
    out.removeDuplicates();
    return out;
}

} // namespace qbucket
