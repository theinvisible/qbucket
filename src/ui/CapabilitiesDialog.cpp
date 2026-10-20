#include "ui/CapabilitiesDialog.h"

#include "controllers/CapabilityController.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

QColor stateColor(s3::CapabilityState state)
{
    switch (state) {
    case s3::CapabilityState::Supported:
        return Theme::successColor();
    case s3::CapabilityState::Unsupported:
        return Theme::dangerColor();
    case s3::CapabilityState::NotPermitted:
        return Theme::warningColor();
    case s3::CapabilityState::Unknown:
        break;
    }
    return Theme::current().text3;
}

QString when(const QDateTime& at)
{
    return at.isValid() ? QLocale().toString(at.toLocalTime(), QLocale::ShortFormat) : QString();
}

QString stateTextFromName(const QString& name)
{
    for (const s3::CapabilityState s : {s3::CapabilityState::Unknown, s3::CapabilityState::Supported,
                                        s3::CapabilityState::Unsupported, s3::CapabilityState::NotPermitted}) {
        if (s3::stateName(s) == name)
            return CapabilityController::stateText(s);
    }
    return name;
}

} // namespace

CapabilitiesDialog::CapabilitiesDialog(CapabilityController* capabilities, const QUuid& account,
                                       const QString& accountName, const QString& bucket, QWidget* parent)
    : QDialog(parent),
      m_caps(capabilities),
      m_account(account)
{
    setWindowTitle(tr("Server Capabilities — %1").arg(accountName));
    resize(760, 620);
    auto* lay = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Show:")));
    m_scope = new QComboBox;
    m_scope->setMinimumWidth(240);
    top->addWidget(m_scope);
    top->addStretch(1);
    m_recheck = new QPushButton(tr("Check Again"));
    top->addWidget(m_recheck);
    lay->addLayout(top);

    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    lay->addWidget(m_summary);

    m_table = new QTreeWidget;
    m_table->setObjectName(QStringLiteral("capabilityTable"));
    m_table->setRootIsDecorated(false);
    m_table->setUniformRowHeights(true);
    m_table->setHeaderLabels({tr("Feature"), tr("State"), tr("Known from"), tr("Checked"), tr("Answer")});
    m_table->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->header()->resizeSection(0, 230);
    m_table->header()->resizeSection(1, 110);
    m_table->header()->resizeSection(2, 150);
    m_table->header()->resizeSection(3, 120);
    lay->addWidget(m_table, 3);

    lay->addWidget(new QLabel(tr("Changes")));
    m_history = new QTreeWidget;
    m_history->setObjectName(QStringLiteral("capabilityHistory"));
    m_history->setRootIsDecorated(false);
    m_history->setUniformRowHeights(true);
    m_history->setHeaderLabels({tr("When"), tr("Where"), tr("Feature"), tr("Change")});
    m_history->header()->resizeSection(0, 130);
    m_history->header()->resizeSection(1, 140);
    m_history->header()->resizeSection(2, 200);
    lay->addWidget(m_history, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(m_scope, &QComboBox::currentIndexChanged, this, &CapabilitiesDialog::refresh_);
    connect(m_recheck, &QPushButton::clicked, this, [this] {
        m_caps->recheck(m_account, scope());
        refresh_();
    });
    connect(m_caps, &CapabilityController::capabilitiesUpdated, this, [this](const QUuid& id, const QString& b) {
        if (id != m_account)
            return;
        if (!b.isEmpty() && m_scope->findData(b) < 0)
            fillScopes_(scope());
        refresh_();
    });
    connect(m_caps, &CapabilityController::changesDetected, this, [this](const QUuid& id) {
        if (id == m_account)
            loadHistory_();
    });

    fillScopes_(bucket);
    refresh_();
    loadHistory_();
}

QString CapabilitiesDialog::scope() const
{
    return m_scope->currentData().toString();
}

void CapabilitiesDialog::setScope(const QString& bucket)
{
    const int i = m_scope->findData(bucket);
    if (i >= 0)
        m_scope->setCurrentIndex(i);
}

void CapabilitiesDialog::fillScopes_(const QString& select)
{
    const QSignalBlocker block(m_scope);
    m_scope->clear();
    m_scope->addItem(tr("Server"), QString());
    QStringList buckets = m_caps->knownBuckets(m_account);
    if (!select.isEmpty() && !buckets.contains(select))
        buckets.append(select);
    buckets.sort(Qt::CaseInsensitive);
    for (const QString& b : std::as_const(buckets))
        m_scope->addItem(tr("Bucket “%1”").arg(b), b);
    const int i = m_scope->findData(select);
    m_scope->setCurrentIndex(i < 0 ? 0 : i);
}

void CapabilitiesDialog::refresh_()
{
    const QString bucket = scope();
    const bool probing = m_caps->isProbing(m_account, bucket);
    m_recheck->setEnabled(!probing && m_caps->isLoaded(m_account));
    m_recheck->setText(probing ? tr("Checking…") : tr("Check Again"));

    const s3::CapabilitySet server = m_caps->serverSet(m_account);
    const std::optional<s3::CapabilitySet> set = bucket.isEmpty() ? std::optional(server)
                                                                  : m_caps->bucketSet(m_account, bucket);
    QStringList summary;
    if (!set || !set->probedAt.isValid())
        summary.append(probing ? tr("Being checked…") : tr("Not checked yet — the provider profile's knowledge is shown."));
    else
        summary.append(tr("Checked %1.").arg(when(set->probedAt)));
    if (!server.fingerprint.serverHeader.isEmpty())
        summary.append(tr("Server software: %1.").arg(server.fingerprint.serverHeader));
    if (set && !bucket.isEmpty()) {
        const QString versioning = set->bucketState.versioning.isEmpty() ? tr("never enabled")
                                                                         : set->bucketState.versioning;
        summary.append(tr("Versioning: %1.").arg(versioning));
        if (set->bucketState.objectLockEnabled)
            summary.append(*set->bucketState.objectLockEnabled ? tr("Object lock: enabled.")
                                                               : tr("Object lock: not enabled."));
    }
    m_summary->setText(summary.join(u' '));

    m_table->clear();
    const s3::CapabilitySet profile = m_caps->profileSet(m_account);
    for (const s3::Feature f : s3::allFeatures()) {
        s3::CapabilityEntry e = bucket.isEmpty() ? server.entry(f) : m_caps->entry(m_account, bucket, f);
        if (bucket.isEmpty() && e.state == s3::CapabilityState::Unknown && profile.contains(f))
            e = profile.entry(f);
        QString answer;
        if (e.httpStatus > 0)
            answer = QStringLiteral("HTTP %1 %2").arg(e.httpStatus).arg(e.s3Code).trimmed();
        else if (!e.s3Code.isEmpty())
            answer = e.s3Code;
        // Nothing known from anywhere: no source to name.
        const bool nowhere = e.state == s3::CapabilityState::Unknown && !e.checkedAt.isValid();
        auto* item = new QTreeWidgetItem(m_table, {CapabilityController::featureName(f),
                                                   CapabilityController::stateText(e.state),
                                                   nowhere ? QStringLiteral("—") : CapabilityController::sourceText(e.source),
                                                   when(e.checkedAt), answer});
        item->setForeground(1, stateColor(e.state));
        const QString why = CapabilityController::reason(f, e, false);
        for (int c = 0; c < item->columnCount(); ++c)
            item->setToolTip(c, why.isEmpty() ? s3::featureId(f) : why);
    }
}

void CapabilitiesDialog::loadHistory_()
{
    m_caps->loadHistory(m_account, this, [this](const QList<QJsonObject>& entries) {
        m_history->clear();
        for (auto it = entries.crbegin(); it != entries.crend(); ++it) {   // newest first
            const QJsonObject& e = *it;
            const std::optional<s3::Feature> f = s3::featureFromId(e.value(QStringLiteral("feature")).toString());
            const QString bucket = e.value(QStringLiteral("bucket")).toString();
            new QTreeWidgetItem(m_history, {
                when(QDateTime::fromString(e.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs)),
                bucket.isEmpty() ? tr("Server") : bucket,
                f ? CapabilityController::featureName(*f) : e.value(QStringLiteral("feature")).toString(),
                tr("%1 → %2").arg(stateTextFromName(e.value(QStringLiteral("before")).toString()),
                                  stateTextFromName(e.value(QStringLiteral("after")).toString())),
            });
        }
        if (entries.isEmpty())
            new QTreeWidgetItem(m_history, {QString(), QString(), tr("No changes recorded.")});
    });
}

} // namespace qbucket
