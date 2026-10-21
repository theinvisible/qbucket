#include "models/BookmarkModel.h"

#include "accounts/AccountStore.h"
#include "accounts/LocationStore.h"

namespace qbucket {

BookmarkModel::BookmarkModel(LocationStore* store, AccountStore* accounts, QObject* parent)
    : QAbstractListModel(parent),
      m_store(store),
      m_accounts(accounts)
{
    m_rows = m_store->bookmarks();
    connect(m_store, &LocationStore::bookmarksChanged, this, &BookmarkModel::reload_);
    if (m_accounts) {
        connect(m_accounts, &AccountStore::accountsChanged, this, [this] {
            if (!m_rows.isEmpty())
                emit dataChanged(index(0), index(int(m_rows.size()) - 1), {Qt::ToolTipRole, PlaceRole});
        });
    }
}

int BookmarkModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant BookmarkModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Bookmark& b = m_rows.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return b.name;
    case Qt::ToolTipRole:
    case PlaceRole:
        return placeText(b.location, m_accounts);
    case IdRole:
        return b.id;
    default:
        return {};
    }
}

Bookmark BookmarkModel::bookmarkAt(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows.at(row) : Bookmark{};
}

int BookmarkModel::rowOf(const QUuid& id) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).id == id)
            return i;
    }
    return -1;
}

QString BookmarkModel::placeText(const Location& location, const AccountStore* accounts)
{
    const std::optional<Account> account = accounts ? accounts->account(location.account) : std::nullopt;
    const QString who = account ? account->name : tr("(removed account)");
    return QStringLiteral("%1 · %2/%3").arg(who, location.bucket, location.prefix);
}

void BookmarkModel::reload_()
{
    beginResetModel();
    m_rows = m_store->bookmarks();
    endResetModel();
}

} // namespace qbucket
