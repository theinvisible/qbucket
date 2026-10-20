#pragma once

#include <QString>
#include <QStringList>
#include <QUuid>

namespace qbucket {

// A place in the browser: account, bucket, prefix ("" or ending with "/").
// Without a bucket it stands for the account itself. Plain value.
struct Location {
    QUuid account;
    QString bucket;
    QString prefix;

    [[nodiscard]] bool isValid() const { return !account.isNull(); }
    [[nodiscard]] bool hasBucket() const { return isValid() && !bucket.isEmpty(); }

    // One folder up; the bucket root stays where it is.
    [[nodiscard]] Location parent() const
    {
        Location up = *this;
        if (up.prefix.isEmpty())
            return up;
        const qsizetype cut = up.prefix.lastIndexOf(u'/', up.prefix.size() - 2);
        up.prefix = cut < 0 ? QString() : up.prefix.left(cut + 1);
        return up;
    }

    // "a/b/" → {"a", "b"}. Empty segments ("a//b/") are kept: S3 keys may
    // contain them.
    [[nodiscard]] QStringList segments() const
    {
        if (prefix.isEmpty())
            return {};
        QStringList parts = prefix.split(u'/');
        parts.removeLast();   // after the trailing "/"
        return parts;
    }

    friend bool operator==(const Location& a, const Location& b)
    {
        return a.account == b.account && a.bucket == b.bucket && a.prefix == b.prefix;
    }
    friend bool operator!=(const Location& a, const Location& b) { return !(a == b); }
};

} // namespace qbucket
