#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace qbucket {

// One S3 remote of an rclone configuration (rclone.conf), as a source to set
// an account up from. Holds the secret key: callers hand it to the
// SecretStore and never log it.
struct RcloneRemote {
    QString name;
    QString provider;                     // rclone's `provider` (AWS, Ceph, Minio, Other, …)
    QString accessKeyId;
    QString secretAccessKey;
    QString region;
    QString endpoint;                     // as written (may lack the scheme)
    std::optional<bool> forcePathStyle;   // unset: rclone's default (path style)
    bool envAuth = false;                 // credentials from the environment / instance role
};

// Reads rclone's INI-style configuration. Only `type = s3` remotes are
// returned; values are taken as written (rclone does not obscure S3 keys).
// Pure parsing. Thread-safe (static).
class RcloneConfigReader {
public:
    struct Result {
        QList<RcloneRemote> remotes;
        bool encrypted = false;           // the whole file is encrypted (RCLONE_ENCRYPT_V0)
    };

    [[nodiscard]] static Result parse(const QByteArray& content);

    // Where rclone looks, in its order: $RCLONE_CONFIG, the per-user
    // locations (~/.config/rclone/rclone.conf, %APPDATA%\rclone\rclone.conf),
    // the legacy ~/.rclone.conf. The first readable one is the default.
    [[nodiscard]] static QStringList candidatePaths();
};

} // namespace qbucket
