#include "platform/KeychainService.h"

#include <QTimer>
#include <QtGlobal>

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#define QBUCKET_KEYCHAIN_DBUS 1
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QStringList>
#endif

namespace qbucket::platform {

#ifdef QBUCKET_KEYCHAIN_DBUS
namespace {

constexpr int kTimeoutMs = 3000;

const QStringList& serviceNames()
{
    static const QStringList names = {
        QStringLiteral("org.freedesktop.secrets"),
        QStringLiteral("org.kde.kwalletd6"),
        QStringLiteral("org.kde.kwalletd5"),
    };
    return names;
}

bool inFlatpak()
{
    return !qEnvironmentVariableIsEmpty("FLATPAK_ID") || QFile::exists(QStringLiteral("/.flatpak-info"));
}

// One asynchronous call; `handle` gets the reply (an error reply on failure
// or timeout).
void call(const QDBusConnection& bus, const QDBusMessage& message,
          std::function<void(const QDBusMessage&)> handle)
{
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, kTimeoutMs));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                     [handle = std::move(handle)](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        handle(w->reply());
    });
}

QDBusMessage busMethod(const QString& method)
{
    return QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                                          QStringLiteral("org.freedesktop.DBus"), method);
}

bool namesAKeychain(const QDBusMessage& reply)
{
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return false;
    const QStringList names = reply.arguments().constFirst().toStringList();
    for (const QString& n : serviceNames()) {
        if (names.contains(n))
            return true;
    }
    return false;
}

} // namespace
#endif

void checkKeychainService(std::function<void(bool exists)> done)
{
#ifdef QBUCKET_KEYCHAIN_DBUS
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        QTimer::singleShot(0, [done = std::move(done)] { done(false); });
        return;
    }
    // Running, then activatable, then (sandboxed) the Secret portal.
    auto portal = [bus, done](bool found) {
        if (found || !inFlatpak()) {
            done(found);
            return;
        }
        const QDBusMessage introspect = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.DBus.Introspectable"), QStringLiteral("Introspect"));
        call(bus, introspect, [done](const QDBusMessage& reply) {
            done(reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
                 && reply.arguments().constFirst().toString().contains(
                     QStringLiteral("org.freedesktop.portal.Secret")));
        });
    };
    call(bus, busMethod(QStringLiteral("ListNames")), [bus, portal](const QDBusMessage& running) {
        if (namesAKeychain(running)) {
            portal(true);
            return;
        }
        call(bus, busMethod(QStringLiteral("ListActivatableNames")), [portal](const QDBusMessage& activatable) {
            portal(namesAKeychain(activatable));
        });
    });
#else
    QTimer::singleShot(0, [done = std::move(done)] { done(true); });
#endif
}

} // namespace qbucket::platform
