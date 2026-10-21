#include "ui/DetailPane.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/FolderSizeTask.h"
#include "controllers/ObjectPropertyJob.h"
#include "controllers/OperationController.h"
#include "models/ObjectTableModel.h"

#include <qbucket/s3/Session.h>
#include <qbucket/s3/providers/ProviderRegistry.h>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QLabel* makeHint(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("emptyState"));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    return label;
}

QString exactSize(qint64 bytes)
{
    const QLocale locale;
    return DetailPane::tr("%1 (%2 bytes)").arg(locale.formattedDataSize(bytes), locale.toString(bytes));
}

} // namespace

DetailPane::DetailPane(AccountSessions* sessions, QWidget* parent)
    : QWidget(parent),
      m_sessions(sessions)
{
    setObjectName(QStringLiteral("detailPane"));
    setMinimumWidth(260);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* title = new QLabel(tr("Details"));
    title->setObjectName(QStringLiteral("paneTitle"));
    lay->addWidget(title);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setExpanding(false);

    // Properties
    m_propsStack = new QStackedWidget;
    m_propsHint = makeHint(tr("Select an object to see its details."));
    m_propsStack->addWidget(m_propsHint);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    m_props = new QWidget;
    m_props->setObjectName(QStringLiteral("detailInner"));
    m_form = new QFormLayout(m_props);
    m_form->setContentsMargins(14, 10, 14, 10);
    m_form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    m_form->setVerticalSpacing(4);
    scroll->setWidget(m_props);
    m_propsStack->addWidget(scroll);
    m_tabs->addTab(m_propsStack, tr("Properties"));

    // Headers and user metadata, read-only in M2.
    m_metaStack = new QStackedWidget;
    m_metaHint = makeHint(tr("Select an object to see its headers and metadata."));
    m_metaStack->addWidget(m_metaHint);
    m_headers = new QTreeWidget;
    m_headers->setObjectName(QStringLiteral("headerList"));
    m_headers->setColumnCount(2);
    m_headers->setHeaderLabels({tr("Header"), tr("Value")});
    m_headers->setRootIsDecorated(true);
    m_headers->setUniformRowHeights(true);
    m_headers->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_headers->header()->resizeSection(0, 150);
    auto* metaPage = new QWidget;
    auto* metaLay = new QVBoxLayout(metaPage);
    metaLay->setContentsMargins(0, 0, 0, 0);
    metaLay->addWidget(m_headers, 1);
    m_editMetadata = new QPushButton(tr("Edit…"));
    m_editMetadata->setObjectName(QStringLiteral("editMetadata"));
    connect(m_editMetadata, &QPushButton::clicked, this, &DetailPane::editMetadataRequested);
    auto* editRow = new QHBoxLayout;
    editRow->setContentsMargins(8, 4, 8, 6);
    editRow->addStretch(1);
    editRow->addWidget(m_editMetadata);
    metaLay->addLayout(editRow);
    m_metaStack->addWidget(metaPage);
    m_tabs->addTab(m_metaStack, tr("Metadata"));

    buildTagsTab_();
    buildVersionsTab_();
    buildAccessTab_();
    connect(m_tabs, &QTabWidget::currentChanged, this, &DetailPane::loadVisibleTab_);
    lay->addWidget(m_tabs, 1);

    m_headDelay.setSingleShot(true);
    m_headDelay.setInterval(kHeadDelayMs);
    connect(&m_headDelay, &QTimer::timeout, this, &DetailPane::requestHead_);
}

DetailPane::~DetailPane() = default;

void DetailPane::setControllers(OperationController* operations, CapabilityController* capabilities)
{
    m_ops = operations;
    m_caps = capabilities;
    if (m_caps) {
        connect(m_caps, &CapabilityController::capabilitiesUpdated, this, [this] {
            updateFeatureTabs_();
        });
    }
}

