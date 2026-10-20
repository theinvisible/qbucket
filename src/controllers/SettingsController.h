#pragma once

#include <QObject>
#include <QString>

class QSettings;
class QVariant;

namespace qbucket {

// QSettings-backed user preferences persisted as INI under AppConfigLocation.
// Every setter normalises its input, writes immediately and emits the
// matching signal only when the value actually changed.
//
// Writes are synchronous disk I/O (QSettings::sync()). Never call a setter
// from a per-mouse-event path — debounce high-frequency state (splitters,
// column widths) first, see MainWindow.
//
// Secrets never go through here: passwords and secret keys belong in the
// keychain (accounts/SecretStore, M1).
class SettingsController : public QObject {
    Q_OBJECT

public:
    explicit SettingsController(QObject* parent = nullptr);
    ~SettingsController() override;

    // "Dark" | "Light" | "System"
    QString themeName()   const { return m_themeName; }
    // "folders" | "flat" | "versions"
    QString browseMode()  const { return m_browseMode; }

    // Base64-encoded QMainWindow / QSplitter state blobs. Persisted only —
    // they have no live listeners, so their setters emit nothing.
    QString windowGeometry()    const { return m_windowGeometry; }
    QString windowState()       const { return m_windowState; }
    QString mainSplitterState() const { return m_mainSplitterState; }

    // Transfers (M3). Where downloads go unless the user picks a folder;
    // empty in the INI: the system's download folder.
    QString downloadDirectory() const { return m_downloadDirectory; }
    // Parts in flight per account at most (the adaptive limit's ceiling).
    int transferParallelParts() const { return m_parallelParts; }
    // Transfer threads per account (each with its own connections); for
    // sessions created after a change.
    int transferThreads() const { return m_transferThreads; }
    static constexpr int kMinParallelParts = 1;
    static constexpr int kMaxParallelParts = 64;
    static constexpr int kMinTransferThreads = 1;
    static constexpr int kMaxTransferThreads = 16;

    void setThemeName(const QString& name);
    void setBrowseMode(const QString& mode);
    void setDownloadDirectory(const QString& dir);
    void setTransferParallelParts(int parts);
    void setTransferThreads(int threads);
    void setWindowGeometry(const QString& base64);
    void setWindowState(const QString& base64);
    void setMainSplitterState(const QString& base64);

    static QString normalizedThemeName(const QString& name);
    static QString normalizedBrowseMode(const QString& mode);

    // Path of the INI file backing this controller (diagnostics, tests).
    QString fileName() const;

signals:
    void themeNameChanged();
    void browseModeChanged();
    void transferSettingsChanged();

private:
    void load_();
    void write_(const QString& key, const QVariant& value);

    QSettings* m_store;
    QString    m_themeName;
    QString    m_browseMode;
    QString    m_windowGeometry;
    QString    m_windowState;
    QString    m_mainSplitterState;
    QString    m_downloadDirectory;
    int        m_parallelParts = 16;
    int        m_transferThreads = 4;
};

} // namespace qbucket
