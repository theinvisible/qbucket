#include "ui/LifecyclePage.h"

#include "controllers/AccountSessions.h"
#include "controllers/SetBucketConfigJob.h"
#include "ui/CodeEditor.h"

#include <qbucket/s3/Session.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

namespace qbucket {

namespace {

// "key=value, key2=value2"
QString tagsText(const s3::TagSet& tags)
{
    QStringList parts;
    for (const s3::Tag& t : tags)
        parts.append(t.key + u'=' + t.value);
    return parts.join(QStringLiteral(", "));
}

s3::TagSet tagsFrom(const QString& text)
{
    s3::TagSet out;
    for (const QString& part : text.split(u',', Qt::SkipEmptyParts)) {
        const QString p = part.trimmed();
        if (p.isEmpty())
            continue;
        const qsizetype eq = p.indexOf(u'=');
        out.append({eq < 0 ? p : p.left(eq).trimmed(), eq < 0 ? QString() : p.mid(eq + 1).trimmed()});
    }
    return out;
}

QSpinBox* daysBox()
{
    auto* box = new QSpinBox;
    box->setRange(1, 36500);
    box->setSuffix(LifecyclePage::tr(" days"));
    return box;
}

QString ruleLabel(const s3::LifecycleRule& rule, int index)
{
    QString label = rule.id.isEmpty() ? LifecyclePage::tr("Rule %1").arg(index + 1) : rule.id;
    if (!rule.enabled)
        label += LifecyclePage::tr(" (off)");
    return label;
}

} // namespace

bool LifecyclePage::formCanShow(const s3::LifecycleConfiguration& config)
{
    if (!config.lossless)
        return false;
    for (const s3::LifecycleRule& rule : config.rules) {
        if (rule.transitions.size() > 1)
            return false;
        if (!rule.transitions.isEmpty() && !rule.transitions.first().days)
            return false;
        for (const s3::Tag& t : rule.tags) {
            if (t.key.contains(u',') || t.key.contains(u'=') || t.value.contains(u','))
                return false;
        }
    }
    return true;
}

LifecyclePage::LifecyclePage(const Context& context, QWidget* parent)
    : BucketConfigPage(s3::BucketConfig::Lifecycle, context, parent)
{
    if (context.sessions) {
        if (const s3::Result<s3::Session*> s = context.sessions->session(context.account))
            m_classes = (*s)->options().profile->storageClasses();
    }
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    auto* split = new QSplitter;
    auto* left = new QWidget;
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 6, 0, 0);
    m_rules = new QListWidget;
    m_rules->setObjectName(QStringLiteral("lifecycleRules"));
    leftLay->addWidget(m_rules, 1);
    auto* buttons = new QHBoxLayout;
    m_add = new QPushButton(tr("Add Rule"));
    m_removeRule = new QPushButton(tr("Remove Rule"));
    buttons->addWidget(m_add);
    buttons->addWidget(m_removeRule);
    leftLay->addLayout(buttons);
    split->addWidget(left);
    // Scrolls rather than squeezes the fields when the window is low.
    auto* formScroll = new QScrollArea;
    formScroll->setWidget(buildForm_());
    formScroll->setWidgetResizable(true);
    formScroll->setFrameShape(QFrame::NoFrame);
    split->addWidget(formScroll);
    split->setStretchFactor(1, 1);
    split->setSizes({150, 400});
    m_tabs->addTab(split, tr("Rules"));
    m_xml = new CodeEditor(CodeHighlighter::Language::Xml);
    m_xml->setObjectName(QStringLiteral("lifecycleXml"));
    m_xml->setPlaceholderText(tr("No lifecycle rules are set."));
    m_tabs->addTab(m_xml, tr("XML"));
    editorArea()->addWidget(m_tabs, 1);

