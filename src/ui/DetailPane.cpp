#include "ui/DetailPane.h"

#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"
#include "models/ObjectTableModel.h"

#include <QFormLayout>
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
    m_metaStack->addWidget(m_headers);
    m_tabs->addTab(m_metaStack, tr("Metadata"));

    for (const QString& name : {tr("Tags"), tr("Versions"), tr("Access")})
        m_tabs->addTab(makeHint(tr("Not available yet.")), name);
    lay->addWidget(m_tabs, 1);

    m_headDelay.setSingleShot(true);
    m_headDelay.setInterval(kHeadDelayMs);
    connect(&m_headDelay, &QTimer::timeout, this, &DetailPane::requestHead_);
}

void DetailPane::clear()
{
    m_headDelay.stop();
    m_run.reset();
    m_key.clear();
    m_meta = s3::ObjectMeta();
    showHint_(tr("Select an object to see its details."));
}

void DetailPane::setSelection(const QUuid& account, const QString& bucket, const QModelIndexList& rows)
{
    m_headDelay.stop();
    m_run.reset();   // an answer for the previous selection is dropped
    m_account = account;
    m_bucket = bucket;
    m_key.clear();
    m_meta = s3::ObjectMeta();

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
        if (folders > 0)
            addField_(tr("Folders"), tr("%n folder(s), their contents not counted", nullptr, folders), false);
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
        m_metaHint->setText(tr("Folders have no headers or metadata of their own."));
        m_metaStack->setCurrentIndex(0);
        return;
    }
    // What the listing already knows, at once; the rest after HeadObject.
    addField_(tr("Size"), exactSize(row.data(ObjectTableModel::SizeRole).toLongLong()));
    addField_(tr("Details"), tr("Loading…"), false);
    m_metaHint->setText(tr("Loading…"));
    m_metaStack->setCurrentIndex(0);
    m_key = key;
    m_headDelay.start();
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
    (*session)->headObject(m_bucket, key, QString(), m_run.get(), [this, key](const s3::Result<s3::ObjectMeta>& r) {
        if (key != m_key)
            return;
        if (r) {
            showMeta_(*r);
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
        addField_(tr("Version ID"), meta.versionId);

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
    while (m_form->rowCount() > 0)
        m_form->removeRow(0);
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
