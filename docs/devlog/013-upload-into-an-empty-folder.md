# Devlog 013 — Uploading into an empty bucket

**Date:** 2026-10-26

## What was built

A fresh bucket could not take its first file: dropping files from the file
manager did nothing, and a right-click did nothing either. The reason was
the browser's `QStackedWidget`. With no rows, `ObjectBrowser::updatePage_`
swaps the list for the message page ("This folder is empty."), and every
drop and context-menu hook hung on the list's viewport only. The empty
folder — the one place where the first upload has to happen — had none.

- The message page accepts drops and goes through the same `handleDrop_`
  as the list (`onList = false`): no rows there, so files and objects land
  in the current folder. Upload rights, local-files-only and the object
  checks (`acceptsObjects_`) are unchanged.
- It has a context menu of its own: Paste, the location actions, Restore
  Deleted Objects, Refresh. Ctrl+V works there too (`m_actPaste` is also
  added to the page; a `WidgetShortcut` fires for every widget the action
  belongs to).
- The context menu had no upload entries at all, not even in a full list.
  `ObjectBrowser::setLocationActions` takes MainWindow's Upload Files…,
  Upload Folder… and New Folder… (MainWindow keeps enabling them through
  the gate, `updateMutationActions_`), shown in both menus, not in the
  versions view.

## Decisions and why

- **An error page is no folder.** The message page also shows listing
  errors ("Access Denied", "Try Again"). Drops and the menu are refused
  there: an error is never an empty folder (PLAN.md §12), and uploading
  into a place that could not be listed would be a guess.
- **The actions stay MainWindow's.** The browser only shows them; the gate
  check and the file dialogs stay where they were, so a drop still always
  shows the preview (`startUploadTo_(…, true)`).

## Tests

`test_object_browser`: `anEmptyFolderTakesDropsAndHasAContextMenu` (drops
refused without upload rights, files and objects land in the empty folder,
the menu offers the location actions and Paste, nothing in the versions
view or on an error) and `theListsContextMenuOffersTheLocationActions`. The
menus are read through `QApplication::activePopupWidget()` from a timer
while `QMenu::exec` runs — no hook in the browser needed.

## Open points

- No drop highlight on the message page (the list marks the folder row a
  drop goes into; here the target is always the current folder).
