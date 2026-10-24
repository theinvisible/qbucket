// platform::checkKeychainService and QtKeychainBackend on a session bus of
// their own (a private dbus-daemon, never this machine's keychain): with no
// keychain service, the real QtKeychain fails — and the backend reports that
// as NoBackend (SecretStore's session-only mode), not as a failure that keeps
// an account from being saved. A Secret Service on the bus is found, running
// or activatable. The bus has a configuration of its own: the system's
// service directories would offer the machine's keyring for activation.

#include "accounts/QtKeychainBackend.h"
#include "platform/KeychainService.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace qbucket;

class TestKeychainService : public QObject {
    Q_OBJECT

private:
    QProcess m_bus;
    QTemporaryDir m_dir;

    QString servicesDir() const { return m_dir.filePath(QStringLiteral("services")); }

    static std::optional<bool> check()
    {
        std::optional<bool> exists;
        platform::checkKeychainService([&](bool e) { exists = e; });
        if (!QTest::qWaitFor([&] { return exists.has_value(); }, 10000))
            return std::nullopt;
        return exists;
    }

    template <typename Start>
    static std::optional<KeychainBackend::Status> await(Start start)
    {
        std::optional<KeychainBackend::Status> status;
        start([&](KeychainBackend::Status s) { status = s; });
        if (!QTest::qWaitFor([&] { return status.has_value(); }, 10000))
            return std::nullopt;
        return status;
    }

private slots:
    void initTestCase()
    {
#if !defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
        QSKIP("the keychain service check matters on Linux only");
#endif
        const QString daemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
        if (daemon.isEmpty())
            QSKIP("dbus-daemon not installed");
        QVERIFY(m_dir.isValid());
        QVERIFY(QDir().mkpath(servicesDir()));
        const QString config = m_dir.filePath(QStringLiteral("bus.conf"));
        QFile f(config);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QStringLiteral(
                    "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                    " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                    "<busconfig>\n"
                    "  <type>session</type>\n"
                    "  <listen>unix:dir=%1</listen>\n"
                    "  <auth>EXTERNAL</auth>\n"
                    "  <servicedir>%2</servicedir>\n"
                    "  <policy context=\"default\">\n"
                    "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                    "    <allow eavesdrop=\"true\"/>\n"
                    "    <allow own=\"*\"/>\n"
                    "  </policy>\n"
                    "</busconfig>\n")
                    .arg(m_dir.path(), servicesDir())
                    .toUtf8());
        f.close();
        m_bus.start(daemon, {QStringLiteral("--config-file=") + config, QStringLiteral("--nofork"),
                             QStringLiteral("--nopidfile"), QStringLiteral("--print-address=1")});
        QVERIFY(m_bus.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(m_bus.canReadLine(), 5000);
        const QByteArray address = m_bus.readLine().trimmed();
        QVERIFY(!address.isEmpty());
        // Before anything opens the session bus: this process sees only the
        // private one, and no desktop that would steer QtKeychain to KWallet.
        qputenv("DBUS_SESSION_BUS_ADDRESS", address);
        for (const char* var : {"XDG_CURRENT_DESKTOP", "DESKTOP_SESSION", "KDE_FULL_SESSION",
                                "GNOME_DESKTOP_SESSION_ID", "FLATPAK_ID"})
            qunsetenv(var);
        QVERIFY(QDBusConnection::sessionBus().isConnected());
    }

    void cleanupTestCase()
    {
        if (m_bus.state() != QProcess::NotRunning) {
            m_bus.kill();
            m_bus.waitForFinished();
        }
    }

    void noServiceOnTheBus()
    {
        const std::optional<bool> exists = check();
        QVERIFY(exists.has_value());
        QVERIFY(!*exists);
    }

    void aFailureWithoutAServiceIsNoBackend()
    {
        QtKeychainBackend backend(platform::checkKeychainService);
        const QString service = QStringLiteral("qbucket-test-keychain-service");
        std::optional<KeychainBackend::Status> s = await([&](auto done) {
            backend.write(service, QStringLiteral("k"), QStringLiteral("secret"),
                          [done](KeychainBackend::Status st, const QString&) { done(st); });
        });
        QVERIFY(s.has_value());
        QCOMPARE(int(*s), int(KeychainBackend::Status::NoBackend));
        s = await([&](auto done) {
            backend.read(service, QStringLiteral("k"),
                         [done](KeychainBackend::Status st, const QString&, const QString&) { done(st); });
        });
        QVERIFY(s.has_value());
        QCOMPARE(int(*s), int(KeychainBackend::Status::NoBackend));
    }

    void withoutACheckItStaysAFailure()
    {
        QtKeychainBackend backend;
        const std::optional<KeychainBackend::Status> s = await([&](auto done) {
            backend.write(QStringLiteral("qbucket-test-keychain-service"), QStringLiteral("k"), QStringLiteral("secret"),
                          [done](KeychainBackend::Status st, const QString&) { done(st); });
        });
        QVERIFY(s.has_value());
        QCOMPARE(int(*s), int(KeychainBackend::Status::Failed));
    }

    // From here on the bus has a (fake) Secret Service.
    void anActivatableServiceIsFound()
    {
        QFile f(servicesDir() + QStringLiteral("/org.freedesktop.secrets.service"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[D-BUS Service]\nName=org.freedesktop.secrets\nExec=/bin/false\n");
        f.close();
        const QDBusMessage reload = QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ReloadConfig")));
        QCOMPARE(reload.type(), QDBusMessage::ReplyMessage);
        const std::optional<bool> exists = check();
        QVERIFY(exists.has_value());
        QVERIFY(*exists);
        QVERIFY(f.remove());
        QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("ReloadConfig")));
    }

    void aRunningServiceIsFound()
    {
        QDBusConnection other = QDBusConnection::connectToBus(QString::fromLocal8Bit(qgetenv("DBUS_SESSION_BUS_ADDRESS")),
                                                              QStringLiteral("fake-secret-service"));
        QVERIFY(other.isConnected());
        QVERIFY(other.registerService(QStringLiteral("org.freedesktop.secrets")));
        const std::optional<bool> exists = check();
        QVERIFY(exists.has_value());
        QVERIFY(*exists);
        other.unregisterService(QStringLiteral("org.freedesktop.secrets"));
        QDBusConnection::disconnectFromBus(QStringLiteral("fake-secret-service"));
    }
};

QTEST_GUILESS_MAIN(TestKeychainService)
#include "test_keychain_service.moc"
