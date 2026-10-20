#pragma once

#include "controllers/ActivityLog.h"

#include <QPlainTextEdit>

namespace qbucket {

// The Log tab: the ActivityLog's entries, newest at the bottom, warnings
// and errors coloured. Read-only; keeps kMaxLines lines.
class LogPanel : public QPlainTextEdit {
    Q_OBJECT

public:
    static constexpr int kMaxLines = 5000;

    explicit LogPanel(ActivityLog* log, QWidget* parent = nullptr);

private:
    void append_(const ActivityLog::Entry& entry);
};

} // namespace qbucket
