#include "models/AccountTreeModel.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"

#include <QLocale>

#include <algorithm>

namespace qbucket {

namespace {

bool lessCaseInsensitive(const QString& a, const QString& b)
{
    const int c = QString::compare(a, b, Qt::CaseInsensitive);
    return c != 0 ? c < 0 : a < b;
}

} // namespace

AccountTreeModel::AccountTreeModel(AccountStore* accounts, AccountSessions* sessions, QObject* parent)
    : QAbstractItemModel(parent),
      m_accounts(accounts),
      m_sessions(sessions)
{
    for (const Account& a : m_accounts->accounts()) {
        auto node = std::make_unique<AccountNode>();
        node->account = a;
        m_nodes.push_back(std::move(node));
    }
    connect(m_accounts, &AccountStore::accountsChanged, this, &AccountTreeModel::onAccountsChanged_);
}

AccountTreeModel::~AccountTreeModel() = default;

// --- structure ---------------------------------------------------------------
// Account rows carry a null internal pointer; child rows point at their
// AccountNode, which stays put while rows come and go.

QModelIndex AccountTreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (column != 0 || row < 0)
        return {};
    if (!parent.isValid())
        return row < int(m_nodes.size()) ? createIndex(row, 0) : QModelIndex();
    if (parent.internalPointer() != nullptr || parent.row() >= int(m_nodes.size()))
        return {};
    AccountNode* node = m_nodes[size_t(parent.row())].get();
    return row < rowCount(parent) ? createIndex(row, 0, node) : QModelIndex();
}

QModelIndex AccountTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid() || child.internalPointer() == nullptr)
        return {};
    const int row = rowOf_(static_cast<const AccountNode*>(child.internalPointer()));
    return row < 0 ? QModelIndex() : createIndex(row, 0);
}

int AccountTreeModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return int(m_nodes.size());
    if (parent.column() != 0 || parent.internalPointer() != nullptr || parent.row() >= int(m_nodes.size()))
        return 0;
    const AccountNode& n = *m_nodes[size_t(parent.row())];
    return int(n.children.size()) + (n.hasStatusRow ? 1 : 0);
}

int AccountTreeModel::columnCount(const QModelIndex&) const
{
    return 1;
}

bool AccountTreeModel::hasChildren(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return !m_nodes.empty();
    // An account always has something to show once listed (buckets or a
    // status row), so the expander is there before the first listing.
    return parent.internalPointer() == nullptr;
}

bool AccountTreeModel::canFetchMore(const QModelIndex& parent) const
{
    if (!parent.isValid() || parent.internalPointer() != nullptr || parent.row() >= int(m_nodes.size()))
        return false;
    return m_nodes[size_t(parent.row())]->state == State::NotLoaded;
}

void AccountTreeModel::fetchMore(const QModelIndex& parent)
{
    if (!canFetchMore(parent))
        return;
    // Rows change only from the event loop: views (and the model tester)
    // call fetchMore in the middle of their own bookkeeping.
    AccountNode& n = *m_nodes[size_t(parent.row())];
    n.state = State::Loading;
    const QUuid id = n.account.id;
    const quint64 generation = n.generation;
    QMetaObject::invokeMethod(this, [this, id, generation] {
        AccountNode* node = nodeFor_(id);
        if (node && node->generation == generation && node->state == State::Loading
            && node->pending == s3::RequestId::Invalid)
            startListing_(rowOf_(node));
    }, Qt::QueuedConnection);
}

QHash<int, QByteArray> AccountTreeModel::roleNames() const
{
    QHash<int, QByteArray> names = QAbstractItemModel::roleNames();
    names.insert(KindRole, "kind");
    names.insert(AccountIdRole, "accountId");
    names.insert(BucketRole, "bucket");
    names.insert(EnvironmentRole, "environment");
    names.insert(ReadOnlyRole, "readOnly");
    names.insert(ExternalRole, "external");
    names.insert(StatusRole, "status");
    return names;
}

