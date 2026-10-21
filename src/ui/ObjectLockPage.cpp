#include "ui/ObjectLockPage.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace qbucket {

ObjectLockPage::ObjectLockPage(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::ObjectLock, context, parent)
{
    m_default = new QCheckBox(tr("Give new versions a default retention"));
    m_default->setObjectName(QStringLiteral("lockDefault"));
    editorArea()->addWidget(m_default);
    auto* form = new QFormLayout;
    m_mode = new QComboBox;
    m_mode->setObjectName(QStringLiteral("lockMode"));
    m_mode->addItem(tr("GOVERNANCE — can be lifted by someone allowed to bypass it"), int(s3::ObjectLockMode::Governance));
    m_mode->addItem(tr("COMPLIANCE — nobody can lift it, not even the account owner"), int(s3::ObjectLockMode::Compliance));
    form->addRow(tr("Mode:"), m_mode);
    auto* period = new QHBoxLayout;
    m_period = new QSpinBox;
    m_period->setObjectName(QStringLiteral("lockPeriod"));
    m_period->setRange(1, 36500);
    m_period->setValue(30);
    m_unit = new QComboBox;
    m_unit->addItem(tr("days"));
    m_unit->addItem(tr("years"));
    period->addWidget(m_period);
    period->addWidget(m_unit);
    period->addStretch(1);
    form->addRow(tr("Period:"), period);
    editorArea()->addLayout(form);
    auto* hint = new QLabel(tr("The retention runs from when a version is written. Deleting such a version, or "
                               "shortening its retention, is refused until it has run out."));
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    editorArea()->addWidget(hint);
    editorArea()->addStretch(1);

    connect(m_default, &QCheckBox::toggled, this, [this] {
        updateEnabled_();
        edited_();
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] { edited_(); });
    connect(m_period, &QSpinBox::valueChanged, this, [this] { edited_(); });
    connect(m_unit, &QComboBox::currentIndexChanged, this, [this] { edited_(); });
}

s3::ObjectLockConfiguration ObjectLockPage::edited() const
{
    s3::ObjectLockConfiguration c;
    c.enabled = m_loaded.enabled;
    if (m_default->isChecked()) {
        c.mode = s3::ObjectLockMode(m_mode->currentData().toInt());
        if (m_unit->currentIndex() == 1)
            c.years = m_period->value();
        else
            c.days = m_period->value();
    }
    return c;
}

void ObjectLockPage::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_loaded = {};
    if (doc.configured) {
        if (const s3::Result<s3::ObjectLockConfiguration> c = s3::parseObjectLockConfiguration(doc.body))
            m_loaded = *c;
    }
    m_default->setChecked(m_loaded.mode.has_value());
    m_mode->setCurrentIndex(m_mode->findData(int(m_loaded.mode.value_or(s3::ObjectLockMode::Governance))));
    m_unit->setCurrentIndex(m_loaded.years ? 1 : 0);
    m_period->setValue(m_loaded.years.value_or(m_loaded.days.value_or(30)));
    if (!m_loaded.enabled)
        setNote_(tr("Object lock is not enabled on this bucket. It can only be enabled when a bucket is created."));
    else if (!m_loaded.lossless)
        setNote_(tr("The server reports more than QBucket shows; applying a change keeps only what is shown."));
    else
        setNote_(QString());
    updateEnabled_();
}

QByteArray ObjectLockPage::editedDocument_() const
{
    return m_loaded.enabled ? s3::buildObjectLockConfiguration(edited()) : QByteArray();
}

void ObjectLockPage::setEditable_(bool editable)
{
    m_editable = editable;
    updateEnabled_();
}

void ObjectLockPage::updateEnabled_()
{
    const bool on = m_editable && m_loaded.enabled;
    m_default->setEnabled(on);
    m_mode->setEnabled(on && m_default->isChecked());
    m_period->setEnabled(on && m_default->isChecked());
    m_unit->setEnabled(on && m_default->isChecked());
}

bool ObjectLockPage::differs_() const
{
    if (!m_loaded.enabled)
        return false;
    s3::ObjectLockConfiguration now = edited();
    s3::ObjectLockConfiguration was = m_loaded;
    now.lossless = was.lossless = true;
    return !(now == was);
}

QString ObjectLockPage::stateText_(const s3::BucketConfigDocument& doc) const
{
    if (!doc.configured)
        return tr("Object lock is not enabled");
    const s3::Result<s3::ObjectLockConfiguration> c = s3::parseObjectLockConfiguration(doc.body);
    if (!c || !c->enabled)
        return tr("Object lock is not enabled");
    const std::optional<s3::ObjectLockMode> mode = c->mode;
    if (!mode)
        return tr("Object lock is enabled, without a default retention");
    const std::optional<int> years = c->years;
    const QString period = years ? tr("%n year(s)", nullptr, *years) : tr("%n day(s)", nullptr, c->days.value_or(0));
    return tr("Object lock is enabled; new versions get %1 retention for %2").arg(s3::objectLockModeName(*mode), period);
}

QString ObjectLockPage::problem_() const
{
    const QString p = s3::objectLockProblem(edited());
    return p.isEmpty() ? QString() : tr("Not possible: %1").arg(p);
}

} // namespace qbucket
