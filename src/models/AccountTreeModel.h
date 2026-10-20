#pragma once

#include "domain/Account.h"

#include <qbucket/s3/Error.h>
#include <qbucket/s3/Session.h>

#include <QAbstractItemModel>
#include <QDateTime>
#include <QList>
#include <QUuid>

#include <memory>
#include <vector>

namespace qbucket {

class AccountSessions;
class AccountStore;

// The sidebar: accounts → buckets (PLAN.md §4). Buckets are listed lazily
// (fetchMore when an account is expanded) through the account's session;
// the account's external buckets are always shown. Loading, an error, or
// an empty bucket list each show as a status row — an error is never an
// empty list.
//
// Thread affinity: GUI thread.
class AccountTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum class Kind { Account, Bucket, Status };
    Q_ENUM(Kind)
    enum class Status { Loading, Error, Empty, ListingNotPermitted };
    Q_ENUM(Status)

    enum Role {
        KindRole = Qt::UserRole + 1,   // int(Kind)
        AccountIdRole,                 // QUuid, every row
        BucketRole,                    // QString, bucket rows
        EnvironmentRole,               // int(Environment), every row
        ReadOnlyRole,                  // bool, every row
        ExternalRole,                  // bool, bucket rows: added by hand, not listed
        StatusRole,                    // int(Status), status rows
    };

    AccountTreeModel(AccountStore* accounts, AccountSessions* sessions, QObject* parent = nullptr);
    ~AccountTreeModel() override;

    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override;
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] bool hasChildren(const QModelIndex& parent = {}) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Lists the buckets of the index's account again.
    void refresh(const QModelIndex& index);

    [[nodiscard]] QModelIndex accountIndex(const QUuid& account) const;
    [[nodiscard]] QModelIndex bucketIndex(const QUuid& account, const QString& bucket) const;

private:
    struct BucketRow {
        QString name;
        QDateTime created;
        bool external = false;
    };
    enum class State { NotLoaded, Loading, Loaded, Failed };
    struct AccountNode {
        Account account;
        State state = State::NotLoaded;
        Status status = Status::Empty;   // of the status row, if there is one
        QString errorText;               // translated, for the status row
        QList<BucketRow> listed;         // from ListBuckets
        QList<BucketRow> children;       // what the rows show: listed + external, sorted
        bool hasStatusRow = false;       // first child row
        quint64 generation = 0;          // answers of an older listing are dropped
        s3::RequestId pending = s3::RequestId::Invalid;
    };

    void onAccountsChanged_();
    void startListing_(int row);
    void onListed_(const QUuid& account, quint64 generation, const s3::Result<s3::BucketList>& result);
    void setChildren_(int row, State state, Status status, const QString& errorText);
    void rebuildChildren_(AccountNode& node) const;
    [[nodiscard]] int rowOf_(const AccountNode* node) const;
    [[nodiscard]] AccountNode* nodeFor_(const QUuid& account) const;

    AccountStore* m_accounts;
    AccountSessions* m_sessions;
    std::vector<std::unique_ptr<AccountNode>> m_nodes;
};

} // namespace qbucket
