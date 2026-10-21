#include "models/ObjectTableModel.h"

#include "models/ObjectSelection.h"

#include <QMimeData>
#include <QSet>

#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QPointer>
#include <QThreadPool>
#include <QTimeZone>

#include <algorithm>

namespace qbucket {

namespace {

// What a sort needs of one row; built on the GUI thread, sorted anywhere.
// Kept cheap to build (a million rows): the key is an implicitly shared
// copy — no allocation — and the name is cut from it by the sorter.
struct SortKey {
    quint32 row = 0;
    bool folder = false;
    bool groupVersions = false;   // Versions mode: a key's versions stay in server order
    quint16 storageClass = 0;     // index into the classes handed to the sorter
    qint64 size = 0;
    qint64 mtime = 0;
    QString key;
};

// The name shown: the key after the location's prefix, a folder without its "/".
QStringView nameView(const SortKey& k, qsizetype prefixLen)
{
    QStringView name = QStringView(k.key).mid(prefixLen);
    if (k.folder && name.endsWith(u'/'))
        name.chop(1);
    return name;
}

int compare3(qint64 a, qint64 b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

// "file9" before "file10": digit runs compare by value, the text between
// them through the collator. Done here rather than with
// QCollator::setNumericMode, which Qt builds without ICU ignore.
int naturalCompare(const QCollator& collator, QStringView a, QStringView b)
{
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = a[i].isDigit();
        const bool db = b[j].isDigit();
        qsizetype ei = i;
        qsizetype ej = j;
        while (ei < a.size() && a[ei].isDigit() == da)
            ++ei;
        while (ej < b.size() && b[ej].isDigit() == db)
            ++ej;
        const QStringView ra = a.sliced(i, ei - i);
        const QStringView rb = b.sliced(j, ej - j);
        int c = 0;
        if (da && db) {
            // By value: without leading zeros, the longer run is larger.
            qsizetype za = 0;
            qsizetype zb = 0;
            while (za + 1 < ra.size() && ra[za] == u'0')
                ++za;
            while (zb + 1 < rb.size() && rb[zb] == u'0')
                ++zb;
            const QStringView va = ra.sliced(za);
            const QStringView vb = rb.sliced(zb);
            c = va.size() != vb.size() ? (va.size() < vb.size() ? -1 : 1) : va.compare(vb);
        } else {
            c = collator.compare(ra, rb);
        }
        if (c != 0)
            return c;
        i = ei;
        j = ej;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    return 0;
}

// Folders first in both directions; ties keep server order.
std::vector<quint32> sortedOrder(std::vector<SortKey> keys, int column, Qt::SortOrder order, const QCollator& collator,
                                 qsizetype prefixLen, const QStringList& classes)
{
    const bool descending = order == Qt::DescendingOrder;
    std::sort(keys.begin(), keys.end(), [&](const SortKey& a, const SortKey& b) {
        if (a.folder != b.folder)
            return a.folder;
        int c = 0;
        switch (column) {
        case ObjectTableModel::NameColumn:
            c = naturalCompare(collator, nameView(a, prefixLen), nameView(b, prefixLen));
            break;
        case ObjectTableModel::SizeColumn:
            c = compare3(a.size, b.size);
            break;
        case ObjectTableModel::ModifiedColumn:
            c = compare3(a.mtime, b.mtime);
            break;
        case ObjectTableModel::StorageClassColumn:
            c = a.storageClass == b.storageClass
                    ? 0
                    : QString::compare(classes.at(a.storageClass), classes.at(b.storageClass), Qt::CaseInsensitive);
            break;
        default:
            break;
        }
        // By name, a key's versions stay together, newest first, either way.
        if (c == 0 && column == ObjectTableModel::NameColumn && a.groupVersions)
            return a.row < b.row;
        if (c == 0)
            c = compare3(a.row, b.row);
        return descending ? c > 0 : c < 0;
    });
    std::vector<quint32> out;
    out.reserve(keys.size());
    for (const SortKey& k : keys)
        out.push_back(k.row);
    return out;
}

QCollator naturalCollator()
{
    QCollator c;
    c.setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

} // namespace

ObjectTableModel::ObjectTableModel(AccountSessions* sessions, QObject* parent)
    : QAbstractTableModel(parent),
      m_sessions(sessions),
      m_collator(naturalCollator())
{
    m_sortDebounce.setSingleShot(true);
    m_sortDebounce.setInterval(150);
    connect(&m_sortDebounce, &QTimer::timeout, this, &ObjectTableModel::runSort_);
    // The pager refers to the session; when that goes, list through the
    // next one.
    connect(m_sessions, &AccountSessions::sessionStopped, this, [this](const QUuid& id) {
        if (id != m_account || !hasPager_())
            return;
        m_run.reset();
        m_pager.reset();
        m_versionPager.reset();
        if (!m_sessions->isShutDown())
            reload();
    });
}

ObjectTableModel::~ObjectTableModel() = default;

// --- location ---------------------------------------------------------------

void ObjectTableModel::setLocation(const QUuid& account, const QString& bucket, const QString& prefix, Mode mode)
{
    keepForCache_();
    m_account = account;
    m_bucket = bucket;
    m_prefix = prefix;
    m_mode = mode;
    m_search.clear();
    start_(true);
}

void ObjectTableModel::clear()
{
    keepForCache_();
    m_account = QUuid();
    m_bucket.clear();
    m_prefix.clear();
    m_search.clear();
    start_(false);
}

void ObjectTableModel::reload()
{
    start_(false);
}

void ObjectTableModel::setSearchPrefix(const QString& text)
{
    if (text == m_search)
        return;
    keepForCache_();
    m_search = text;
    start_(text.isEmpty());
}

void ObjectTableModel::setFilterText(const QString& text)
{
    if (text == m_filter)
        return;
    m_filter = text;
    // The row count changes arbitrarily: a reset is the honest signal, and
    // with uniform row heights a cheap one.
    beginResetModel();
    rebuildView_();
    endResetModel();
}

void ObjectTableModel::resetListing_()
{
    m_run.reset();   // answers of the previous listing are dropped
    m_pager.reset();
    m_versionPager.reset();
    m_sortDebounce.stop();
    m_fresh.clear();
    m_freshEnds.clear();
    m_freshPages = 0;
    setStale_(false);
    beginResetModel();
    m_rows.clear();
    m_pageEnds.clear();
    m_order.clear();
    m_view.clear();
    m_folderRows = m_orderFolders = m_viewFolders = 0;
    m_bytes = 0;
    m_storageClasses = QStringList{QString()};
    m_versionIds.assign(1, QString());
    endResetModel();
    ++m_orderVersion;
    m_pages = 0;
    m_error = s3::Error();
    m_errorText.clear();
}

void ObjectTableModel::start_(bool useCache)
{
    resetListing_();
    if (m_account.isNull() || m_bucket.isEmpty()) {
        setState_(State::Idle);
        return;
    }
    const s3::Result<s3::Session*> session = m_sessions->session(m_account);
    if (!session) {
        m_error = session.error();
        m_errorText = ConnectionTester::describe(m_error);
        setState_(State::Error);
        return;
    }
    m_run = std::make_unique<QObject>();
    if (m_mode == Mode::Versions) {
        s3::ListObjectVersionsParams params;
        params.bucket = m_bucket;
        params.prefix = m_prefix + m_search;
        params.delimiter = QStringLiteral("/");
        params.maxKeys = kPageSize;
        m_versionPager = std::make_unique<s3::ListObjectVersionsPager>(**session, params);
        setState_(State::Loading);   // never from the cache
        fetchPage_();
        return;
    }
    s3::ListObjectsParams params;
    params.bucket = m_bucket;
    params.prefix = m_prefix + m_search;
    params.delimiter = m_mode == Mode::Folders ? QStringLiteral("/") : QString();
    params.maxKeys = kPageSize;
    m_pager = std::make_unique<s3::ListObjectsPager>(**session, params);
    if (useCache && m_search.isEmpty() && restoreSnapshot_()) {
        fetchPage_();   // the refresh
        return;
    }
    setState_(State::Loading);
    fetchPage_();
}

// --- listing cache -----------------------------------------------------------

void ObjectTableModel::keepForCache_()
{
    if (m_account.isNull() || m_bucket.isEmpty() || !m_search.isEmpty() || m_pageEnds.empty() || m_mode == Mode::Versions
        || (m_state != State::Ready && m_state != State::Complete))
        return;
    auto snap = std::make_unique<Snapshot>();
    snap->account = m_account;
    snap->bucket = m_bucket;
    snap->prefix = m_prefix;
    snap->mode = m_mode;
    const size_t pages = std::min(m_pageEnds.size(), size_t(kCachePages));
    snap->pageEnds.assign(m_pageEnds.begin(), m_pageEnds.begin() + qsizetype(pages));
    snap->rows.assign(m_rows.begin(), m_rows.begin() + qsizetype(snap->pageEnds.back()));
    snap->storageClasses = m_storageClasses;
    snap->complete = m_state == State::Complete && pages == m_pageEnds.size();
    snap->at = m_stale ? m_staleSince : QDateTime::currentDateTimeUtc();

    const auto same = [&](const std::unique_ptr<Snapshot>& e) {
        return e->account == snap->account && e->bucket == snap->bucket && e->prefix == snap->prefix
            && e->mode == snap->mode;
    };
    m_cache.erase(std::remove_if(m_cache.begin(), m_cache.end(), same), m_cache.end());
    m_cache.insert(m_cache.begin(), std::move(snap));
    qsizetype rows = 0;
    size_t keep = 0;
    for (; keep < m_cache.size() && keep < size_t(kCacheEntries); ++keep) {
        rows += qsizetype(m_cache[keep]->rows.size());
        if (rows > kCacheRows && keep > 0)
            break;
    }
    m_cache.resize(keep);
}

bool ObjectTableModel::restoreSnapshot_()
{
    const auto it = std::find_if(m_cache.begin(), m_cache.end(), [&](const std::unique_ptr<Snapshot>& e) {
        return e->account == m_account && e->bucket == m_bucket && e->prefix == m_prefix && e->mode == m_mode;
    });
    if (it == m_cache.end())
        return false;
    std::unique_ptr<Snapshot> snap = std::move(*it);
    m_cache.erase(it);

    m_storageClasses = snap->storageClasses;
    replaceRows_(std::move(snap->rows), std::move(snap->pageEnds));
    m_staleSince = snap->at;
    m_stalePages = m_pages;
    setStale_(true);
    setState_(snap->complete ? State::Complete : State::Ready);
    emit pageLoaded();
    return true;
}

void ObjectTableModel::swapInFresh_()
{
    replaceRows_(std::move(m_fresh), std::move(m_freshEnds));
    m_fresh.clear();
    m_freshEnds.clear();
    m_freshPages = 0;
    setStale_(false);
    setState_(pagerHasMore_() ? State::Ready : State::Complete);
    emit pageLoaded();
}

void ObjectTableModel::replaceRows_(std::vector<Row>&& rows, std::vector<size_t>&& pageEnds)
{
    beginResetModel();
    m_rows = std::move(rows);
    m_pageEnds = std::move(pageEnds);
    m_pages = int(m_pageEnds.size());
    m_folderRows = 0;
    m_bytes = 0;
    m_order.clear();
    m_order.reserve(m_rows.size());
    for (quint32 i = 0; i < quint32(m_rows.size()); ++i) {
        if (m_rows[i].flags & Folder) {
            ++m_folderRows;
            m_order.push_back(i);
        } else {
            m_bytes += m_rows[i].size;
        }
    }
    for (quint32 i = 0; i < quint32(m_rows.size()); ++i) {
        if (!(m_rows[i].flags & Folder))
            m_order.push_back(i);
    }
    rebuildView_();
    ++m_orderVersion;
    endResetModel();
    if (m_sortColumn >= 0)
        sort(m_sortColumn, m_sortOrder);
}

void ObjectTableModel::setStale_(bool stale)
{
    if (stale == m_stale)
        return;
    m_stale = stale;
    if (!stale)
        m_staleSince = QDateTime();
    emit staleChanged(stale);
}

bool ObjectTableModel::pagerHasMore_() const
{
    return m_versionPager ? m_versionPager->hasMore() : (m_pager && m_pager->hasMore());
}

bool ObjectTableModel::pagerFetching_() const
{
    return m_versionPager ? m_versionPager->isFetching() : (m_pager && m_pager->isFetching());
}

void ObjectTableModel::fetchPage_()
{
    if (!hasPager_() || pagerFetching_() || !pagerHasMore_())
        return;
    if (m_versionPager) {
        m_versionPager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListObjectVersionsPage>& page) {
            onVersionPage_(page);
        });
        return;
    }
    m_pager->fetchNext(m_run.get(), [this](const s3::Result<s3::ListPage>& page) { onPage_(page); });
}

void ObjectTableModel::listingFailed_(const s3::Error& error)
{
    if (error.category == s3::ErrorCategory::Cancelled)
        return;   // a replaced session; sessionStopped lists again
    m_error = error;
    m_errorText = ConnectionTester::describe(m_error);
    m_fresh.clear();
    m_freshEnds.clear();
    setState_(State::Error);   // a stale listing stays, still marked stale
}

void ObjectTableModel::onVersionPage_(const s3::Result<s3::ListObjectVersionsPage>& page)
{
    if (!page) {
        listingFailed_(page.error());
        return;
    }
    std::vector<Row> folders;
    std::vector<Row> objects;
    parseVersionPage_(*page, folders, objects);
    pageArrived_(std::move(folders), std::move(objects));
}

void ObjectTableModel::onPage_(const s3::Result<s3::ListPage>& page)
{
    if (!page) {
        listingFailed_(page.error());
        return;
    }
    std::vector<Row> folders;
    std::vector<Row> objects;
    parsePage_(*page, folders, objects);
    pageArrived_(std::move(folders), std::move(objects));
}

void ObjectTableModel::pageArrived_(std::vector<Row>&& folders, std::vector<Row>&& objects)
{
    if (m_stale) {
        // Refreshing a cached listing: collect the same pages, then swap.
        for (Row& r : folders)
            m_fresh.push_back(std::move(r));
        for (Row& r : objects)
            m_fresh.push_back(std::move(r));
        m_freshEnds.push_back(m_fresh.size());
        ++m_freshPages;
        if (m_freshPages >= m_stalePages || !pagerHasMore_())
            swapInFresh_();
        else
            fetchPage_();
        return;
    }

    ++m_pages;
    const bool empty = folders.empty() && objects.empty();
    appendRows_(std::move(folders), std::move(objects));

    setState_(pagerHasMore_() ? State::Ready : State::Complete);
    emit pageLoaded();
    // A page without rows brings no rows to scroll to — the view would not
    // ask again.
    if (empty && pagerHasMore_())
        fetchPage_();
}

void ObjectTableModel::parsePage_(const s3::ListPage& page, std::vector<Row>& folders, std::vector<Row>& objects)
{
    const qsizetype prefixLen = m_prefix.size();
    folders.reserve(size_t(page.commonPrefixes.size()));
    for (const QString& p : page.commonPrefixes) {
        if (p.size() <= prefixLen)
            continue;
        Row r;
        r.key = p;
        r.flags = Folder;
        folders.push_back(std::move(r));
    }
    objects.reserve(size_t(page.objects.size()));
    for (const s3::ObjectEntry& e : page.objects) {
        if (e.key == m_prefix)
            continue;   // the folder's own marker object
        Row r;
        r.key = e.key;
        r.size = e.size;
        r.mtimeMs = e.lastModified.isValid() ? e.lastModified.toMSecsSinceEpoch() : 0;
        r.storageClass = internStorageClass_(e.storageClass);
        objects.push_back(std::move(r));
    }
}

void ObjectTableModel::parseVersionPage_(const s3::ListObjectVersionsPage& page, std::vector<Row>& folders,
                                         std::vector<Row>& objects)
{
    const qsizetype prefixLen = m_prefix.size();
    folders.reserve(size_t(page.commonPrefixes.size()));
    for (const QString& p : page.commonPrefixes) {
        if (p.size() <= prefixLen)
            continue;
        Row r;
        r.key = p;
        r.flags = Folder;
        folders.push_back(std::move(r));
    }
    objects.reserve(size_t(page.entries.size()));
    for (const s3::VersionEntry& e : page.entries) {
        if (e.key == m_prefix)
            continue;   // the folder's own marker object
        Row r;
        r.key = e.key;
        r.size = e.size;
        r.mtimeMs = e.lastModified.isValid() ? e.lastModified.toMSecsSinceEpoch() : 0;
        r.storageClass = internStorageClass_(e.storageClass);
        r.flags = quint8((e.isLatest ? 0 : Older) | (e.isDeleteMarker ? DeleteMarker : 0));
        r.version = quint32(m_versionIds.size());
        m_versionIds.push_back(e.versionId);
        objects.push_back(std::move(r));
    }
}

void ObjectTableModel::appendRows_(std::vector<Row>&& folders, std::vector<Row>&& objects)
{
    const auto nf = qsizetype(folders.size());
    std::vector<quint32> newFolders;
    std::vector<quint32> newObjects;
    for (Row& r : folders) {
        newFolders.push_back(quint32(m_rows.size()));
        m_rows.push_back(std::move(r));
    }
    for (Row& r : objects) {
        newObjects.push_back(quint32(m_rows.size()));
        m_bytes += r.size;
        m_rows.push_back(std::move(r));
    }
    m_folderRows += nf;
    m_pageEnds.push_back(m_rows.size());

    std::vector<quint32> viewFolders;
    std::vector<quint32> viewObjects;
    for (quint32 i : newFolders) {
        if (passesFilter_(m_rows[i]))
            viewFolders.push_back(i);
    }
    for (quint32 i : newObjects) {
        if (passesFilter_(m_rows[i]))
            viewObjects.push_back(i);
    }

    ++m_orderVersion;
    if (m_sortColumn < 0) {
        // Server order: new folders after the folders, objects at the end.
        m_order.insert(m_order.begin() + m_orderFolders, newFolders.begin(), newFolders.end());
        m_orderFolders += qsizetype(newFolders.size());
        m_order.insert(m_order.end(), newObjects.begin(), newObjects.end());
        if (!viewFolders.empty()) {
            const int at = int(m_viewFolders);
            beginInsertRows({}, at, at + int(viewFolders.size()) - 1);
            m_view.insert(m_view.begin() + m_viewFolders, viewFolders.begin(), viewFolders.end());
            m_viewFolders += qsizetype(viewFolders.size());
            endInsertRows();
        }
        if (!viewObjects.empty()) {
            const int at = int(m_view.size());
            beginInsertRows({}, at, at + int(viewObjects.size()) - 1);
            m_view.insert(m_view.end(), viewObjects.begin(), viewObjects.end());
            endInsertRows();
        }
        return;
    }

    // Sorted: append in one go, then sort the page in (in the background).
    m_order.insert(m_order.end(), newFolders.begin(), newFolders.end());
    m_order.insert(m_order.end(), newObjects.begin(), newObjects.end());
    std::vector<quint32> visible = std::move(viewFolders);
    visible.insert(visible.end(), viewObjects.begin(), viewObjects.end());
    if (!visible.empty()) {
        const int at = int(m_view.size());
        beginInsertRows({}, at, at + int(visible.size()) - 1);
        m_view.insert(m_view.end(), visible.begin(), visible.end());
        endInsertRows();
    }
    scheduleSort_();
}

void ObjectTableModel::setState_(State state)
{
    if (state == m_state)
        return;
    m_state = state;
    emit stateChanged(state);
}

// --- sorting & filtering ---------------------------------------------------

void ObjectTableModel::sort(int column, Qt::SortOrder order)
{
    if (column >= ColumnCount)
        return;
    m_sortColumn = column;
    m_sortOrder = order;
    ++m_orderVersion;
    m_sortDebounce.stop();

    if (column < 0) {
        // Server order: the folders, then the objects, each as listed.
        std::vector<quint32> serverOrder;
        serverOrder.reserve(m_rows.size());
        for (quint32 i = 0; i < quint32(m_rows.size()); ++i) {
            if (m_rows[i].flags & Folder)
                serverOrder.push_back(i);
        }
        for (quint32 i = 0; i < quint32(m_rows.size()); ++i) {
            if (!(m_rows[i].flags & Folder))
                serverOrder.push_back(i);
        }
        applyOrder_(std::move(serverOrder), m_orderVersion);
        return;
    }
    if (m_rows.size() < size_t(kBackgroundSortRows)) {
        std::vector<SortKey> keys;
        keys.reserve(m_rows.size());
        for (quint32 i : m_order) {
            const Row& r = m_rows[i];
            keys.push_back({i, bool(r.flags & Folder), m_mode == Mode::Versions, r.storageClass, r.size, r.mtimeMs, r.key});
        }
        applyOrder_(sortedOrder(std::move(keys), column, order, m_collator, m_prefix.size(), m_storageClasses),
                    m_orderVersion);
        return;
    }
    runSort_();
}

void ObjectTableModel::scheduleSort_()
{
    if (m_sortColumn >= 0)
        m_sortDebounce.start();
}

void ObjectTableModel::runSort_()
{
    if (m_sortColumn < 0)
        return;
    if (m_sortRunning) {
        m_sortPending = true;
        return;
    }
    std::vector<SortKey> keys;
    keys.reserve(m_order.size());
    for (quint32 i : m_order) {
        const Row& r = m_rows[i];
        keys.push_back({i, bool(r.flags & Folder), m_mode == Mode::Versions, r.storageClass, r.size, r.mtimeMs, r.key});
    }
    m_sortRunning = true;
    const int column = m_sortColumn;
    const Qt::SortOrder order = m_sortOrder;
    const quint64 version = m_orderVersion;
    const qsizetype prefixLen = m_prefix.size();
    const QStringList classes = m_storageClasses;
    const QPointer<ObjectTableModel> self(this);
    QThreadPool::globalInstance()->start([keys = std::move(keys), column, order, version, prefixLen, classes, self]() mutable {
        std::vector<quint32> sorted = sortedOrder(std::move(keys), column, order, naturalCollator(), prefixLen, classes);
        // Back through the application object, which outlives the model;
        // `self` is only looked at on the GUI thread.
        QMetaObject::invokeMethod(QCoreApplication::instance(),
                                  [self, sorted = std::move(sorted), version]() mutable {
            if (!self)
                return;
            self->m_sortRunning = false;
            self->applyOrder_(std::move(sorted), version);
            if (self->m_sortPending) {
                self->m_sortPending = false;
                self->scheduleSort_();
            }
        }, Qt::QueuedConnection);
    });
}

void ObjectTableModel::applyOrder_(std::vector<quint32> order, quint64 version)
{
    if (version != m_orderVersion || order.size() != m_order.size()) {
        scheduleSort_();   // rows came in meanwhile: sort them in too
        return;
    }
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    const QModelIndexList before = persistentIndexList();
    std::vector<quint32> beforeRows;
    beforeRows.reserve(size_t(before.size()));
    for (const QModelIndex& i : before)
        beforeRows.push_back(m_view[size_t(i.row())]);

    m_order = std::move(order);
    rebuildView_();

    std::vector<int> position(m_rows.size(), -1);
    for (size_t i = 0; i < m_view.size(); ++i)
        position[m_view[i]] = int(i);
    QModelIndexList after;
    after.reserve(before.size());
    for (qsizetype k = 0; k < before.size(); ++k) {
        const int row = position[beforeRows[size_t(k)]];
        after.append(row < 0 ? QModelIndex() : index(row, before[k].column()));
    }
    changePersistentIndexList(before, after);
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
    emit sortFinished();
}

void ObjectTableModel::rebuildView_()
{
    m_view.clear();
    m_view.reserve(m_order.size());
    m_orderFolders = m_viewFolders = 0;
    bool leading = true;
    for (quint32 i : m_order) {
        const Row& r = m_rows[i];
        const bool folder = r.flags & Folder;
        if (leading && !folder)
            leading = false;
        if (leading)
            ++m_orderFolders;
        if (!passesFilter_(r))
            continue;
        if (leading)
            ++m_viewFolders;
        m_view.push_back(i);
    }
}

bool ObjectTableModel::passesFilter_(const Row& row) const
{
    if (m_filter.isEmpty())
        return true;
    return QStringView(row.key).mid(m_prefix.size()).contains(m_filter, Qt::CaseInsensitive);
}

QString ObjectTableModel::nameOf_(const Row& row) const
{
    QString name = row.key.mid(m_prefix.size());
    if ((row.flags & Folder) && name.endsWith(u'/'))
        name.chop(1);
    return name;
}

quint16 ObjectTableModel::internStorageClass_(const QString& name)
{
    if (name.isEmpty())
        return 0;
    const qsizetype i = m_storageClasses.indexOf(name);
    if (i >= 0)
        return quint16(i);
    if (m_storageClasses.size() >= 0xffff)
        return 0;
    m_storageClasses.append(name);
    return quint16(m_storageClasses.size() - 1);
}

int ObjectTableModel::rowOfKey(const QString& key) const
{
    for (size_t i = 0; i < m_view.size(); ++i) {
        if (m_rows[m_view[i]].key == key)
            return int(i);
    }
    return -1;
}

int ObjectTableModel::rowOfVersion(const QString& key, const QString& versionId) const
{
    for (size_t i = 0; i < m_view.size(); ++i) {
        const Row& r = m_rows[m_view[i]];
        if (r.key == key && m_versionIds[r.version] == versionId)
            return int(i);
    }
    return -1;
}

// --- QAbstractItemModel ------------------------------------------------------

int ObjectTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_view.size());
}

int ObjectTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(ColumnCount);
}

bool ObjectTableModel::canLoadMore() const
{
    return hasPager_() && m_state == State::Ready && !m_stale && pagerHasMore_() && !pagerFetching_();
}

void ObjectTableModel::loadMore()
{
    if (canLoadMore())
        fetchPage_();
}

Qt::ItemFlags ObjectTableModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && m_mode != Mode::Versions)   // versions are not moved around
        f |= Qt::ItemIsDragEnabled;
    return f;
}

QStringList ObjectTableModel::mimeTypes() const
{
    return {QString::fromLatin1(ObjectSelection::kMimeType)};
}

QMimeData* ObjectTableModel::mimeData(const QModelIndexList& indexes) const
{
    ObjectSelection sel;
    sel.account = m_account;
    sel.bucket = m_bucket;
    sel.base = m_prefix;
    QSet<int> rows;
    QList<int> ordered;
    for (const QModelIndex& i : indexes) {
        if (i.isValid() && !rows.contains(i.row())) {
            rows.insert(i.row());
            ordered.append(i.row());
        }
    }
    std::sort(ordered.begin(), ordered.end());
    for (const int row : ordered) {
        const QModelIndex i = index(row, 0);
        sel.items.append({i.data(KeyRole).toString(), i.data(IsFolderRole).toBool()});
    }
    return sel.isEmpty() ? nullptr : sel.toMimeData();
}

