#include "controllers/SelfTest.h"

#include "accounts/KeychainBackend.h"

#include <QCoreApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QUuid>

namespace qbucket {
namespace {

const QString kKey = QStringLiteral("probe");

QString statusText(KeychainBackend::Status s, const QString& error)
{
    QString text;
    switch (s) {
    case KeychainBackend::Status::Ok:
        return QStringLiteral("ok");
    case KeychainBackend::Status::NotFound:
        text = QStringLiteral("not found");
        break;
    case KeychainBackend::Status::NoBackend:
        text = QStringLiteral("no backend; secrets would be kept for one session only");
        break;
    case KeychainBackend::Status::AccessDenied:
        text = QStringLiteral("access denied");
        break;
    case KeychainBackend::Status::Failed:
        text = QStringLiteral("failed");
        break;
    }
    return error.isEmpty() ? text : text + QStringLiteral(" (") + error + u')';
}

} // namespace

SelfTest::SelfTest(KeychainBackend* keychain, QTextStream* out, QObject* parent)
    : QObject(parent)
    , m_keychain(keychain)
    , m_out(out)
{
}

bool SelfTest::inFlatpak()
{
    return !qEnvironmentVariableIsEmpty("FLATPAK_ID") || QFile::exists(QStringLiteral("/.flatpak-info"));
}

void SelfTest::start()
{
    line_(QStringLiteral("QBucket"), QCoreApplication::applicationVersion());
    line_(QStringLiteral("Qt"), QStringLiteral("%1 (built with %2)")
                                    .arg(QString::fromLatin1(qVersion()), QStringLiteral(QT_VERSION_STR)));
    line_(QStringLiteral("System"), QSysInfo::prettyProductName() + QStringLiteral(", ")
                                        + QSysInfo::currentCpuArchitecture());
    line_(QStringLiteral("Sandbox"),
          inFlatpak() ? QStringLiteral("Flatpak (%1)").arg(qEnvironmentVariable("FLATPAK_ID", QStringLiteral("?")))
                      : QStringLiteral("none"));
    line_(QStringLiteral("Settings"), QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation));
    line_(QStringLiteral("Data"), QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    line_(QStringLiteral("Cache"), QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    line_(QStringLiteral("Qt plugins"), QLibraryInfo::path(QLibraryInfo::PluginsPath));
    m_out->flush();

    // A value of its own each run: a stale entry can't pass for a working
    // keychain.
    m_probe = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString service = QString::fromLatin1(kService);
    m_keychain->write(service, kKey, m_probe, [this, service](KeychainBackend::Status s, const QString& error) {
        if (s != KeychainBackend::Status::Ok) {
            finish_(false, QStringLiteral("write: ") + statusText(s, error));
            return;
        }
        m_keychain->read(service, kKey,
                         [this, service](KeychainBackend::Status rs, const QString& secret, const QString& rerror) {
            const bool same = rs == KeychainBackend::Status::Ok && secret == m_probe;
            const QString readResult = rs != KeychainBackend::Status::Ok
                                           ? QStringLiteral("read: ") + statusText(rs, rerror)
                                           : QStringLiteral("read: a different value came back");
            m_keychain->remove(service, kKey,
                               [this, same, readResult](KeychainBackend::Status ds, const QString& derror) {
                if (!same)
                    finish_(false, readResult);
                else if (ds != KeychainBackend::Status::Ok)
                    finish_(false, QStringLiteral("remove: ") + statusText(ds, derror));
                else
                    finish_(true, QStringLiteral("ok (written, read back, removed)"));
            });
        });
    });
}

void SelfTest::line_(const QString& label, const QString& value)
{
    *m_out << (label + u':').leftJustified(12) << value << '\n';
}

void SelfTest::finish_(bool keychainOk, const QString& result)
{
    line_(QStringLiteral("Keychain"), result);
    m_out->flush();
    emit finished(keychainOk ? 0 : 1);
}

} // namespace qbucket
