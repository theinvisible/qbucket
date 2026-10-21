#pragma once

#include <QList>
#include <QString>
#include <QUuid>

#include <optional>

class QMimeData;

namespace qbucket {

// Objects and folders picked for a copy, a cut or a drag inside QBucket —
// carried as QMimeData: application/x-qbucket-objects (JSON) and text/plain
// (their s3:// URIs, so pasting into an editor still works). Only keys: the
// job that uses them lists its scope afresh.
struct ObjectSelection {
    enum class Intent { Copy, Cut };
    struct Item {
        QString key;           // an object key, or a folder prefix ending with "/"
        bool folder = false;
    };
    static constexpr char kMimeType[] = "application/x-qbucket-objects";

    QUuid account;
    QString bucket;
    QString base;              // the folder they were picked in ("" for the root)
    Intent intent = Intent::Copy;
    QList<Item> items;

    [[nodiscard]] bool isEmpty() const { return items.isEmpty(); }
    // A new QMimeData (the caller owns it, or hands it to a clipboard / drag).
    [[nodiscard]] QMimeData* toMimeData() const;
    // Only what this process put there (another QBucket's accounts are not
    // ours); nullopt for anything else.
    [[nodiscard]] static std::optional<ObjectSelection> fromMimeData(const QMimeData* mime);
};

} // namespace qbucket