QVariant AccountTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};

    const auto* owner = static_cast<const AccountNode*>(index.internalPointer());
    const AccountNode& n = owner ? *owner : *m_nodes[size_t(index.row())];
    switch (role) {
    case AccountIdRole:
        return n.account.id;
    case EnvironmentRole:
        return int(n.account.environment);
    case ReadOnlyRole:
        return n.account.readOnly;
    default:
        break;
    }

    if (!owner) {   // account row
        switch (role) {
        case Qt::DisplayRole:
            return n.account.name;
        case Qt::ToolTipRole: {
            const QString where = n.account.endpoint.isEmpty() ? n.account.region : n.account.endpoint;
            QString env;
            switch (n.account.environment) {
            case Environment::Production:
                env = tr("Production");
                break;
            case Environment::Staging:
                env = tr("Staging");
                break;
            case Environment::Development:
                env = tr("Development");
                break;
            }
            QString tip = tr("%1 · %2\n%3").arg(n.account.providerId, where, env);
            if (n.account.readOnly)
                tip += tr(" · read-only");
            return tip;
        }
        case KindRole:
            return int(Kind::Account);
        default:
            return {};
        }
    }

    const int statusRows = n.hasStatusRow ? 1 : 0;
    if (index.row() < statusRows) {
        switch (role) {
        case Qt::DisplayRole:
            switch (n.status) {
            case Status::Loading:
                return tr("Loading…");
            case Status::Error:
                return n.errorText;
            case Status::Empty:
                return tr("No buckets");
            case Status::ListingNotPermitted:
                return tr("Listing buckets is not permitted");
            }
            return {};
        case Qt::ToolTipRole:
            if (n.status == Status::Error)
                return tr("%1\nPress F5 to try again.").arg(n.errorText);
            if (n.status == Status::ListingNotPermitted)
                return tr("Only the buckets added by hand are shown.");
            return {};
        case KindRole:
            return int(Kind::Status);
        case StatusRole:
            return int(n.status);
        default:
            return {};
        }
    }

    const int i = index.row() - statusRows;
    if (i >= int(n.children.size()))
        return {};
    const BucketRow& b = n.children[i];
    switch (role) {
    case Qt::DisplayRole:
    case BucketRole:
        return b.name;
    case Qt::ToolTipRole:
        if (b.external)
            return tr("Added by hand — not in the bucket list of this account");
        return b.created.isValid()
                   ? tr("Created %1").arg(QLocale().toString(b.created.toLocalTime(), QLocale::ShortFormat))
                   : QVariant();
    case KindRole:
        return int(Kind::Bucket);
    case ExternalRole:
        return b.external;
    default:
        return {};
    }
}

QModelIndex AccountTreeModel::accountIndex(const QUuid& account) const
{
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i]->account.id == account)
            return createIndex(int(i), 0);
    }
    return {};
}

QModelIndex AccountTreeModel::bucketIndex(const QUuid& account, const QString& bucket) const
{
    AccountNode* n = nodeFor_(account);
    if (!n)
        return {};
    for (qsizetype i = 0; i < n->children.size(); ++i) {
        if (n->children[i].name == bucket)
            return createIndex(int(i) + (n->hasStatusRow ? 1 : 0), 0, n);
    }
    return {};
}

void AccountTreeModel::refresh(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    const QModelIndex account = index.internalPointer() ? index.parent() : index;
    if (account.isValid())
        startListing_(account.row());
}

// --- listing -------------------------------------------------------------

void AccountTreeModel::startListing_(int row)
{
    AccountNode& n = *m_nodes[size_t(row)];
    ++n.generation;
    if (n.pending != s3::RequestId::Invalid && m_sessions->hasSession(n.account.id)) {
        if (const s3::Result<s3::Session*> s = m_sessions->session(n.account.id))
            (*s)->cancel(n.pending);
    }
    n.pending = s3::RequestId::Invalid;

    const s3::Result<s3::Session*> session = m_sessions->session(n.account.id);
    if (!session) {
        setChildren_(row, State::Failed, Status::Error, ConnectionTester::describe(session.error()));
        return;
    }
    setChildren_(row, State::Loading, Status::Loading, {});
    const QUuid id = n.account.id;
    const quint64 generation = n.generation;
    n.pending = (*session)->listBuckets(this, [this, id, generation](const s3::Result<s3::BucketList>& r) {
        onListed_(id, generation, r);
    });
}

void AccountTreeModel::onListed_(const QUuid& account, quint64 generation,
                                 const s3::Result<s3::BucketList>& result)
{
    AccountNode* n = nodeFor_(account);
    if (!n || n->generation != generation)
        return;
    n->pending = s3::RequestId::Invalid;
    const int row = rowOf_(n);

    if (result) {
        n->listed.clear();
        for (const s3::BucketInfo& b : result->buckets)
            n->listed.append({b.name, b.creationDate, false});
        setChildren_(row, State::Loaded, Status::Empty, {});
        return;
    }

    const s3::Error& e = result.error();
    if (e.category == s3::ErrorCategory::Cancelled) {
        // The session was replaced (settings changed, reconnect): list
        // through the new one. At shutdown there is none.
        if (!m_sessions->isShutDown())
            startListing_(row);
        return;
    }
    n->listed.clear();
    if (e.category == s3::ErrorCategory::NotPermitted && !n->account.externalBuckets.isEmpty())
        setChildren_(row, State::Failed, Status::ListingNotPermitted, {});
    else
        setChildren_(row, State::Failed, Status::Error, ConnectionTester::describe(e));
}

