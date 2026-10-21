#include "ui/PresignDialog.h"

#include "controllers/PresignService.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace qbucket {

namespace {

enum Unit { Minutes = 60, Hours = 3600, Days = 86400 };

} // namespace

PresignDialog::PresignDialog(PresignService* service, const QUuid& account, const QString& bucket,
                             const QStringList& keys, int folders, QWidget* parent)
    : QDialog(parent),
      m_service(service),
      m_account(account),
      m_bucket(bucket),
      m_keys(keys)
{
    setWindowTitle(tr("Presigned Links"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(keys.size() == 1 ? tr("A link to s3://%1/%2 that works without credentials until it "
                                                   "expires.").arg(bucket, keys.first())
                                              : tr("Links to %n object(s) in s3://%1 that work without credentials "
                                                   "until they expire.", nullptr, int(keys.size())).arg(bucket));
    intro->setWordWrap(true);
    lay->addWidget(intro);
    if (folders > 0) {
        auto* note = new QLabel(tr("%n selected folder(s) left out: links are for objects.", nullptr, folders));
        note->setWordWrap(true);
        lay->addWidget(note);
    }

    auto* form = new QFormLayout;
    m_method = new QComboBox;
    m_method->addItem(tr("Download (GET)"), QByteArray("GET"));
    m_method->addItem(tr("Upload (PUT)"), QByteArray("PUT"));
    form->addRow(tr("Link for:"), m_method);
    auto* expiry = new QHBoxLayout;
    m_amount = new QSpinBox;
    m_amount->setRange(1, 999);
    m_amount->setValue(1);
    m_unit = new QComboBox;
    m_unit->addItem(tr("minutes"), int(Minutes));
    m_unit->addItem(tr("hours"), int(Hours));
    m_unit->addItem(tr("days"), int(Days));
    m_unit->setCurrentIndex(1);
    expiry->addWidget(m_amount);
    expiry->addWidget(m_unit);
    const int max = m_service->maxExpirySeconds(account);
    expiry->addWidget(new QLabel(tr("(at most %1)").arg(max % Days == 0 ? tr("%n day(s)", nullptr, max / Days)
                                                                         : tr("%n hour(s)", nullptr, max / Hours))));
    expiry->addStretch(1);
    form->addRow(tr("Valid for:"), expiry);
    m_download = new QCheckBox(tr("Save as a file in the browser (instead of showing it)"));
    form->addRow(QString(), m_download);
    lay->addLayout(form);

    m_warnings = new QLabel;
    m_warnings->setWordWrap(true);
    m_warnings->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::warningColor().name()));
    lay->addWidget(m_warnings);
    m_reason = new QLabel;
    m_reason->setWordWrap(true);
    lay->addWidget(m_reason);

    m_generate = new QPushButton(tr("Create Links"));
    m_generate->setObjectName(QStringLiteral("createLinks"));
    auto* top = new QHBoxLayout;
    top->addStretch(1);
    top->addWidget(m_generate);
    lay->addLayout(top);
    m_output = new QPlainTextEdit;
    m_output->setObjectName(QStringLiteral("links"));
    m_output->setReadOnly(true);
    m_output->setLineWrapMode(QPlainTextEdit::NoWrap);
    lay->addWidget(m_output, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    m_save = buttons->addButton(tr("Save List…"), QDialogButtonBox::ActionRole);
    m_copy->setEnabled(false);
    m_save->setEnabled(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_copy, &QPushButton::clicked, this, [this] { QGuiApplication::clipboard()->setText(m_output->toPlainText()); });
    connect(m_save, &QPushButton::clicked, this, &PresignDialog::save_);
    connect(m_generate, &QPushButton::clicked, this, &PresignDialog::generate_);
    lay->addWidget(buttons);

    for (QComboBox* box : {m_method, m_unit})
        connect(box, &QComboBox::currentIndexChanged, this, &PresignDialog::update_);
    connect(m_amount, &QSpinBox::valueChanged, this, &PresignDialog::update_);
    resize(720, 460);
    update_();
}

int PresignDialog::expiresSeconds() const
{
    const qint64 secs = qint64(m_amount->value()) * m_unit->currentData().toInt();
    return int(std::min<qint64>(secs, m_service->maxExpirySeconds(m_account)));
}

void PresignDialog::setVersionIds(const QStringList& versionIds)
{
    m_versionIds = versionIds;
    m_method->setCurrentIndex(m_method->findData(QByteArray("GET")));
    m_method->setEnabled(versionIds.isEmpty());
    m_method->setToolTip(versionIds.isEmpty() ? QString()
                                              : tr("Earlier versions can only be downloaded: an upload link writes "
                                                   "the current version."));
    update_();
}

void PresignDialog::update_()
{
    const QByteArray method = m_method->currentData().toByteArray();
    m_download->setEnabled(method == "GET");
    const qint64 asked = qint64(m_amount->value()) * m_unit->currentData().toInt();
    QStringList warnings = PresignService::warnings(method, expiresSeconds());
    if (asked > m_service->maxExpirySeconds(m_account))
        warnings.prepend(tr("Shortened to the provider's limit."));
    m_warnings->setText(warnings.join(u'\n'));
    m_warnings->setVisible(!warnings.isEmpty());
    const OperationController::Check gate = m_service->check(m_account, m_bucket, method);
    m_generate->setEnabled(gate.allowed);
    m_reason->setText(gate.reason);
    m_reason->setVisible(!gate.allowed);
}

void PresignDialog::generate_()
{
    PresignService::Request r;
    r.account = m_account;
    r.bucket = m_bucket;
    r.keys = m_keys;
    r.versionIds = m_versionIds;
    r.method = m_method->currentData().toByteArray();
    r.expiresSeconds = expiresSeconds();
    r.download = m_download->isChecked() && m_download->isEnabled();
    m_generate->setEnabled(false);
    const QString expires = QLocale().toString(QDateTime::currentDateTime().addSecs(r.expiresSeconds), QLocale::ShortFormat);
    m_service->presign(r, this, [this, expires](const QList<PresignService::Link>& links) {
        QStringList lines;
        m_csv = {QStringLiteral("key,link,expires")};
        const auto quote = [](QString s) { return u'"' + s.replace(u'"', QStringLiteral("\"\"")) + u'"'; };
        for (const PresignService::Link& l : links) {
            const QString text = l.url.isValid() ? QString::fromLatin1(l.url.toEncoded()) : tr("(failed: %1)").arg(l.error);
            lines.append(links.size() == 1 ? text : l.key + u'\t' + text);
            if (l.url.isValid())
                m_csv.append(quote(l.key) + u',' + quote(text) + u',' + quote(expires));
        }
        m_output->setPlainText(lines.join(u'\n'));
        m_copy->setEnabled(m_csv.size() > 1);
        m_save->setEnabled(m_csv.size() > 1);
        update_();
        emit linksReady();
    });
}

void PresignDialog::save_()
{
    auto* dlg = new QFileDialog(this, tr("Save List"), QString(), tr("CSV (*.csv);;Text (*.txt)"));
    dlg->setAcceptMode(QFileDialog::AcceptSave);
    dlg->setDefaultSuffix(QStringLiteral("csv"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QFileDialog::fileSelected, this, [this](const QString& path) {
        QFile f(path);
        const QByteArray data = path.endsWith(QLatin1String(".txt")) ? m_output->toPlainText().toUtf8() + '\n'
                                                                       : m_csv.join(u'\n').toUtf8() + '\n';
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        {
            m_reason->setText(tr("Could not save “%1”: %2").arg(path, f.errorString()));
            m_reason->setVisible(true);
        }
    });
    dlg->open();
}

} // namespace qbucket
