#include "ui/OtherConfigPage.h"

#include "controllers/AccountSessions.h"
#include "controllers/ConnectionTester.h"
#include "controllers/SetBucketConfigJob.h"
#include "ui/CodeEditor.h"

#include <qbucket/s3/Session.h>

#include <QHeaderView>
#include <QLabel>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

constexpr int kKindRole = Qt::UserRole;
constexpr int kBodyRole = Qt::UserRole + 1;

QString nameOf(s3::BucketConfig kind)
{
    using K = s3::BucketConfig;
    switch (kind) {
    case K::Tagging: return OtherConfigPage::tr("Bucket tags");
    case K::Website: return OtherConfigPage::tr("Static website");
    case K::Logging: return OtherConfigPage::tr("Access logging");
    case K::Replication: return OtherConfigPage::tr("Replication");
    case K::Encryption: return OtherConfigPage::tr("Default encryption");
    case K::Notifications: return OtherConfigPage::tr("Event notifications");
    case K::OwnershipControls: return OtherConfigPage::tr("Ownership controls");
    default: break;
    }
    return s3::bucketConfigName(kind);
}

} // namespace

OtherConfigPage::OtherConfigPage(const BucketConfigPage::Context& context, QWidget* parent)
    : QWidget(parent),
      m_context(context)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* hint = new QLabel(tr("Further settings of the bucket, as the server reports them. QBucket doesn't change "
                               "them."));
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto* split = new QSplitter(Qt::Vertical);
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("otherConfigs"));
    m_list->setHeaderLabels({tr("Setting"), tr("State")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_list->setColumnWidth(0, 200);
    for (const s3::BucketConfig kind : {s3::BucketConfig::Tagging, s3::BucketConfig::Website, s3::BucketConfig::Logging,
                                        s3::BucketConfig::Replication, s3::BucketConfig::Encryption,
                                        s3::BucketConfig::Notifications, s3::BucketConfig::OwnershipControls}) {
        auto* item = new QTreeWidgetItem({nameOf(kind), QStringLiteral("…")});
        item->setData(0, kKindRole, int(kind));
        m_list->addTopLevelItem(item);
    }
    split->addWidget(m_list);
    m_viewer = new CodeEditor(CodeHighlighter::Language::Xml);
    m_viewer->setObjectName(QStringLiteral("otherConfigDocument"));
    m_viewer->setReadOnly(true);
    split->addWidget(m_viewer);
    split->setSizes({180, 220});
    lay->addWidget(split, 1);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        m_viewer->setContents(item ? item->data(0, kBodyRole).toString() : QString());
    });
}

OtherConfigPage::~OtherConfigPage() = default;

void OtherConfigPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    ensureLoaded();
}

void OtherConfigPage::ensureLoaded()
{
    if (m_started || !m_context.sessions)
        return;
    const s3::Result<s3::Session*> session = m_context.sessions->session(m_context.account);
    if (!session)
        return;
    m_started = true;
    m_run = std::make_unique<QObject>();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_list->topLevelItem(i);
        const auto kind = s3::BucketConfig(item->data(0, kKindRole).toInt());
        ++m_pending;
        (*session)->getBucketConfig(m_context.bucket, kind, m_run.get(),
                                    [this, item, kind](const s3::Result<s3::BucketConfigDocument>& r) {
            --m_pending;
            QString state;
            if (!r) {
                state = r.error().category == s3::ErrorCategory::Unsupported
                            ? tr("Not supported")
                            : (r.error().category == s3::ErrorCategory::NotPermitted ? tr("No permission")
                                                                                       : ConnectionTester::describe(r.error()));
            } else {
                state = r->configured ? tr("Set") : tr("Not set");
                item->setData(0, kBodyRole, r->configured ? SetBucketConfigJob::pretty(kind, r->body) : QString());
            }
            item->setText(1, state);
            if (item == m_list->currentItem())
                m_viewer->setContents(item->data(0, kBodyRole).toString());
            if (m_pending == 0)
                emit loaded();
        });
    }
}

} // namespace qbucket
