#pragma once

#include <QObject>

class QTextStream;

namespace qbucket {

class KeychainBackend;

// `qbucket --self-test`: what a package needs to work, checked without a
// window, an account or the network — the version, the Qt it runs on, the
// sandbox, the paths, and a round trip through the keychain (a test entry of
// its own written, read back and removed). The smoke test of every package,
// and what a bug report asks for.
//
// Thread affinity: the thread it was created on; finished() comes from its
// event loop.
class SelfTest : public QObject {
    Q_OBJECT

public:
    static constexpr const char* kService = "qbucket-self-test";

    SelfTest(KeychainBackend* keychain, QTextStream* out, QObject* parent = nullptr);

    // Prints the environment, then starts the keychain round trip.
    void start();

    // Inside a Flatpak sandbox: FLATPAK_ID set, or /.flatpak-info present.
    static bool inFlatpak();

signals:
    // 0 when the keychain stored and returned the test entry, 1 otherwise.
    void finished(int exitCode);

private:
    void line_(const QString& label, const QString& value);
    void finish_(bool keychainOk, const QString& result);

    KeychainBackend* m_keychain;
    QTextStream* m_out;
    QString m_probe;
};

} // namespace qbucket