void DetailPane::buildTagsTab_()
{
    m_tagsStack = new QStackedWidget;
    m_tagsHint = makeHint(tr("Select an object to see its tags."));
    m_tagsStack->addWidget(m_tagsHint);
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8);
    m_tagTable = new QTableWidget(0, 2);
    m_tagTable->setObjectName(QStringLiteral("tagTable"));
    m_tagTable->setHorizontalHeaderLabels({tr("Key"), tr("Value")});
    m_tagTable->horizontalHeader()->setStretchLastSection(true);
    m_tagTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tagTable->setColumnWidth(0, 120);
    m_tagTable->verticalHeader()->hide();
    lay->addWidget(m_tagTable, 1);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    connect(add, &QPushButton::clicked, this, [this] {
        addTagRow_(QString(), QString());
        m_tagTable->editItem(m_tagTable->item(m_tagTable->rowCount() - 1, 0));
    });
    auto* remove = new QPushButton(tr("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_tagTable->currentRow() >= 0)
            m_tagTable->removeRow(m_tagTable->currentRow());
    });
    m_applyTags = new QPushButton(tr("Apply…"));
    m_applyTags->setObjectName(QStringLiteral("applyTags"));
    connect(m_applyTags, &QPushButton::clicked, this, &DetailPane::applyTags_);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    row->addWidget(m_applyTags);
    lay->addLayout(row);
    m_tagsReason = new QLabel;
    m_tagsReason->setWordWrap(true);
    m_tagsReason->setObjectName(QStringLiteral("hint"));
    lay->addWidget(m_tagsReason);
    m_tagsStack->addWidget(page);
    m_tabs->addTab(m_tagsStack, tr("Tags"));
}

void DetailPane::buildAccessTab_()
{
    m_accessStack = new QStackedWidget;
    m_accessHint = makeHint(tr("Select an object to see who may access it."));
    m_accessStack->addWidget(m_accessHint);
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8);
    m_owner = new QLabel;
    m_owner->setWordWrap(true);
    m_owner->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_owner);
    m_grants = new QTreeWidget;
    m_grants->setObjectName(QStringLiteral("grantList"));
    m_grants->setColumnCount(2);
    m_grants->setHeaderLabels({tr("Grantee"), tr("Permission")});
    m_grants->setRootIsDecorated(false);
    m_grants->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_grants->header()->resizeSection(0, 170);
    lay->addWidget(m_grants, 1);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Access:")));
    m_canned = new QComboBox;
    m_canned->setObjectName(QStringLiteral("cannedAcl"));
    row->addWidget(m_canned, 1);
    m_applyAccess = new QPushButton(tr("Apply…"));
    m_applyAccess->setObjectName(QStringLiteral("applyAccess"));
    connect(m_applyAccess, &QPushButton::clicked, this, &DetailPane::applyAccess_);
    row->addWidget(m_applyAccess);
    lay->addLayout(row);
    m_accessReason = new QLabel;
    m_accessReason->setWordWrap(true);
    m_accessReason->setObjectName(QStringLiteral("hint"));
    lay->addWidget(m_accessReason);
    m_accessStack->addWidget(page);
    m_tabs->addTab(m_accessStack, tr("Access"));
}

void DetailPane::buildVersionsTab_()
{
    m_versionsStack = new QStackedWidget;
    m_versionsHint = makeHint(tr("Select an object to see its versions."));
    m_versionsStack->addWidget(m_versionsHint);
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8);
    m_versionList = new QTreeWidget;
    m_versionList->setObjectName(QStringLiteral("versionList"));
    m_versionList->setColumnCount(3);
    m_versionList->setHeaderLabels({tr("Modified"), tr("Size"), tr("Version")});
    m_versionList->setRootIsDecorated(false);
    m_versionList->setUniformRowHeights(true);
    m_versionList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_versionList->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_versionList->header()->resizeSection(0, 140);
    m_versionList->header()->resizeSection(1, 80);
    lay->addWidget(m_versionList, 1);
    m_versionsNote = new QLabel;
    m_versionsNote->setWordWrap(true);
    m_versionsNote->setObjectName(QStringLiteral("hint"));
    lay->addWidget(m_versionsNote);
    m_versionsStack->addWidget(page);
    m_tabs->addTab(m_versionsStack, tr("Versions"));
}