    connect(m_rules, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_syncing)
            showRule_(row);
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        s3::LifecycleRule rule;
        rule.id = QStringLiteral("rule-%1").arg(m_config.rules.size() + 1);
        rule.expirationDays = 30;
        m_config.rules.append(rule);
        configToXml_();
        fillList_();
        m_rules->setCurrentRow(int(m_config.rules.size()) - 1);
    });
    connect(m_removeRule, &QPushButton::clicked, this, [this] {
        if (m_current < 0 || m_current >= m_config.rules.size())
            return;
        m_config.rules.removeAt(m_current);
        m_current = -1;
        configToXml_();
        fillList_();
        m_rules->setCurrentRow(std::min(int(m_config.rules.size()) - 1, 0));
    });
    connect(m_xml, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_syncing)
            xmlToConfig_();
        edited_();
    });
}

QWidget* LifecyclePage::buildForm_()
{
    m_form = new QWidget;
    auto* form = new QFormLayout(m_form);
    form->setContentsMargins(10, 6, 0, 0);
    m_id = new QLineEdit;
    form->addRow(tr("ID:"), m_id);
    m_enabled = new QCheckBox(tr("Enabled"));
    form->addRow(QString(), m_enabled);
    m_prefix = new QLineEdit;
    m_prefix->setObjectName(QStringLiteral("lifecyclePrefix"));
    m_prefix->setPlaceholderText(tr("every object"));
    form->addRow(tr("Prefix:"), m_prefix);
    m_tags = new QLineEdit;
    m_tags->setPlaceholderText(tr("key=value, key2=value2"));
    form->addRow(tr("Tags:"), m_tags);

    auto* current = new QLabel(tr("Current versions"));
    current->setObjectName(QStringLiteral("fieldLabel"));
    form->addRow(current);
    m_expire = new QCheckBox(tr("Expire after"));
    m_expire->setObjectName(QStringLiteral("lifecycleExpire"));
    m_expireDays = daysBox();
    auto* expireRow = new QHBoxLayout;
    expireRow->addWidget(m_expire);
    expireRow->addWidget(m_expireDays);
    expireRow->addStretch(1);
    form->addRow(expireRow);
    m_expireOn = new QCheckBox(tr("Expire on"));
    m_expireDate = new QDateEdit;
    m_expireDate->setCalendarPopup(true);
    m_expireDate->setDate(QDate::currentDate().addYears(1));
    auto* dateRow = new QHBoxLayout;
    dateRow->addWidget(m_expireOn);
    dateRow->addWidget(m_expireDate);
    dateRow->addStretch(1);
    form->addRow(dateRow);
    m_markers = new QCheckBox(tr("Remove delete markers no version is left under"));
    form->addRow(m_markers);
    m_transitionRow = new QWidget;
    auto* transitionLay = new QHBoxLayout(m_transitionRow);
    transitionLay->setContentsMargins(0, 0, 0, 0);
    m_transition = new QCheckBox(tr("Move after"));
    m_transitionDays = daysBox();
    m_transitionDays->setMinimum(0);
    m_transitionClass = new QComboBox;
    for (const QString& c : std::as_const(m_classes)) {
        if (c != QLatin1String("STANDARD"))
            m_transitionClass->addItem(c);
    }
    transitionLay->addWidget(m_transition);
    transitionLay->addWidget(m_transitionDays);
    transitionLay->addWidget(new QLabel(tr("to")));
    transitionLay->addWidget(m_transitionClass);
    transitionLay->addStretch(1);
    form->addRow(m_transitionRow);
    m_transitionRow->setVisible(m_transitionClass->count() > 0);

    auto* earlier = new QLabel(tr("Earlier versions"));
    earlier->setObjectName(QStringLiteral("fieldLabel"));
    form->addRow(earlier);
    m_noncurrent = new QCheckBox(tr("Delete for good after"));
    m_noncurrent->setObjectName(QStringLiteral("lifecycleNoncurrent"));
    m_noncurrentDays = daysBox();
    auto* noncurrentRow = new QHBoxLayout;
    noncurrentRow->addWidget(m_noncurrent);
    noncurrentRow->addWidget(m_noncurrentDays);
    noncurrentRow->addStretch(1);
    form->addRow(noncurrentRow);
    m_keepNewer = new QSpinBox;
    m_keepNewer->setRange(0, 100);
    m_keepNewer->setSpecialValueText(tr("none"));
    m_keepNewer->setMaximumWidth(120);
    form->addRow(tr("But keep the newest:"), m_keepNewer);

    auto* uploads = new QLabel(tr("Unfinished uploads"));
    uploads->setObjectName(QStringLiteral("fieldLabel"));
    form->addRow(uploads);
    m_abort = new QCheckBox(tr("Abort after"));
    m_abort->setObjectName(QStringLiteral("lifecycleAbort"));
    m_abortDays = daysBox();
    auto* abortRow = new QHBoxLayout;
    abortRow->addWidget(m_abort);
    abortRow->addWidget(m_abortDays);
    abortRow->addStretch(1);
    form->addRow(abortRow);

    const auto changed = [this] {
        if (!m_syncing)
            formToRule_();
    };
    for (QLineEdit* e : {m_id, m_prefix, m_tags})
        connect(e, &QLineEdit::textEdited, this, changed);
    for (QCheckBox* c : {m_enabled, m_expire, m_expireOn, m_markers, m_transition, m_noncurrent, m_abort})
        connect(c, &QCheckBox::toggled, this, changed);
    for (QSpinBox* s : {m_expireDays, m_transitionDays, m_noncurrentDays, m_keepNewer, m_abortDays})
        connect(s, &QSpinBox::valueChanged, this, changed);
    connect(m_expireDate, &QDateEdit::dateChanged, this, changed);
    connect(m_transitionClass, &QComboBox::currentIndexChanged, this, changed);
    return m_form;
}

