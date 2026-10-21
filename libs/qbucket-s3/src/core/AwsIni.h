#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

namespace qbucket::s3::detail {

// A section of an AWS shared file. Values are kept as written (no INI
// escaping: QSettings would mangle secrets with '/', '+' or '\').
struct AwsIniSection {
    QString kind;                           // "" (credentials / [default]), "profile", "services", "sso-session", …
    QString name;
    QMap<QByteArray, QString> values;       // lower-case keys
    // Nested settings: "s3 =" followed by indented "key = value" lines.
    QMap<QByteArray, QMap<QByteArray, QString>> nested;
};

// The sections in file order. `configSpelling`: "[profile x]" is profile x,
// "[default]" the default profile, "[services x]" etc. their own kinds; a
// bare "[x]" in the config file is a profile only for "default". In the
// credentials file every "[x]" is profile x ("[profile x]" is accepted too).
QList<AwsIniSection> parseAwsIni(const QByteArray& content, bool configSpelling);

} // namespace qbucket::s3::detail