void DetailPane::addTagRow_(const QString& key, const QString& value)
{
    const int row = m_tagTable->rowCount();
    m_tagTable->insertRow(row);
    m_tagTable->setItem(row, 0, new QTableWidgetItem(key));
    m_tagTable->setItem(row, 1, new QTableWidgetItem(value));
}

// Tabs of features the server doesn't have are hidden.
void DetailPane::updateFeatureTabs_()
{
    if (!m_caps || m_account.isNull() || m_bucket.isEmpty())
        return;
    const auto unsupported = [this](s3::Feature f) {
        return m_caps->entry(m_account, m_bucket, f).state == s3::CapabilityState::Unsupported;
    };
    m_tabs->setTabVisible(TagsTab, !unsupported(s3::Feature::ObjectTagging));
    m_tabs->setTabVisible(VersionsTab, !unsupported(s3::Feature::ObjectVersionsListing));
    m_tabs->setTabVisible(AccessTab, !unsupported(s3::Feature::ObjectAcl));
}

void DetailPane::loadVisibleTab_()
{
    if (m_tabs->currentIndex() == VersionsTab && !m_versionsKey.isEmpty() && m_versionsFor != m_versionsKey) {
        loadVersions_();
        return;
    }
    if (m_key.isEmpty())
        return;
    if (m_tabs->currentIndex() == TagsTab && m_tagsFor != m_key)
        loadTags_();
    else if (m_tabs->currentIndex() == AccessTab && m_accessFor != m_key)
        loadAccess_();
}

// Every version and delete marker of one key: listed with the key as the
// prefix, kept where the key matches exactly. Every key listed starts with
// it and the key itself sorts first, so the first other key ends the walk.
void DetailPane::loadVersions_()
{
    m_versionsFor = m_versionsKey;
    m_versionsRun = std::make_unique<QObject>();
    m_versions.clear();
    m_versionsCapped = false;
    m_versionsHint->setText(tr("Loading…"));
    m_versionsStack->setCurrentIndex(0);
    const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account) : s3::Result<s3::Session*>(
        s3::Error::make(s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        m_versionsHint->setText(ConnectionTester::describe(session.error()));
        return;
    }
    s3::ListObjectVersionsParams p;
    p.bucket = m_bucket;
    p.prefix = m_versionsFor;
    p.maxKeys = 1000;
    m_versionsPager = std::make_unique<s3::ListObjectVersionsPager>(**session, p);
    fetchVersions_();
}

void DetailPane::fetchVersions_()
{
    const QString key = m_versionsFor;
    m_versionsPager->fetchNext(m_versionsRun.get(), [this, key](const s3::Result<s3::ListObjectVersionsPage>& page) {
        if (key != m_versionsFor)
            return;
        if (!page) {
            m_versionsHint->setText(page.error().category == s3::ErrorCategory::Unsupported
                                        ? tr("This server keeps no object versions.")
                                        : ConnectionTester::describe(page.error()));
            updateFeatureTabs_();
            emit versionsLoaded();
            return;
        }
        bool pastKey = false;
        for (const s3::VersionEntry& e : page->entries) {
            if (e.key == key)
                m_versions.append(e);
            else
                pastKey = true;
        }
        if (m_versions.size() >= kMaxVersionsShown) {
            m_versionsCapped = true;
            m_versions.resize(kMaxVersionsShown);
            pastKey = true;
        }
        if (!pastKey && m_versionsPager->hasMore()) {
            fetchVersions_();
            return;
        }
        m_versionsPager.reset();
        showVersions_();
    });
}