void LifecyclePage::fillList_()
{
    m_syncing = true;
    m_rules->clear();
    for (qsizetype i = 0; i < m_config.rules.size(); ++i)
        m_rules->addItem(ruleLabel(m_config.rules[i], int(i)));
    m_syncing = false;
    if (m_current >= m_config.rules.size())
        m_current = -1;
}

void LifecyclePage::showRule_(int index)
{
    m_current = index;
    m_syncing = true;
    const bool valid = index >= 0 && index < m_config.rules.size();
    const s3::LifecycleRule rule = valid ? m_config.rules[index] : s3::LifecycleRule{};
    m_id->setText(rule.id);
    m_enabled->setChecked(rule.enabled);
    m_prefix->setText(rule.prefix);
    m_tags->setText(tagsText(rule.tags));
    m_expire->setChecked(rule.expirationDays.has_value());
    m_expireDays->setValue(rule.expirationDays.value_or(30));
    m_expireOn->setChecked(rule.expirationDate.isValid());
    if (rule.expirationDate.isValid())
        m_expireDate->setDate(rule.expirationDate);
    m_markers->setChecked(rule.expiredObjectDeleteMarker);
    const bool transition = !rule.transitions.isEmpty();
    m_transition->setChecked(transition);
    m_transitionDays->setValue(transition ? rule.transitions.first().days.value_or(30) : 30);
    if (transition) {
        if (m_transitionClass->findText(rule.transitions.first().storageClass) < 0)
            m_transitionClass->addItem(rule.transitions.first().storageClass);
        m_transitionClass->setCurrentText(rule.transitions.first().storageClass);
    }
    m_noncurrent->setChecked(rule.noncurrentDays.has_value());
    m_noncurrentDays->setValue(rule.noncurrentDays.value_or(30));
    m_keepNewer->setValue(rule.newerNoncurrentVersions.value_or(0));
    m_abort->setChecked(rule.abortIncompleteDays.has_value());
    m_abortDays->setValue(rule.abortIncompleteDays.value_or(7));
    m_syncing = false;
    updateEnabled_();
}

