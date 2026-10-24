# Submitting QBucket to Flathub

Prepared for 1.0.0, not submitted: Flathub builds only from public sources,
and this repository is private. Until then the Flatpak ships as a bundle
(`packaging/flatpak/build-in-docker.sh` → `qbucket-<version>-x86_64.flatpak`)
attached to the GitHub release.

## Before submitting

1. Make `github.com/theinvisible/qbucket` public. The app id
   `io.github.theinvisible.QBucket` is verified through that GitHub account.
2. Tag the release (`v1.0.0`) and put the tag's commit into
   `io.github.theinvisible.QBucket.yml` (`commit:`).
3. Run `packaging/flatpak/build-in-docker.sh` once more: with the repository
   public, `flatpak-builder-lint` must not report `appid-url-not-reachable`
   or `appstream-missing-screenshots` any more — the metainfo's screenshot
   URLs (`docs/img/*.png` at the tag) are then reachable. The third known
   error, `appstream-screenshots-not-mirrored-in-ostree`, only goes away on
   Flathub's own builders, which mirror the screenshots.

## Submitting

Follow <https://docs.flathub.org/docs/for-app-authors/submission>: fork
`flathub/flathub`, branch off `new-pr`, add this directory's manifest, open
the pull request against `new-pr`. Reviewers usually ask about:

- **finish-args.** `--share=network` (S3 endpoints), `--socket=wayland` +
  `fallback-x11`, `--device=dri`, `--share=ipc`, and
  `--filesystem=xdg-download` (the default download folder). Everything else
  goes through portals: the FileChooser for uploads, imports and other
  targets; OpenURI for "Open in the default application"; the Secret portal
  for the keychain — checked by the smoke test, no
  `--talk-name=org.freedesktop.secrets`. Dragging out stages copies in the
  app's own cache, which the host can read.
- **The tests in the build** (`run-tests: true`): headless, against
  in-process fake servers on loopback, no network.
- **The KDE runtime version**: keep it on a supported branch; a newer one
  needs a full run of `build-in-docker.sh`.

After the merge Flathub creates `flathub/io.github.theinvisible.QBucket`;
updates are pull requests there (new tag + commit).