void DetailPane::showVersions_()
{
    const QLocale locale;
    m_versionList->clear();
    for (const s3::VersionEntry& v : std::as_const(m_versions)) {
        const QString when = locale.toString(v.lastModified.toLocalTime(), QLocale::ShortFormat);
        const QString id = v.versionId == QLatin1String("null") ? tr("null (unversioned)") : v.versionId;
        QString state;
        if (v.isDeleteMarker)
            state = v.isLatest ? tr("deleted (current)") : tr("deleted");
        else if (v.isLatest)
            state = tr("current");
        auto* item = new QTreeWidgetItem(m_versionList, {when, v.isDeleteMarker ? QStringLiteral("—") : locale.formattedDataSize(v.size),
                                                         state.isEmpty() ? id : QStringLiteral("%1 · %2").arg(state, id)});
        item->setData(0, Qt::UserRole, v.versionId);
        item->setToolTip(2, v.versionId);
        if (v.isDeleteMarker) {
            QFont f = item->font(0);
            f.setItalic(true);
            for (int c = 0; c < 3; ++c)
                item->setFont(c, f);
        }
        if (v.versionId == m_versionId) {
            item->setSelected(true);
            m_versionList->setCurrentItem(item);
        }
    }
    QString note;
    if (m_versions.isEmpty())
        note = tr("No versions — the bucket may not keep any.");
    else if (m_versionsCapped)
        note = tr("Showing the newest %n version(s); open the versions view for all.", nullptr, int(m_versions.size()));
    else
        note = tr("%n version(s), newest first.", nullptr, int(m_versions.size()));
    m_versionsNote->setText(note);
    m_versionsStack->setCurrentIndex(1);
    emit versionsLoaded();
}

void DetailPane::loadTags_()
{
    m_tagsFor = m_key;
    m_tagsRun = std::make_unique<QObject>();
    m_tagsHint->setText(tr("Loading…"));
    m_tagsStack->setCurrentIndex(0);
    const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account) : s3::Result<s3::Session*>(
        s3::Error::make(s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        m_tagsHint->setText(ConnectionTester::describe(session.error()));
        return;
    }
    const QString key = m_key;
    (*session)->getObjectTagging(m_bucket, key, m_versionId, m_tagsRun.get(), [this, key](const s3::Result<s3::ObjectTagging>& r) {
        if (key != m_key)
            return;
        if (!r) {
            m_tagsHint->setText(r.error().category == s3::ErrorCategory::Unsupported
                                    ? tr("This server has no object tags.")
                                    : ConnectionTester::describe(r.error()));
            updateFeatureTabs_();
            emit tagsLoaded();
            return;
        }
        m_tagTable->setRowCount(0);
        for (const s3::Tag& t : r->tags)
            addTagRow_(t.key, t.value);
        OperationController::Check gate{false, tr("Read-only here.")};
        if (showsOlderVersion_())
            gate.reason = tr("An earlier version: only the current one can be changed.");
        else if (m_ops)
            gate = m_ops->check(m_account, m_bucket, {s3::Feature::ObjectTaggingWrite}, true);
        m_applyTags->setEnabled(gate.allowed);
        m_tagsReason->setText(gate.allowed ? QString() : gate.reason);
        m_tagsReason->setVisible(!gate.allowed);
        m_tagTable->setEditTriggers(gate.allowed ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                                                 : QAbstractItemView::NoEditTriggers);
        m_tagsStack->setCurrentIndex(1);
        emit tagsLoaded();
    });
}

