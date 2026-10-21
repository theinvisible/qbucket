#pragma once

#include "domain/Bookmark.h"

#include <QAbstractListModel>
#include <QList>

namespace qbucket {

class AccountStore;
class LocationStore;

// The bookmarks of a LocationStore, in its order: the name, the place as a
// tooltip ("Account · bucket/prefix"). Follows the store and account
// renames.
//
// Thread affinity: GUI thread.
class BookmarkModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,   // QUuid
        PlaceRole,                   // QString: "Account · bucket/prefix"
    };

    BookmarkModel(LocationStore* store, AccountStore* accounts, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    [[nodiscard]] Bookmark bookmarkAt(int row) const;
    [[nodiscard]] int rowOf(const QUuid& id) const;

    // "Account · bucket/prefix"; the account's name, or "(removed account)".
    [[nodiscard]] static QString placeText(const Location& location, const AccountStore* accounts);

private:
    void reload_();

    LocationStore* m_store;
    AccountStore* m_accounts;
    QList<Bookmark> m_rows;
};

} // namespace qbucket
