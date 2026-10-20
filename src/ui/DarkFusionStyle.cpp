#include "ui/DarkFusionStyle.h"

#include "ui/Theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace qbucket {
namespace {

QPalette paletteFor(const Palette& p)
{
    QPalette pal;
    pal.setColor(QPalette::Window,          p.win);
    pal.setColor(QPalette::WindowText,      p.text);
    pal.setColor(QPalette::Base,            p.inset);
    pal.setColor(QPalette::AlternateBase,   p.panel2);
    pal.setColor(QPalette::ToolTipBase,     p.panel2);
    pal.setColor(QPalette::ToolTipText,     p.text);
    pal.setColor(QPalette::Text,            p.text);
    pal.setColor(QPalette::Button,          p.panel2);
    pal.setColor(QPalette::ButtonText,      p.text);
    pal.setColor(QPalette::BrightText,      Qt::white);
    pal.setColor(QPalette::Highlight,       p.accent);
    pal.setColor(QPalette::HighlightedText, p.accentFg);
    pal.setColor(QPalette::PlaceholderText, p.text3);
    pal.setColor(QPalette::Link,            p.accent);
    pal.setColor(QPalette::Mid,             p.border);
    pal.setColor(QPalette::Dark,            p.borderStrong);

    pal.setColor(QPalette::Disabled, QPalette::Text,       p.text3);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.text3);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.text3);
    pal.setColor(QPalette::Disabled, QPalette::Highlight,  p.panel3);
    return pal;
}

// The QSS template uses @token@ placeholders resolved from the Palette.
// Ported from xyz-profiler; the QBucket-specific additions are marked.
//
// Keep this scoped to real chrome. A catch-all `QWidget { … }` rule would
// match every widget in the application, which puts all of them on
// QStyleSheetStyle's rule-resolution path and makes each theme switch
// re-polish the entire tree against a matching rule. Text colours are
// already covered by QPalette::WindowText / Text / ButtonText.
const char* kTemplate = R"QSS(
QMainWindow { background: @win@; }
QDialog { background: @panel@; }
QToolTip { background: @panel2@; color: @text@; border: 1px solid @border@; padding: 4px 6px; }

/* ---- Menu bar ---- */
QMenuBar { background: @panel@; color: @text2@; border-bottom: 1px solid @border@; padding: 2px 4px; }
QMenuBar::item { background: transparent; padding: 5px 11px; margin: 2px 1px; border-radius: 6px; }
QMenuBar::item:selected { background: @hover@; color: @text@; }
QMenuBar::item:pressed { background: @accent@; color: @accentFg@; }
QMenu { background: @panel@; color: @text@; border: 1px solid @border@; padding: 5px; }
QMenu::item { padding: 6px 24px 6px 14px; border-radius: 6px; }
QMenu::item:selected { background: @accent@; color: @accentFg@; }
QMenu::item:disabled { color: @text3@; }
QMenu::separator { height: 1px; background: @border@; margin: 5px 8px; }

/* ---- Toolbar ---- */
QToolBar { background: @panel@; border: none; border-bottom: 1px solid @border@; spacing: 5px; padding: 2px 10px; }
QToolBar::separator { background: transparent; width: 6px; }

QToolButton { background: @panel2@; border: 1px solid @borderStrong@; border-radius: 6px; color: @text@; padding: 4px 11px; font-weight: 500; }
QToolButton:hover { background: @hover@; }
QToolButton:disabled { color: @text3@; }
QToolButton:checked { background: @accent@; color: @accentFg@; border-color: transparent; }
QToolButton::menu-indicator { image: none; }

QToolButton#tbPrimary { background: @accent@; color: @accentFg@; border: none; padding: 5px 13px; font-weight: 600; }
QToolButton#tbPrimary:hover { background: @accentHover@; }
QToolButton#tbPrimary:disabled { background: @panel3@; color: @text3@; }
QToolButton#tbIcon { padding: 4px; }
QToolButton#tbIcon:hover { background: @hover@; }

/* segmented controls (browse mode: Folders | Flat | Versions) */
QWidget#viewSeg { background: @panel2@; border: 1px solid @borderStrong@; border-radius: 6px; }
QToolButton#segBtn { background: transparent; border: none; border-radius: 5px; padding: 6px 9px; }
QToolButton#segBtn:hover { background: @hover@; }
QToolButton#segBtn:checked { background: @accent@; }
QToolButton#segText { background: transparent; border: none; border-radius: 5px; padding: 5px 14px; color: @text2@; font-weight: 500; }
QToolButton#segText:hover { background: @hover@; color: @text@; }
QToolButton#segText:checked { background: @accent@; color: @accentFg@; }
QToolButton#segText:disabled { color: @text3@; }

/* preferences / bucket-properties sidebar */
QWidget#prefSidebar { background: @panel2@; border: none; border-right: 1px solid @border@; }
QToolButton#prefCat { background: transparent; border: none; border-radius: 6px; padding: 9px 11px; color: @text2@; font-weight: 500; }
QToolButton#prefCat:hover { background: @hover@; color: @text@; }
QToolButton#prefCat:checked { background: @accent@; color: @accentFg@; }

