# Changelog — QBucket

Changes to the QBucket application, newest first. The S3 library has its own
version and changelog: [`libs/qbucket-s3/CHANGELOG.md`](libs/qbucket-s3/CHANGELOG.md).
The version here is the one in `CMakeLists.txt`; `tools/check-version.sh`
keeps the AppStream metainfo and the Debian changelog in step.

## [1.0.0] — 2026-10-30

The first release: parity with the essentials of NetSDK's S3 Browser, on
Windows and Linux. Built on `qbucket-s3` 0.1.0. How each feature came about:
[docs/devlog/](docs/devlog/).

### Accounts and servers

- Accounts for AWS S3, Hetzner Object Storage, Ceph RGW, Garage and any
  S3-compatible endpoint; provider presets with their quirks built in.
- Secrets only in the system keychain (Windows Credential Manager, Secret
  Service / KWallet, the Flatpak Secret portal); without one, asked once per
  session and kept in memory — never written to a file.
- Production / staging / development environments; production accounts start
  read-only.
- Import from the AWS CLI's `credentials` / `config` and from rclone; a
  Hetzner endpoint becomes the Hetzner profile.
- The server's capabilities probed read-only and kept fresh; an unsupported
  feature can't be triggered, and the reason is shown.

### Browsing

- Buckets and folders of any size: paged listings, own sorting and filtering,
  no stalls with a million objects.
- Object details: headers, metadata, tags, access, versions, object lock.
- Bookmarks and recent locations; back / forward / up; the sidebar follows.
- Presigned links; folder sizes.

### Changing things — every change planned, previewed, verified and journaled

- New folder, delete, rename; copy and move, also between buckets (clipboard
  and drag & drop); a move deletes the source only after the copy is verified.
- Headers, metadata, storage class, tags and canned ACLs, merged into the
  current state.
- Versions: browse, download, restore, delete for good, undelete folders;
  switch a bucket's versioning.
- Bucket policy, CORS, lifecycle rules, ACL, public access block, object lock
  (default retention, retention and legal hold per object, the governance
  bypass per operation); create and delete buckets.
- A running operation shows its progress and what it is doing in the status
  bar; deleting a bucket names each stage and ends with a message.

### Transfers

- Uploads and downloads of any size: multipart, verified by MD5, resumable
  after a quit or a crash; a transfer panel; adaptive concurrency per account.
- Drag & drop in from the file manager; drag out into Explorer (virtual files)
  or a Linux file manager (staged copies, also from the Flatpak).
- An empty bucket or folder takes files too: drop them onto it, or upload,
  create a folder or paste from its context menu.
- Open an object in its default application and upload the changes, keeping
  its headers, tags and ACL; a change on the server meanwhile is a conflict,
  never overwritten silently.

### Packages

- Linux: a Flatpak bundle (KDE runtime 6.10) and a `.deb` for Ubuntu 26.04.
- Windows: an installer and a ZIP (x64), with the MSVC runtime inside — no
  Visual C++ Redistributable and no admin rights needed (the installer
  installs per user unless "for all users" is chosen).
- `qbucket --self-test` checks an installation (Qt, sandbox, paths, a keychain
  round trip) without a window; `--version`, `--help`.

### Known limitations

- If-Match on writes is unusable on Hetzner, Ceph RGW and Garage: replacing an
  object there checks its ETag with HEAD right before the write, which leaves
  a small window.
- A Windows drag lists at most 10 000 objects; nothing is dragged out of the
  versions view.
- Encrypted rclone configurations and AWS SSO / role / session-token profiles
  are not imported (listed with the reason).
- No temporary credentials (STS / SSO), no Glacier restore, no folder sync yet
  (PLAN.md, post-parity backlog).