void DetailPane::loadAccess_()
{
    m_accessFor = m_key;
    m_accessRun = std::make_unique<QObject>();
    m_accessHint->setText(tr("Loading…"));
    m_accessStack->setCurrentIndex(0);
    const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account) : s3::Result<s3::Session*>(
        s3::Error::make(s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        m_accessHint->setText(ConnectionTester::describe(session.error()));
        return;
    }
    const QString key = m_key;
    (*session)->getObjectAcl(m_bucket, key, m_versionId, m_accessRun.get(), [this, key](const s3::Result<s3::AccessControlPolicy>& r) {
        if (key != m_key)
            return;
        if (!r) {
            m_accessHint->setText(r.error().category == s3::ErrorCategory::Unsupported
                                      ? tr("This server has no object permissions (ACLs).")
                                      : ConnectionTester::describe(r.error()));
            updateFeatureTabs_();
            emit accessLoaded();
            return;
        }
        m_owner->setText(tr("Owner: %1").arg(r->ownerName.isEmpty() ? r->ownerId : r->ownerName));
        m_grants->clear();
        for (const s3::Grant& g : r->grants) {
            QString who;
            switch (g.grantee) {
            case s3::Grant::Grantee::Group:
                who = g.uri.section(u'/', -1);   // AllUsers, AuthenticatedUsers, …
                break;
            case s3::Grant::Grantee::Email:
                who = g.email;
                break;
            case s3::Grant::Grantee::CanonicalUser:
                who = g.id == r->ownerId ? tr("Owner") : (g.displayName.isEmpty() ? g.id : g.displayName);
                break;
            }
            auto* item = new QTreeWidgetItem(m_grants, {who, g.permission});
            item->setToolTip(0, g.grantee == s3::Grant::Grantee::Group ? g.uri : g.id);
        }
        // The canned ACLs the provider offers; a custom ACL can only be replaced.
        QStringList canned = QStringList{QStringLiteral("private"), QStringLiteral("public-read")};
        if (m_ops) {
            if (const std::optional<Account> a = m_ops->accounts()->account(m_account))
                canned = s3::ProviderRegistry::builtin().findOrGeneric(a->providerId)->cannedObjectAcls();
        }
        const std::optional<QString> current = s3::cannedAclOf(*r);
        m_canned->clear();
        m_canned->addItems(canned);
        if (current && canned.contains(*current)) {
            m_canned->setCurrentText(*current);
        } else {
            m_canned->insertItem(0, tr("custom (grants above)"));
            m_canned->setCurrentIndex(0);
        }
        OperationController::Check gate{false, tr("Read-only here.")};
        if (showsOlderVersion_())
            gate.reason = tr("An earlier version: only the current one can be changed.");
        else if (m_ops)
            gate = m_ops->check(m_account, m_bucket, {s3::Feature::ObjectAclWrite}, true);
        m_canned->setEnabled(gate.allowed);
        m_applyAccess->setEnabled(gate.allowed);
        m_accessReason->setText(gate.allowed ? QString() : gate.reason);
        m_accessReason->setVisible(!gate.allowed);
        m_accessStack->setCurrentIndex(1);
        emit accessLoaded();
    });
}

void DetailPane::applyTags_()
{
    if (!m_ops || m_tagsFor.isEmpty())
        return;
    const std::optional<Account> account = m_ops->accounts()->account(m_account);
    if (!account)
        return;
    s3::TagSet tags;
    for (int row = 0; row < m_tagTable->rowCount(); ++row) {
        const QString key = m_tagTable->item(row, 0) ? m_tagTable->item(row, 0)->text().trimmed() : QString();
        if (!key.isEmpty())
            tags.append({key, m_tagTable->item(row, 1) ? m_tagTable->item(row, 1)->text() : QString()});
    }
    if (const QString problem = s3::tagSetProblem(tags); !problem.isEmpty()) {
        m_tagsReason->setText(problem);
        m_tagsReason->setVisible(true);
        return;
    }
    Job* job = ObjectPropertyJob::tags(*account, m_bucket, m_tagsFor, tags);
    connect(job, &Job::finished, this, [this, job, key = m_tagsFor] {
        if (key == m_key && job->phase() == Job::Phase::Finished && job->outcome() != Job::Outcome::Refused)
            loadTags_();
    });
    m_ops->start(job);
}

void DetailPane::applyAccess_()
{
    if (!m_ops || m_accessFor.isEmpty())
        return;
    const std::optional<Account> account = m_ops->accounts()->account(m_account);
    const QString canned = m_canned->currentText();
    if (!account || canned.startsWith(tr("custom")))
        return;
    Job* job = ObjectPropertyJob::acl(*account, m_bucket, m_accessFor, canned);
    connect(job, &Job::finished, this, [this, job, key = m_accessFor] {
        if (key == m_key && job->phase() == Job::Phase::Finished && job->outcome() != Job::Outcome::Refused)
            loadAccess_();
    });
    m_ops->start(job);
}

