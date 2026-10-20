#include "ui/LogPanel.h"

#include "ui/Theme.h"

#include <QLocale>

namespace qbucket {

LogPanel::LogPanel(ActivityLog* log, QWidget* parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("logPanel"));
    setReadOnly(true);
    setMaximumBlockCount(kMaxLines);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setPlaceholderText(tr("No activity yet."));
    for (const ActivityLog::Entry& e : log->entries())
        append_(e);
    connect(log, &ActivityLog::added, this, &LogPanel::append_);
}

void LogPanel::append_(const ActivityLog::Entry& entry)
{
    const QString time = QLocale().toString(entry.at.toLocalTime().time(), QLocale::LongFormat);
    QString color;
    switch (entry.level) {
    case ActivityLog::Level::Info:
        break;
    case ActivityLog::Level::Warning:
        color = Theme::warningColor().name();
        break;
    case ActivityLog::Level::Error:
        color = Theme::dangerColor().name();
        break;
    }
    const QString text = entry.text.toHtmlEscaped();
    appendHtml(color.isEmpty()
                   ? QStringLiteral("<span>%1&nbsp;&nbsp;%2</span>").arg(time, text)
                   : QStringLiteral("<span style=\"color:%1\">%2&nbsp;&nbsp;%3</span>").arg(color, time, text));
}

} // namespace qbucket
