#include "ui/BucketPropertiesDialog.h"

#include "accounts/AccountStore.h"
#include "controllers/AccountSessions.h"
#include "controllers/CapabilityController.h"
#include "controllers/ConnectionTester.h"
#include "controllers/OperationController.h"
#include "controllers/FolderSizeTask.h"
#include "controllers/SetBucketVersioningJob.h"
#include "ui/AclSection.h"
#include "ui/CorsPage.h"
#include "ui/LifecyclePage.h"
#include "ui/ObjectLockPage.h"
#include "ui/OtherConfigPage.h"
#include "ui/PolicyPage.h"
#include "ui/PublicAccessBlockSection.h"

#include <qbucket/s3/Session.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QString statusText(s3::VersioningStatus status)
{
    switch (status) {
    case s3::VersioningStatus::Enabled: return BucketPropertiesDialog::tr("Enabled");
    case s3::VersioningStatus::Suspended: return BucketPropertiesDialog::tr("Suspended");
    case s3::VersioningStatus::Never: break;
    }
    return BucketPropertiesDialog::tr("Never enabled");
}

QLabel* valueLabel()
{
    auto* l = new QLabel(QStringLiteral("…"));
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setWordWrap(true);
    return l;
}

} // namespace

BucketPropertiesDialog::BucketPropertiesDialog(const QUuid& account, const QString& bucket, AccountSessions* sessions,
                                               OperationController* operations, CapabilityController* capabilities,
                                               QWidget* parent)
    : QDialog(parent),
      m_account(account),
      m_bucket(bucket),
      m_sessions(sessions),
      m_ops(operations),
      m_caps(capabilities)
{
    setWindowTitle(tr("Properties of “%1”").arg(bucket));
    setMinimumSize(780, 520);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    m_sidebar = new QWidget;
    m_sidebar->setObjectName(QStringLiteral("prefSidebar"));
    m_sidebar->setFixedWidth(180);
    auto* sideLay = new QVBoxLayout(m_sidebar);
    sideLay->setContentsMargins(8, 10, 8, 10);
    sideLay->setSpacing(2);
    sideLay->addStretch(1);
    body->addWidget(m_sidebar);
    m_pages = new QStackedWidget;
    body->addWidget(m_pages, 1);
    root->addLayout(body, 1);

    addCategory_(tr("General"), buildGeneralPage_());
    addCategory_(tr("Versioning"), buildVersioningPage_());
    const BucketConfigPage::Context ctx{m_account, m_bucket, m_sessions, m_ops, m_caps};
    m_policy = new PolicyPage(ctx);
    addCategory_(tr("Policy"), framed_(tr("Bucket policy"),
                                       tr("Who may do what with the bucket and its objects, as a JSON policy."), m_policy));
    m_cors = new CorsPage(ctx);
    addCategory_(tr("CORS"), framed_(tr("CORS"),
                                     tr("Which other web sites may use the bucket from a browser."), m_cors));
    m_lifecycle = new LifecyclePage(ctx);
    addCategory_(tr("Lifecycle"),
                 framed_(tr("Lifecycle rules"),
                         tr("What the server deletes or moves by itself, after a time."), m_lifecycle));
    addCategory_(tr("Access"), buildAccessPage_());
    m_lock = new ObjectLockPage(ctx);
    addCategory_(tr("Object Lock"),
                 framed_(tr("Object lock"), tr("Retention that keeps versions from being deleted."), m_lock));
    m_other = new OtherConfigPage(ctx);
    addCategory_(tr("More"), framed_(tr("More settings"), QString(), m_other));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->setContentsMargins(12, 10, 12, 12);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    if (m_caps) {
        connect(m_caps, &CapabilityController::capabilitiesUpdated, this, [this](const QUuid& a, const QString& b) {
            if (a == m_account && (b.isEmpty() || b == m_bucket))
                updateButtons_();
        });
    }
    load_();
}

BucketPropertiesDialog::~BucketPropertiesDialog() = default;

void BucketPropertiesDialog::addCategory_(const QString& label, QWidget* page)
{
    auto* sideLay = static_cast<QVBoxLayout*>(m_sidebar->layout());
    const int index = m_pages->addWidget(page);
    auto* btn = new QToolButton;
    btn->setObjectName(QStringLiteral("prefCat"));
    btn->setText(label);
    btn->setCheckable(true);
    btn->setAutoExclusive(true);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setProperty("page", index);
    connect(btn, &QToolButton::clicked, this, [this, index] { m_pages->setCurrentIndex(index); });
    sideLay->insertWidget(sideLay->count() - 1, btn);
    if (index == 0)
        btn->setChecked(true);
}