void DetailPane::clear()
{
    m_headDelay.stop();
    m_run.reset();
    m_key.clear();
    m_versionId.clear();
    m_versionsKey.clear();
    m_versionsRun.reset();
    m_versionsPager.reset();
    m_versionsFor.clear();
    m_meta = s3::ObjectMeta();
    showHint_(tr("Select an object to see its details."));
    m_versionsHint->setText(tr("Select an object to see its versions."));
    m_versionsStack->setCurrentIndex(0);
}

void DetailPane::setSelection(const QUuid& account, const QString& bucket, const QModelIndexList& rows)
{
    m_headDelay.stop();
    m_run.reset();   // an answer for the previous selection is dropped
    m_tagsRun.reset();
    m_accessRun.reset();
    m_versionsRun.reset();
    m_versionsPager.reset();
    m_sizeTask.reset();   // a count of the previous selection stops
    m_tagsFor.clear();
    m_accessFor.clear();
    m_versionsFor.clear();
    m_account = account;
    m_bucket = bucket;
    m_key.clear();
    m_versionId.clear();
    m_latest = true;
    m_versionsKey.clear();
    m_meta = s3::ObjectMeta();
    updateFeatureTabs_();
    m_tagsHint->setText(tr("Select a single object to see its tags."));
    m_tagsStack->setCurrentIndex(0);
    m_accessHint->setText(tr("Select a single object to see who may access it."));
    m_accessStack->setCurrentIndex(0);
    m_versionsHint->setText(tr("Select a single object to see its versions."));
    m_versionsStack->setCurrentIndex(0);
    m_editMetadata->setEnabled(true);
    m_editMetadata->setToolTip(QString());

    QModelIndexList unique;
    QSet<int> seen;
    for (const QModelIndex& i : rows) {
        if (!seen.contains(i.row())) {
            seen.insert(i.row());
            unique.append(i.siblingAtColumn(0));
        }
    }
    if (unique.isEmpty()) {
        showHint_(tr("Select an object to see its details."));
        return;
    }

    resetFields_();
    m_propsStack->setCurrentIndex(1);
    if (unique.size() > 1) {
        qint64 bytes = 0;
        int folders = 0;
        for (const QModelIndex& i : unique) {
            if (i.data(ObjectTableModel::IsFolderRole).toBool())
                ++folders;
            else
                bytes += i.data(ObjectTableModel::SizeRole).toLongLong();
        }
        addField_(tr("Selection"), tr("%n item(s)", nullptr, int(unique.size())), false);
        addField_(tr("Size of the objects"), exactSize(bytes));
        if (folders > 0) {
            addField_(tr("Folders"), tr("%n folder(s), their contents not counted yet", nullptr, folders), false);
            QStringList prefixes;
            for (const QModelIndex& i : unique) {
                if (i.data(ObjectTableModel::IsFolderRole).toBool())
                    prefixes.append(i.data(ObjectTableModel::KeyRole).toString());
            }
            addSizeRow_(prefixes);
        }
        m_metaHint->setText(tr("Select a single object to see its headers and metadata."));
        m_metaStack->setCurrentIndex(0);
        return;
    }

    const QModelIndex row = unique.first();
    const QString key = row.data(ObjectTableModel::KeyRole).toString();
    addField_(tr("Name"), row.data(ObjectTableModel::NameRole).toString());
    addField_(tr("Location"), QStringLiteral("s3://%1/%2").arg(bucket, key));
    if (row.data(ObjectTableModel::IsFolderRole).toBool()) {
        addField_(tr("Kind"), tr("Folder (a common key prefix)"), false);
        addSizeRow_({key});
        m_metaHint->setText(tr("Folders have no headers or metadata of their own."));
        m_metaStack->setCurrentIndex(0);
        return;
    }
    m_versionsKey = key;
    m_versionId = row.data(ObjectTableModel::VersionIdRole).toString();
    m_latest = row.data(ObjectTableModel::IsLatestRole).toBool();
    if (row.data(ObjectTableModel::IsDeleteMarkerRole).toBool()) {
        // Nothing to ask the server about: the marker is all there is.
        const QDateTime when = row.data(ObjectTableModel::ModifiedRole).toDateTime();
        addField_(tr("Kind"), m_latest ? tr("Delete marker — the object is deleted here; earlier versions remain")
                                       : tr("Delete marker (an earlier one)"), false);
        if (when.isValid())
            addField_(tr("Deleted"), QLocale().toString(when.toLocalTime(), QLocale::LongFormat));
        addField_(tr("Version ID"), m_versionId);
        m_metaHint->setText(tr("A delete marker has no content, headers or metadata."));
        m_metaStack->setCurrentIndex(0);
        m_tagsHint->setText(tr("A delete marker has no tags."));
        m_accessHint->setText(tr("A delete marker has no permissions."));
        loadVisibleTab_();
        return;
    }
    if (showsOlderVersion_()) {
        m_editMetadata->setEnabled(false);
        m_editMetadata->setToolTip(tr("An earlier version: only the current one can be changed."));
    }
    // What the listing already knows, at once; the rest after HeadObject.
    addField_(tr("Size"), exactSize(row.data(ObjectTableModel::SizeRole).toLongLong()));
    addField_(tr("Details"), tr("Loading…"), false);
    m_metaHint->setText(tr("Loading…"));
    m_metaStack->setCurrentIndex(0);
    m_key = key;
    m_headDelay.start();
    if (m_tabs->currentIndex() == VersionsTab)
        loadVisibleTab_();
}