Qt::DropActions ObjectTableModel::supportedDragActions() const
{
    return Qt::CopyAction | Qt::MoveAction;
}

QVariant ObjectTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_view.size()))
        return {};
    const Row& r = rowAt_(index.row());
    const bool folder = r.flags & Folder;
    const bool marker = r.flags & DeleteMarker;
    const QDateTime modified = (folder || r.mtimeMs == 0)
                                   ? QDateTime()
                                   : QDateTime::fromMSecsSinceEpoch(r.mtimeMs, QTimeZone::UTC);

    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case NameColumn:
            return nameOf_(r);
        case SizeColumn:
            return folder || marker ? QStringLiteral("—") : QLocale().formattedDataSize(r.size);
        case ModifiedColumn:
            return modified.isValid() ? QLocale().toString(modified.toLocalTime(), QLocale::ShortFormat)
                                      : QStringLiteral("—");
        case StorageClassColumn:
            return folder || marker ? QStringLiteral("—") : m_storageClasses[r.storageClass];
        case VersionColumn:
            if (marker)
                return tr("delete marker");
            return m_versionIds[r.version] == QLatin1String("null") ? tr("null (unversioned)") : m_versionIds[r.version];
        default:
            return {};
        }
    case Qt::ToolTipRole:
        if (index.column() == VersionColumn)
            return m_versionIds[r.version].isEmpty() ? QVariant() : QVariant(m_versionIds[r.version]);
        if (index.column() != NameColumn)
            return {};
        if (marker)
            return tr("%1 — deleted here (a delete marker hides the versions below)").arg(r.key);
        return (r.flags & Older) ? tr("%1 — an earlier version").arg(r.key) : QVariant(r.key);
    case Qt::TextAlignmentRole:
        return index.column() == SizeColumn ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant();
    case KeyRole:
        return r.key;
    case IsFolderRole:
        return folder;
    case SizeRole:
        return folder ? qint64(-1) : r.size;
    case ModifiedRole:
        return modified;
    case StorageClassRole:
        return m_storageClasses[r.storageClass];
    case NameRole:
        return nameOf_(r);
    case VersionIdRole:
        return m_versionIds[r.version];
    case IsLatestRole:
        return !(r.flags & Older);
    case IsDeleteMarkerRole:
        return marker;
    default:
        return {};
    }
}

QVariant ObjectTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case NameColumn:
            return tr("Name");
        case SizeColumn:
            return tr("Size");
        case ModifiedColumn:
            return tr("Modified");
        case StorageClassColumn:
            return tr("Class");
        case VersionColumn:
            return tr("Version");
        default:
            return {};
        }
    }
    if (role == Qt::ToolTipRole && section == m_sortColumn && m_state != State::Complete)
        return tr("Sorted among the %n row(s) loaded so far; pages still to come are sorted in "
                  "as they arrive.", nullptr, int(m_rows.size()));
    if (role == Qt::TextAlignmentRole && section == SizeColumn)
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

} // namespace qbucket