void BucketPropertiesDialog::showPage(Page page)
{
    m_pages->setCurrentIndex(int(page));
    for (QToolButton* btn : m_sidebar->findChildren<QToolButton*>(QStringLiteral("prefCat")))
        btn->setChecked(btn->property("page").toInt() == int(page));
}

QWidget* BucketPropertiesDialog::buildGeneralPage_()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);
    auto* form = new QFormLayout;
    auto* name = new QLabel(m_bucket);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Name:"), name);
    QString accountName;
    if (m_ops) {
        if (const std::optional<Account> a = m_ops->accounts()->account(m_account))
            accountName = a->name;
    }
    form->addRow(tr("Account:"), new QLabel(accountName));
    m_region = valueLabel();
    form->addRow(tr("Region:"), m_region);
    m_generalVersioning = valueLabel();
    form->addRow(tr("Versioning:"), m_generalVersioning);
    m_objectLock = valueLabel();
    form->addRow(tr("Object lock:"), m_objectLock);
    m_created = valueLabel();
    form->addRow(tr("Created:"), m_created);
    auto* sizeRow = new QHBoxLayout;
    m_size = new QLabel(tr("not counted"));
    m_size->setObjectName(QStringLiteral("bucketSize"));
    m_sizeButton = new QPushButton(tr("Calculate"));
    m_sizeButton->setObjectName(QStringLiteral("bucketSizeButton"));
    connect(m_sizeButton, &QPushButton::clicked, this, &BucketPropertiesDialog::calculateSize_);
    sizeRow->addWidget(m_size, 1);
    sizeRow->addWidget(m_sizeButton);
    form->addRow(tr("Objects:"), sizeRow);
    lay->addLayout(form);
    auto* caps = new QPushButton(tr("Server Capabilities…"));
    connect(caps, &QPushButton::clicked, this, &BucketPropertiesDialog::capabilitiesRequested);
    auto* row = new QHBoxLayout;
    row->addWidget(caps);
    row->addStretch(1);
    lay->addLayout(row);
    lay->addStretch(1);
    return page;
}

QWidget* BucketPropertiesDialog::buildVersioningPage_()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 18);
    auto* form = new QFormLayout;
    m_status = valueLabel();
    m_status->setObjectName(QStringLiteral("versioningStatus"));
    form->addRow(tr("Status:"), m_status);
    m_mfa = valueLabel();
    form->addRow(tr("MFA delete:"), m_mfa);
    lay->addLayout(form);
    auto* hint = new QLabel(tr("With versioning, overwriting or deleting an object keeps the earlier version: it can "
                               "be restored from the Versions view. Once enabled, versioning can only be suspended."));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hint"));
    lay->addWidget(hint);
    auto* row = new QHBoxLayout;
    m_enable = new QPushButton(tr("Enable Versioning…"));
    m_enable->setObjectName(QStringLiteral("enableVersioning"));
    m_suspend = new QPushButton(tr("Suspend Versioning…"));
    m_suspend->setObjectName(QStringLiteral("suspendVersioning"));
    connect(m_enable, &QPushButton::clicked, this, [this] { setVersioning_(s3::VersioningStatus::Enabled); });
    connect(m_suspend, &QPushButton::clicked, this, [this] { setVersioning_(s3::VersioningStatus::Suspended); });
    row->addWidget(m_enable);
    row->addWidget(m_suspend);
    row->addStretch(1);
    lay->addLayout(row);
    m_reason = new QLabel;
    m_reason->setWordWrap(true);
    m_reason->setObjectName(QStringLiteral("hint"));
    lay->addWidget(m_reason);
    lay->addStretch(1);
    return page;
}