void LifecyclePage::formToRule_()
{
    if (m_current < 0 || m_current >= m_config.rules.size())
        return;
    s3::LifecycleRule& rule = m_config.rules[m_current];
    rule.id = m_id->text().trimmed();
    rule.enabled = m_enabled->isChecked();
    rule.prefix = m_prefix->text();
    rule.tags = tagsFrom(m_tags->text());
    if (!rule.tags.isEmpty())
        rule.legacyPrefix = false;   // tags need the <Filter> form
    rule.expirationDays = m_expire->isChecked() ? std::optional<int>(m_expireDays->value()) : std::nullopt;
    rule.expirationDate = m_expireOn->isChecked() ? m_expireDate->date() : QDate();
    rule.expiredObjectDeleteMarker = m_markers->isChecked();
    rule.transitions.clear();
    if (m_transition->isChecked() && m_transitionClass->count() > 0) {
        s3::LifecycleTransition t;
        t.days = m_transitionDays->value();
        t.storageClass = m_transitionClass->currentText();
        rule.transitions.append(t);
    }
    rule.noncurrentDays = m_noncurrent->isChecked() ? std::optional<int>(m_noncurrentDays->value()) : std::nullopt;
    rule.newerNoncurrentVersions = m_noncurrent->isChecked() && m_keepNewer->value() > 0
                                       ? std::optional<int>(m_keepNewer->value())
                                       : std::nullopt;
    rule.abortIncompleteDays = m_abort->isChecked() ? std::optional<int>(m_abortDays->value()) : std::nullopt;
    if (QListWidgetItem* item = m_rules->item(m_current))
        item->setText(ruleLabel(rule, m_current));
    updateEnabled_();
    configToXml_();
}

void LifecyclePage::configToXml_()
{
    m_syncing = true;
    if (m_config.rules.isEmpty()) {
        m_xml->setContents(QString());
    } else if (const s3::Result<QByteArray> xml = s3::buildLifecycleConfiguration(m_config)) {
        m_xml->setContents(SetBucketConfigJob::pretty(kind(), *xml));
    }
    m_syncing = false;
    edited_();
}

void LifecyclePage::xmlToConfig_()
{
    const QByteArray xml = m_xml->contents();
    if (xml.trimmed().isEmpty()) {
        m_config = {};
        m_formUsable = true;
    } else if (const s3::Result<s3::LifecycleConfiguration> c = s3::parseLifecycleConfiguration(xml)) {
        m_formUsable = formCanShow(*c);
        if (m_formUsable)
            m_config = *c;
    } else {
        return;   // being typed: the form keeps what it showed
    }
    const int keep = m_current;
    fillList_();
    m_rules->setCurrentRow(m_config.rules.isEmpty() ? -1 : std::clamp(keep, 0, int(m_config.rules.size()) - 1));
    showRule_(m_rules->currentRow());
    setNote_(m_formUsable ? QString()
                          : tr("These rules have parts the form can't show: edit them as XML."));
    setEditable_(m_editable);
}

void LifecyclePage::updateEnabled_()
{
    const bool form = m_editable && m_formUsable;
    const bool rule = form && m_current >= 0 && m_current < m_config.rules.size();
    m_add->setEnabled(form);
    m_removeRule->setEnabled(rule);
    for (QWidget* w : m_form->findChildren<QWidget*>())
        w->setEnabled(rule);
    if (rule) {
        m_expireDays->setEnabled(m_expire->isChecked());
        m_expireDate->setEnabled(m_expireOn->isChecked());
        m_transitionDays->setEnabled(m_transition->isChecked());
        m_transitionClass->setEnabled(m_transition->isChecked());
        m_noncurrentDays->setEnabled(m_noncurrent->isChecked());
        m_keepNewer->setEnabled(m_noncurrent->isChecked());
        m_abortDays->setEnabled(m_abort->isChecked());
    }
}

void LifecyclePage::showDocument_(const s3::BucketConfigDocument& doc)
{
    m_syncing = true;
    m_xml->setContents(doc.configured ? SetBucketConfigJob::pretty(kind(), doc.body) : QString());
    m_syncing = false;
    m_current = -1;
    xmlToConfig_();
    if (!m_formUsable)
        m_tabs->setCurrentIndex(1);
}

QByteArray LifecyclePage::editedDocument_() const
{
    return m_xml->contents();
}

void LifecyclePage::setEditable_(bool editable)
{
    m_editable = editable;
    m_xml->setReadOnly(!editable);
    updateEnabled_();
}

} // namespace qbucket