/* ---- Inputs ---- */
QLineEdit { background: @inset@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 6px 10px; color: @text@; selection-background-color: @accent@; selection-color: @accentFg@; }
QLineEdit:focus { border-color: @accent@; }
QLineEdit:disabled { color: @text3@; }

QComboBox { background: @panel2@; border: 1px solid @borderStrong@; border-radius: 6px; padding: 6px 10px; color: @text@; }
QComboBox:hover { background: @hover@; }
QComboBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: @panel@; color: @text@; border: 1px solid @border@; selection-background-color: @accent@; selection-color: @accentFg@; outline: none; padding: 4px; }

QTextEdit, QPlainTextEdit { background: @inset@; border: 1px solid @borderStrong@; border-radius: 6px; color: @text@; selection-background-color: @accent@; selection-color: @accentFg@; }
QTextEdit:focus, QPlainTextEdit:focus { border-color: @accent@; }

/* ---- Push buttons ---- */
QPushButton { background: @panel2@; border: 1px solid @borderStrong@; border-radius: 6px; color: @text@; padding: 7px 16px; font-weight: 500; }
QPushButton:hover { background: @hover@; }
QPushButton:disabled { color: @text3@; }
QPushButton:default, QPushButton#primary { background: @accent@; color: @accentFg@; border-color: transparent; }
QPushButton:default:hover, QPushButton#primary:hover { background: @accentHover@; }
QPushButton#ghost { background: transparent; border-color: transparent; }
QPushButton#ghost:hover { background: @hover@; }

/* ---- Item views (sidebar tree, object list) ---- */
QTreeView, QListView, QTableView { background: @win@; border: none; color: @text@; outline: none; alternate-background-color: @win@; selection-background-color: @sel@; selection-color: @text@; }
QTableView::item { border: none; padding: 0 4px; }
QTableView::item:hover { background: @hover@; }
QTableView::item:selected { background: @sel@; color: @text@; }
QAbstractScrollArea { background: @win@; }
QTreeView::item { border: none; padding: 3px 0; }
QTreeView::item:hover { background: @hover@; }
QTreeView::item:selected { background: @sel@; color: @text@; }
QTreeView::branch:hover { background: @hover@; }
QTreeView::branch:selected { background: @sel@; }

QHeaderView { background: @panel2@; }
QHeaderView::section { background: @panel2@; color: @text2@; border: none; border-bottom: 1px solid @border@; padding: 7px 10px; font-weight: 600; }
QHeaderView::section:hover { color: @text@; }
QTableCornerButton::section { background: @panel2@; border: none; border-bottom: 1px solid @border@; }

/* ---- QBucket: sidebar, path bar, empty states ---- */
QWidget#sidebar { background: @panel2@; }
QWidget#sidebar QTreeView { background: @panel2@; }
QLabel#sectionHeader { color: @text3@; font-weight: 600; padding: 10px 12px 4px 12px; }
QWidget#pathBar { background: @panel@; border-bottom: 1px solid @border@; }
QLabel#pathText { color: @text2@; padding: 7px 12px; }
QToolButton#crumb, QToolButton#crumbCurrent { background: transparent; border: none; border-radius: 5px; padding: 3px 6px; color: @text2@; }
QToolButton#crumb:hover, QToolButton#crumbCurrent:hover { background: @hover@; color: @text@; }
QToolButton#crumbCurrent { color: @text@; font-weight: 600; }
QToolButton#crumb::menu-indicator { image: none; width: 0; }
QLabel#crumbSep { color: @text3@; padding: 0 1px; }
QLabel#crumbAccount { color: @text2@; padding: 0 2px 0 6px; }
QLabel#envBadge { border-radius: 4px; padding: 0 5px; font-weight: 700; font-size: 8pt; }
QLineEdit#pathEdit { border-radius: 5px; padding: 3px 8px; margin: 2px 6px; }
QLabel#bannerInfo, QLabel#bannerError { background: @panel2@; border-bottom: 1px solid @border@; padding: 6px 12px; }
QLabel#bannerInfo { color: @text2@; }
QLabel#bannerError { color: @danger@; }
QLabel#emptyState { color: @text3@; padding: 24px; }
QLabel#paneTitle { color: @text@; font-weight: 600; padding: 12px 14px 6px 14px; }
QLabel#fieldLabel { color: @text3@; padding-top: 6px; }

/* ---- Tabs (detail pane, transfer panel) ---- */
QTabWidget::pane { border: none; border-top: 1px solid @border@; background: @panel@; }
/* The tab bar base (documentMode) is painted by the style in a fixed colour
   that ignores the palette; draw the separator as a themed border instead. */
QTabBar { background: @panel@; qproperty-drawBase: 0; border-bottom: 1px solid @border@; }
QTabBar::tab { background: transparent; color: @text2@; padding: 9px 13px; margin-right: 2px; border: none; border-bottom: 2px solid transparent; font-weight: 500; }
QTabBar::tab:hover { color: @text@; }
QTabBar::tab:selected { color: @accent@; border-bottom-color: @accent@; }