void DetailPane::requestHead_()
{
    if (!m_sessions || m_key.isEmpty())
        return;
    const s3::Result<s3::Session*> session = m_sessions->session(m_account);
    if (!session) {
        showHint_(ConnectionTester::describe(session.error()));
        return;
    }
    m_run = std::make_unique<QObject>();
    const QString key = m_key;
    (*session)->headObject(m_bucket, key, m_versionId, m_run.get(), [this, key](const s3::Result<s3::ObjectMeta>& r) {
        if (key != m_key)
            return;
        if (r) {
            showMeta_(*r);
            loadVisibleTab_();
        } else {
            resetFields_();
            addField_(tr("Location"), QStringLiteral("s3://%1/%2").arg(m_bucket, key));
            addField_(tr("Error"), ConnectionTester::describe(r.error()));
            m_metaHint->setText(ConnectionTester::describe(r.error()));
        }
        emit headFinished();
    });
}

void DetailPane::showMeta_(const s3::ObjectMeta& meta)
{
    m_meta = meta;
    const QLocale locale;
    resetFields_();
    const qsizetype slash = meta.key.lastIndexOf(u'/', meta.key.size() - 2);
    addField_(tr("Name"), meta.key.mid(slash + 1));
    addField_(tr("Location"), QStringLiteral("s3://%1/%2").arg(m_bucket, meta.key));
    if (meta.size >= 0)
        addField_(tr("Size"), exactSize(meta.size));
    if (meta.lastModified.isValid())
        addField_(tr("Modified"), locale.toString(meta.lastModified.toLocalTime(), QLocale::LongFormat));
    addField_(tr("Storage class"), meta.storageClass.isEmpty() ? QStringLiteral("STANDARD") : meta.storageClass);
    addField_(tr("Content type"), meta.contentType.isEmpty() ? tr("(none)") : meta.contentType);
    addField_(tr("ETag"), meta.etag);
    if (!meta.versionId.isEmpty())
        addField_(tr("Version ID"), showsOlderVersion_() ? tr("%1 (an earlier version)").arg(meta.versionId) : meta.versionId);

    m_headers->clear();
    auto* system = new QTreeWidgetItem(m_headers, {tr("Headers")});
    auto* user = new QTreeWidgetItem(m_headers, {tr("User metadata")});
    for (const auto& [name, value] : meta.responseHeaders) {
        if (name.startsWith(QLatin1String("x-amz-meta-"), Qt::CaseInsensitive))
            continue;
        new QTreeWidgetItem(system, {name, value});
    }
    for (auto it = meta.userMetadata.cbegin(); it != meta.userMetadata.cend(); ++it)
        new QTreeWidgetItem(user, {it.key(), it.value()});
    if (meta.userMetadata.isEmpty())
        new QTreeWidgetItem(user, {tr("(none)")});
    m_headers->expandAll();
    m_metaStack->setCurrentIndex(1);
}

