#pragma once

#include "ui/BucketConfigPage.h"

#include <QWidget>

#include <memory>

class QTreeWidget;

namespace qbucket {

class CodeEditor;

// The settings QBucket only shows: bucket tags, website, access logging,
// replication, default encryption, event notifications, ownership controls
// — each with whether it is set, and the document as the server sends it.
class OtherConfigPage : public QWidget {
    Q_OBJECT

public:
    OtherConfigPage(const BucketConfigPage::Context& context, QWidget* parent = nullptr);
    ~OtherConfigPage() override;

    void ensureLoaded();
    [[nodiscard]] QTreeWidget* list() const { return m_list; }
    [[nodiscard]] CodeEditor* viewer() const { return m_viewer; }
    [[nodiscard]] int pending() const { return m_pending; }

signals:
    void loaded();

protected:
    void showEvent(QShowEvent* event) override;

private:
    BucketConfigPage::Context m_context;
    std::unique_ptr<QObject> m_run;
    QTreeWidget* m_list = nullptr;
    CodeEditor* m_viewer = nullptr;
    int m_pending = 0;
    bool m_started = false;
};

} // namespace qbucket