void AccountTreeModel::rebuildChildren_(AccountNode& n) const
{
    QList<BucketRow> rows;
    if (n.state == State::Loaded)
        rows = n.listed;
    for (const QString& name : n.account.externalBuckets) {
        const bool listed = std::any_of(rows.cbegin(), rows.cend(),
                                        [&](const BucketRow& r) { return r.name == name; });
        if (!listed && !name.isEmpty())
            rows.append({name, {}, true});
    }
    std::sort(rows.begin(), rows.end(),
              [](const BucketRow& a, const BucketRow& b) { return lessCaseInsensitive(a.name, b.name); });
    n.children = rows;
    n.hasStatusRow = n.state == State::Loading || n.state == State::Failed
                  || (n.state == State::Loaded && rows.isEmpty());
}

void AccountTreeModel::setChildren_(int row, State state, Status status, const QString& errorText)
{
    AccountNode& n = *m_nodes[size_t(row)];
    const QModelIndex parent = createIndex(row, 0);
    if (const int old = rowCount(parent); old > 0) {
        beginRemoveRows(parent, 0, old - 1);
        n.children.clear();
        n.hasStatusRow = false;
        endRemoveRows();
    }

    AccountNode next;
    next.account = n.account;
    next.state = state;
    next.listed = n.listed;
    rebuildChildren_(next);

    n.state = state;
    n.status = status;
    n.errorText = errorText;
    if (state == State::NotLoaded)
        return;   // rows appear with the next fetchMore
    const int count = int(next.children.size()) + (next.hasStatusRow ? 1 : 0);
    if (count == 0)
        return;
    beginInsertRows(parent, 0, count - 1);
    n.children = next.children;
    n.hasStatusRow = next.hasStatusRow;
    endInsertRows();
}

// --- account changes ---------------------------------------------------------

void AccountTreeModel::onAccountsChanged_()
{
    const QList<Account> now = m_accounts->accounts();
    bool sameLayout = qsizetype(m_nodes.size()) == now.size();
    for (qsizetype i = 0; sameLayout && i < now.size(); ++i)
        sameLayout = m_nodes[size_t(i)]->account.id == now[i].id;

    if (sameLayout) {
        for (qsizetype i = 0; i < now.size(); ++i) {
            AccountNode& n = *m_nodes[size_t(i)];
            const Account before = n.account;
            n.account = now[i];
            const int row = int(i);
            if (!sameConnection(before, now[i])) {
                n.listed.clear();
                if (n.state != State::NotLoaded)
                    startListing_(row);
            } else if (before.externalBuckets != now[i].externalBuckets && n.state != State::NotLoaded) {
                setChildren_(row, n.state, n.status, n.errorText);
            }
            const QModelIndex idx = createIndex(row, 0);
            emit dataChanged(idx, idx);
        }
        return;
    }

    // Accounts added, removed or re-sorted: rebuild, keeping what was
    // already listed for accounts whose connection is unchanged.
    beginResetModel();
    std::vector<std::unique_ptr<AccountNode>> old = std::move(m_nodes);
    m_nodes.clear();
    for (const Account& a : now) {
        auto it = std::find_if(old.begin(), old.end(), [&](const std::unique_ptr<AccountNode>& n) {
            return n && n->account.id == a.id && sameConnection(n->account, a);
        });
        std::unique_ptr<AccountNode> node;
        if (it != old.end()) {
            node = std::move(*it);
            node->account = a;
            if (node->state == State::Loading) {
                // Its answer still arrives; drop it and list again on expand.
                ++node->generation;
                node->state = State::NotLoaded;
            }
            if (node->state != State::NotLoaded)
                rebuildChildren_(*node);
            else {
                node->children.clear();
                node->hasStatusRow = false;
            }
        } else {
            node = std::make_unique<AccountNode>();
            node->account = a;
        }
        m_nodes.push_back(std::move(node));
    }
    endResetModel();
}

// --- helpers -----------------------------------------------------------------

int AccountTreeModel::rowOf_(const AccountNode* node) const
{
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].get() == node)
            return int(i);
    }
    return -1;
}

AccountTreeModel::AccountNode* AccountTreeModel::nodeFor_(const QUuid& account) const
{
    for (const auto& n : m_nodes) {
        if (n->account.id == account)
            return n.get();
    }
    return nullptr;
}

} // namespace qbucket