// Fresh from the server whenever the dialog opens or the setting changed.
void BucketPropertiesDialog::load_()
{
    m_current.reset();
    updateButtons_();
    const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account)
                                                        : s3::Result<s3::Session*>(s3::Error::make(
                                                              s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        const QString why = ConnectionTester::describe(session.error());
        m_status->setText(why);
        m_generalVersioning->setText(why);
        m_region->setText(why);
        return;
    }
    m_run = std::make_unique<QObject>();
    (*session)->listBuckets(m_run.get(), [this](const s3::Result<s3::BucketList>& r) {
        if (!r) {
            m_created->setText(ConnectionTester::describe(r.error()));
            return;
        }
        m_created->setText(tr("not reported"));
        for (const s3::BucketInfo& b : r->buckets) {
            if (b.name == m_bucket && b.creationDate.isValid())
                m_created->setText(QLocale().toString(b.creationDate.toLocalTime(), QLocale::LongFormat));
        }
    });
    (*session)->getBucketLocation(m_bucket, m_run.get(), [this](const s3::Result<QString>& r) {
        m_region->setText(r ? *r : ConnectionTester::describe(r.error()));
    });
    (*session)->getBucketVersioning(m_bucket, m_run.get(), [this](const s3::Result<s3::BucketVersioning>& r) {
        if (!r) {
            const QString why = r.error().category == s3::ErrorCategory::Unsupported
                                    ? tr("not supported by this server")
                                    : ConnectionTester::describe(r.error());
            m_status->setText(why);
            m_generalVersioning->setText(why);
            m_mfa->setText(QStringLiteral("—"));
        } else {
            m_current = r->status;
            m_status->setText(statusText(r->status));
            m_generalVersioning->setText(statusText(r->status));
            m_mfa->setText(!r->mfaDelete ? tr("not reported") : (*r->mfaDelete ? tr("Enabled") : tr("Disabled")));
        }
        updateButtons_();
        emit versioningLoaded();
    });
}

void BucketPropertiesDialog::updateButtons_()
{
    const CapabilityController::Versioning v = m_caps ? m_caps->versioningOf(m_account, m_bucket)
                                                      : CapabilityController::Versioning();
    m_objectLock->setText(!v.objectLock ? tr("unknown") : (*v.objectLock ? tr("enabled") : tr("not enabled")));
    QString why;
    if (m_running) {
        why = tr("A change is under way.");
    } else if (!m_current) {
        why = tr("The current setting is not known yet.");
    } else if (!m_ops) {
        why = tr("Read-only here.");
    } else {
        const OperationController::Check gate =
            m_ops->check(m_account, m_bucket, {s3::Feature::BucketVersioningWrite}, true);
        if (!gate.allowed)
            why = gate.reason;
        else if (v.mfaDelete.value_or(false))
            why = tr("MFA delete is enabled: QBucket can't send the MFA code a change needs.");
    }
    m_enable->setEnabled(why.isEmpty() && m_current != s3::VersioningStatus::Enabled);
    QString suspendWhy = why;
    if (suspendWhy.isEmpty() && v.objectLock.value_or(false))
        suspendWhy = tr("The bucket has object lock: its versioning can't be suspended.");
    m_suspend->setEnabled(suspendWhy.isEmpty() && m_current == s3::VersioningStatus::Enabled);
    m_reason->setText(suspendWhy);
    m_reason->setVisible(!suspendWhy.isEmpty());
}

QWidget* BucketPropertiesDialog::framed_(const QString& title, const QString& about, QWidget* content)
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(20, 18, 20, 14);
    auto* heading = new QLabel(title);
    QFont f = heading->font();
    f.setPointSizeF(f.pointSizeF() * 1.15);
    f.setBold(true);
    heading->setFont(f);
    lay->addWidget(heading);
    if (!about.isEmpty()) {
        auto* line = new QLabel(about);
        line->setObjectName(QStringLiteral("hint"));
        line->setWordWrap(true);
        lay->addWidget(line);
    }
    lay->addWidget(content, 1);
    return page;
}