/* ---- QBucket: transfer dock ---- */
QDockWidget { color: @text2@; }
QDockWidget::title { background: @panel2@; border-top: 1px solid @border@; border-bottom: 1px solid @border@; padding: 6px 12px; text-align: left; }

/* ---- Group box / check ---- */
QGroupBox { border: 1px solid @border@; border-radius: 9px; margin-top: 16px; padding: 16px 14px 14px; background: @panel2@; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 12px; padding: 0 4px; color: @text2@; }
QCheckBox, QRadioButton { color: @text@; spacing: 7px; }

/* ---- Splitter ---- */
QSplitter::handle { background: @border@; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: @selLine@; }

/* ---- Scrollbars ---- */
QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar::handle:vertical { background: @borderStrong@; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: @text3@; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle:horizontal { background: @borderStrong@; border-radius: 4px; min-width: 30px; margin: 2px; }
QScrollBar::handle:horizontal:hover { background: @text3@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ---- Status bar ---- */
QStatusBar { background: @panel2@; border-top: 1px solid @border@; color: @text2@; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @text2@; padding: 0 6px; }

/* ---- Detail pane ---- */
QWidget#detailPane, QWidget#detailInner { background: @panel@; }
QWidget#detailPane QTabBar::tab { padding: 8px 9px; }

/* ---- Misc ---- */
QScrollArea { border: none; background: @panel@; }
QProgressBar { border: 1px solid @border@; border-radius: 6px; background: @inset@; text-align: center; color: @text@; }
QProgressBar::chunk { background: @accent@; border-radius: 5px; }
)QSS";

QString resolve(const Palette& p)
{
    QString s = QString::fromUtf8(kTemplate);
    const auto rep = [&s](const char* token, const QColor& c) {
        s.replace(QLatin1String(token), c.name(QColor::HexRgb));
    };
    rep("@accent@",       p.accent);
    rep("@accentFg@",     p.accentFg);
    rep("@accentHover@",  p.accentHover);
    rep("@bg@",           p.bg);
    rep("@win@",          p.win);
    rep("@panel@",        p.panel);
    rep("@panel2@",       p.panel2);
    rep("@panel3@",       p.panel3);
    rep("@border@",       p.border);
    rep("@borderStrong@", p.borderStrong);
    rep("@text@",         p.text);
    rep("@text2@",        p.text2);
    rep("@text3@",        p.text3);
    rep("@hover@",        p.hover);
    rep("@sel@",          p.sel);
    rep("@selLine@",      p.selLine);
    rep("@titlebar@",     p.titlebar);
    rep("@inset@",        p.inset);
    rep("@danger@",       Theme::dangerColor());   // semantic, same in both themes
    return s;
}

QFont applicationFont()
{
#ifdef Q_OS_WIN
    // The design font. Only on Windows — elsewhere the desktop's own UI font
    // is the right choice (and Segoe would only fall back anyway).
    QFont font(QStringLiteral("Segoe UI Variable Display"));
    if (!QFontDatabase::families().contains(font.family()))
        font.setFamily(QStringLiteral("Segoe UI"));
    font.setPointSize(10);
    return font;
#else
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#endif
}

} // namespace

QPalette DarkFusionStyle::lightPalette() { return paletteFor(Theme::palette(false)); }
QPalette DarkFusionStyle::darkPalette()  { return paletteFor(Theme::palette(true)); }

QString DarkFusionStyle::styleSheet(bool dark)
{
    return resolve(Theme::palette(dark));
}

bool DarkFusionStyle::resolvesToDark(const QString& themeName)
{
    if (themeName == QLatin1String("Light"))
        return false;
    if (themeName == QLatin1String("System"))
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    return true;
}

bool DarkFusionStyle::applyTheme(const QString& themeName)
{
    const bool dark = resolvesToDark(themeName);

    // The Fusion base style only needs installing once. Recreating it on every
    // toggle would re-polish the entire widget tree for nothing — a major part
    // of the switch latency.
    //
    // Don't detect "already Fusion" via QApplication::style()->name(): once a
    // stylesheet is set, style() returns Qt's internal QStyleSheetStyle proxy,
    // whose name() is empty — the check would fail on every call and reinstall
    // Fusion each time. Remember it ourselves instead.
    static bool fusionInstalled = false;
    if (!fusionInstalled) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        fusionInstalled = true;
    }

    QApplication::setPalette(dark ? darkPalette() : lightPalette());

    // Font is theme-independent: set it only if it isn't ours yet, otherwise we
    // pay for a full re-polish + relayout on every theme switch.
    const QFont font = applicationFont();
    if (QApplication::font() != font)
        QApplication::setFont(font);

    Theme::setDark(dark);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
        app->setStyleSheet(styleSheet(dark));
    return dark;
}

} // namespace qbucket
