# Devlog 002 — M0: verification on Linux

**Date:** 2026-10-20

## What was checked

The open points from devlog 001, on Ubuntu 26.04 (GCC 15.2, system Qt
6.10.2, Wayland session):

- **CI:** the first push ran all five jobs green (Windows MSVC, Linux GCC,
  ASan + UBSan, clang-tidy, Flatpak with the `kde-6.10` container). The
  container tag and the KDE runtime version in the manifest are therefore
  confirmed.
- **Local build:** Debug with `QBUCKET_WARNINGS_AS_ERRORS=ON` builds cleanly
  with zero warnings; all four test binaries pass. The Qt auto-detection
  falls through to the system Qt as intended.
- **Translations:** `lupdate` over `src/` finds 65 source texts, 0 new, and
  none are unfinished, so `qbucket_de.ts` is current.
- **`desktop-file-validate`:** one hint. `Network` and `Utility` are both
  main categories, so the app could appear twice in menus. Dropped
  `Utility`.
- **`appstreamcli validate --pedantic`:** passes. The only pedantic note
  is `cid-contains-uppercase-letter`. That one is accepted: the
  reverse-DNS ID with a capitalised app name is common on Flathub, and
  renaming the ID later is expensive.
- **Screenshots on Wayland:** the system-font fallback works (Noto Sans,
  the desktop font). Layout and tokens look as they do on Windows.
- **CI actions:** `actions/checkout` and `actions/upload-artifact` are
  now on v7, which runs on Node 24. That removes most of the Node 20
  deprecation warnings. `ilammy/msvc-dev-cmd` and
  `seanmiddleditch/gha-setup-ninja` have no Node 24 release yet, so they
  still warn.

## Bug found: tab-bar line ignores the theme

The screenshots showed a 2 px dark line between the "Details" title and
the tabs, and under the transfer dock title. It was `#393e43` in **both**
themes, and the Windows screenshots in devlog 001 show it too. With
`setDocumentMode(true)` the style paints the tab bar's base itself, in a
colour that comes from neither our palette nor the QSS.
`QTabWidget::pane { border-top: … }` has no effect in document mode.

Fix in the design system rather than per widget:
`QTabBar { qproperty-drawBase: 0; border-bottom: 1px solid @border@; }`.
The separator now sits under the tab row in the theme's border colour.
Regression test: `TestMainWindow::tabBarsUseThemedSeparator`. It fails
without the QSS change. xyz-profiler likely has the same line.

## Open

- **Start the Flatpak locally.** CI builds the bundle, but it has not been
  run on a desktop yet. `flatpak-builder` isn't installed on the dev box;
  the CI artifact `qbucket-x86_64.flatpak` can be installed with
  `flatpak install --user` (pulls `org.kde.Platform//6.10`).
- The devlog 001 screenshots still show the old line. Refresh them on the
  next screenshot pass.