QWidget* BucketPropertiesDialog::buildAccessPage_()
{
    const BucketConfigPage::Context ctx{m_account, m_bucket, m_sessions, m_ops, m_caps};
    auto* content = new QWidget;
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* aclBox = new QGroupBox(tr("Bucket ACL"));
    auto* aclLay = new QVBoxLayout(aclBox);
    m_acl = new AclSection(ctx);
    aclLay->addWidget(m_acl);
    lay->addWidget(aclBox);
    auto* statusRow = new QHBoxLayout;
    statusRow->addWidget(new QLabel(tr("Public through the policy:")));
    m_policyStatus = new QLabel(QStringLiteral("…"));
    m_policyStatus->setObjectName(QStringLiteral("policyStatus"));
    statusRow->addWidget(m_policyStatus, 1);
    lay->addLayout(statusRow);
    auto* pabBox = new QGroupBox(tr("Public access block"));
    auto* pabLay = new QVBoxLayout(pabBox);
    m_pab = new PublicAccessBlockSection(ctx);
    pabLay->addWidget(m_pab);
    lay->addWidget(pabBox);
    lay->addStretch(1);
    // The policy status follows the policy: read with the ACL, again after a change.
    const auto readStatus = [this] {
        const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account)
                                                            : s3::Result<s3::Session*>(s3::Error::make(
                                                                  s3::ErrorCategory::Internal, tr("not connected")));
        if (!session)
            return;
        (*session)->getBucketConfig(m_bucket, s3::BucketConfig::PolicyStatus, m_policyStatus,
                                    [this](const s3::Result<s3::BucketConfigDocument>& r) {
            if (!r) {
                m_policyStatus->setText(r.error().category == s3::ErrorCategory::Unsupported
                                            ? tr("not supported by this server")
                                            : ConnectionTester::describe(r.error()));
            } else if (!r->configured) {
                m_policyStatus->setText(tr("no (no policy)"));
            } else {
                const s3::Result<bool> isPublic = s3::parsePolicyStatus(r->body);
                m_policyStatus->setText(!isPublic ? tr("unknown") : (*isPublic ? tr("yes") : tr("no")));
            }
        });
    };
    connect(m_acl, &BucketConfigPage::loaded, this, readStatus);
    connect(m_policy, &BucketConfigPage::loaded, this, readStatus);
    auto* scroll = new QScrollArea;
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return framed_(tr("Access"), tr("Who may reach the bucket without its policy."), scroll);
}

void BucketPropertiesDialog::calculateSize_()
{
    if (m_sizeTask) {
        m_sizeTask->cancel();
        return;
    }
    const s3::Result<s3::Session*> session = m_sessions ? m_sessions->session(m_account)
                                                        : s3::Result<s3::Session*>(s3::Error::make(
                                                              s3::ErrorCategory::Internal, tr("not connected")));
    if (!session) {
        m_size->setText(ConnectionTester::describe(session.error()));
        return;
    }
    m_sizeTask = new FolderSizeTask(*session, m_bucket, {QString()}, this);
    m_sizeButton->setText(tr("Stop"));
    const auto show = [this] {
        const QLocale locale;
        m_size->setText(tr("%1 — %2").arg(locale.toString(m_sizeTask->totals().objects),
                                                   locale.formattedDataSize(m_sizeTask->totals().bytes)));
    };
    connect(m_sizeTask, &FolderSizeTask::progress, this, show);
    connect(m_sizeTask, &FolderSizeTask::finished, this, [this, show] {
        show();
        if (!m_sizeTask->errorText().isEmpty())
            m_size->setText(m_sizeTask->errorText());
        else if (!m_sizeTask->isComplete())
            m_size->setText(m_size->text() + tr(" (stopped)"));
        else
            m_size->setText(m_size->text() + tr(" — current versions"));
        m_sizeButton->setText(tr("Calculate"));
        m_sizeTask->deleteLater();
        m_sizeTask = nullptr;
    });
    m_sizeTask->start();
}

bool BucketPropertiesDialog::hasUnsavedChanges() const
{
    for (const BucketConfigPage* page : findChildren<BucketConfigPage*>()) {
        if (page->isDirty())
            return true;
    }
    return false;
}

void BucketPropertiesDialog::reject()
{
    if (m_askBeforeDiscarding && hasUnsavedChanges()) {
        const auto answer = QMessageBox::question(this, tr("Changes not applied"),
                                                  tr("Some settings were changed but not applied. Close anyway and "
                                                     "discard them?"),
                                                  QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard)
            return;
    }
    QDialog::reject();
}

void BucketPropertiesDialog::setVersioning_(s3::VersioningStatus status)
{
    if (!m_ops)
        return;
    const std::optional<Account> account = m_ops->accounts()->account(m_account);
    if (!account)
        return;
    auto* job = new SetBucketVersioningJob(*account, m_bucket, status);
    m_running = true;
    updateButtons_();
    connect(job, &Job::finished, this, [this] {
        m_running = false;
        load_();
    });
    m_ops->start(job);
}

} // namespace qbucket