void DetailPane::showHint_(const QString& text)
{
    m_propsHint->setText(text);
    m_propsStack->setCurrentIndex(0);
    m_metaHint->setText(text);
    m_metaStack->setCurrentIndex(0);
}

void DetailPane::resetFields_()
{
    m_sizeButton = nullptr;
    m_sizeLabel = nullptr;
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
}

void DetailPane::addSizeRow_(const QStringList& prefixes)
{
    auto* name = new QLabel(tr("Contents"));
    name->setObjectName(QStringLiteral("fieldLabel"));
    auto* row = new QWidget;
    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    m_sizeLabel = new QLabel(tr("not counted"));
    m_sizeLabel->setWordWrap(true);
    m_sizeLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_sizeButton = new QPushButton(tr("Calculate Size"));
    m_sizeButton->setObjectName(QStringLiteral("calculateSize"));
    m_sizeButton->setEnabled(m_sessions != nullptr);
    connect(m_sizeButton, &QPushButton::clicked, this, [this, prefixes] {
        if (m_sizeTask && m_sizeTask->isRunning())
            m_sizeTask->cancel();
        else
            startSize_(prefixes);
    });
    lay->addWidget(m_sizeLabel, 1);
    lay->addWidget(m_sizeButton);
    m_form->addRow(name, row);
}

void DetailPane::startSize_(const QStringList& prefixes)
{
    const s3::Result<s3::Session*> session = m_sessions->session(m_account);
    if (!session) {
        m_sizeLabel->setText(ConnectionTester::describe(session.error()));
        return;
    }
    m_sizeTask = std::make_unique<FolderSizeTask>(*session, m_bucket, prefixes);
    FolderSizeTask* task = m_sizeTask.get();
    const QLocale locale;
    connect(task, &FolderSizeTask::progress, this, [this, task, locale] {
        m_sizeLabel->setText(tr("counting… %n object(s), %1 so far", nullptr, int(task->totals().objects))
                                 .arg(locale.formattedDataSize(task->totals().bytes)));
    });
    connect(task, &FolderSizeTask::finished, this, [this, task, locale] {
        m_sizeButton->setText(tr("Calculate Size"));
        if (!task->isComplete()) {
            m_sizeLabel->setText(tr("not counted: %1").arg(task->errorText()));
            return;
        }
        const FolderSizeTask::Totals& t = task->totals();
        QString text = tr("%n object(s), %1", nullptr, int(t.objects)).arg(exactSize(t.bytes));
        if (t.byClass.size() > 1) {
            QStringList classes;
            for (auto it = t.byClass.cbegin(); it != t.byClass.cend(); ++it)
                classes.append(QStringLiteral("%1: %2").arg(it.key(), locale.formattedDataSize(it.value().second)));
            text += u'\n' + classes.join(QStringLiteral(", "));
        }
        m_sizeLabel->setText(text);
        m_sizeLabel->setToolTip(text);
    });
    m_sizeButton->setText(tr("Cancel"));
    m_sizeLabel->setText(tr("counting…"));
    task->start();
}

void DetailPane::addField_(const QString& label, const QString& value, bool selectable)
{
    auto* name = new QLabel(label);
    name->setObjectName(QStringLiteral("fieldLabel"));
    auto* text = new QLabel(value);
    text->setWordWrap(true);
    // Keys and ETags have no spaces to wrap at: never let them widen the
    // pane; the full value is in the tooltip and copies in full.
    text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->setToolTip(value);
    if (selectable)
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_form->addRow(name, text);
}

} // namespace qbucket
