# QBucket — implementation plan

QBucket is an open-source, native desktop browser for S3 and S3-compatible
object storage (AWS, Hetzner Object Storage, Ceph RGW, MinIO, Cloudflare R2,
Backblaze B2, Wasabi, …). The feature yardstick is NetSDK's **S3 Browser**
(Windows-only, freeware/paid); QBucket aims to cover what makes that tool
useful day to day, on **Windows and Linux**, under the **MIT license**.

**Priorities:** Hetzner Object Storage is the first-class provider — it is
what gets tested first and most. Linux ships first as a **Flatpak**. Every
provider-specific behaviour lives behind a provider profile (§3.1), so
quirks for Ceph, MinIO, R2, B2, AWS … can be added later without touching
the protocol core.

**Ground rule:** QBucket almost always works on **production data**. It is
designed to be robust first — check once too often rather than once too
little — and fast on very large buckets second. See §12 (data safety) and
§13 (performance); both apply to every milestone.

Look and feel follow **xyz-profiler** (`H:\DEV\CLion\xyz-profiler`): Qt
Widgets on Fusion, the same `Theme` palette tokens and QSS template, the same
line-icon set, Dark / Light / System theme switching.

Effort tags as in xyz-profiler's `PLAN.md`: **S** ≈ a focused session,
**M** ≈ a dialog or a new view, **L** ≈ touches several subsystems.
Overall estimate to S3-Browser parity: **~11 weeks full-time** (including
the robustness work from §12).

---

## 1. Decisions (fixed)

| Topic | Decision | Why |
|---|---|---|
| Language / UI | C++20, Qt 6.5+ Widgets, CMake ≥ 3.22 | Same stack as xyz-profiler; tables, trees, forms, D&D are built in |
| S3 client | **Own client on `QNetworkAccessManager`** — SigV4 signer + `QXmlStreamReader` parsers | Async Qt-native networking that runs on worker threads (one QNAM per thread), no aws-sdk-cpp build pain, full control over provider quirks |
| TLS | Qt's default backend (Schannel on Windows, OpenSSL on Linux) | Nothing extra to ship on Windows |
| Secrets | **All passwords / secret keys via QtKeychain** (installed package if built for our Qt, else FetchContent; Flatpak module) → Windows Credential Manager, Secret Service / KWallet on Linux. Wrapped in `SecretStore`; **no plaintext fallback** | Never store secrets in the INI, a JSON file or the resume store |
| Settings | `QSettings` INI via a `SettingsController` (pattern from xyz-profiler) | Proven, including the debounce lesson |
| Theme | Port `Theme`, `DarkFusionStyle`, `IconFactory` from xyz-profiler, namespace `qbucket` | One design language across the user's apps |
| Namespace / naming | `namespace qbucket`, **no `Q` class prefix** (that looks like Qt API) | Mirrors xyz-profiler's `xyz::` |
| Binary | target `qbucket`, display name **QBucket** | |
| License | **MIT** (QtKeychain is BSD-3, compatible; Qt itself used under LGPL-3.0 via dynamic linking) | Lets others reuse the `qbucket-s3` library freely |
| S3 library | **`libs/qbucket-s3`: standalone, reusable library** (Qt Core/Network only), app consumes its public API only (§3.0) | Usable in other projects; hard boundary keeps the core clean |
| Provider priority | **Hetzner first**; others via provider profiles | The user's main storage; Ceph-based, so most Ceph quirks come along |
| Linux packaging | **Flatpak first** (KDE runtime), AppImage optional later | Flathub reach, sandboxed, Qt from the runtime |
| i18n | English source strings, German `.ts` from day one | Same as xyz-profiler |
| Tests | QtTest, one binary per module, ctest | Same as xyz-profiler |

## 2. Scope against S3 Browser

**In scope (parity target)**

- Accounts: multiple, any endpoint, region, path-style vs. virtual-hosted,
  provider presets, "external bucket" for keys without `ListBuckets`
  permission, import from `~/.aws/credentials` and rclone config
- Browse: buckets, prefix navigation, breadcrumb, back/forward/up, flat
  (recursive) view, filter, very large prefixes (100k+ objects) without stalls
- Transfers: upload/download files and folders, multipart with resume,
  parallelism, queue with pause/cancel/retry, D&D in from the file manager,
  D&D out to the file manager
- Object ops: copy/move (server-side, also across buckets and > 5 GB),
  rename, delete (batched), create folder, edit HTTP headers and user
  metadata, tags, storage class, canned ACLs + grant view, presigned URLs,
  folder size calculation
- Versioning: enable/suspend, show versions, download/restore an old version,
  delete markers, permanent delete
- Bucket management: create/delete, policy (JSON editor), CORS, lifecycle,
  default encryption, object lock + retention/legal hold, tagging, logging,
  static website, replication (raw editor), incomplete multipart uploads
  (list + abort)

**Later / AWS-only (post-parity)**

- Folder compare / sync, bandwidth limiting, multiple browser tabs
- STS AssumeRole, AWS SSO profiles, Glacier restore, Transfer Acceleration,
  requester pays, CloudFront, inventory, event notifications

**Out of scope**: IAM user management, S3 Select, mounting as a drive.

## 3. Architecture

The repository holds two things with a hard boundary between them:

1. **`libs/qbucket-s3/`** — a **reusable S3 library** (Qt6 Core + Network
   only, MIT). Everything that speaks S3: signing, client, provider
   profiles, threading proxy, transfer engine. Usable from other projects
   without anything from the app. See §3.0.
2. **`src/`** — the **QBucket application**: accounts + keychain, safety
   layer (OperationController, journal), models, widgets.

```
qbucket/
├── CMakeLists.txt            Qt auto-detect (copied from xyz-profiler), add_subdirectory(libs/qbucket-s3),
│                             QtKeychain (app only: cmake/QBucketQtKeychain.cmake)
├── libs/qbucket-s3/          reusable library — layout in §3.0
├── src/                      the application (consumes the library's public API only)
│   ├── domain/               App-level header-only structs (qbucket_domain INTERFACE)
│   │   ├── Account.h         id, name, providerId, endpoint, region, addressing, environment
│   │   │                     marker, read-only flag, keychain ref (never the secret itself)
│   │   └── TransferJob.h     UI-facing job state (wraps library transfer progress)
│   ├── accounts/
│   │   ├── AccountStore      non-secret account fields in the INI (endpoint, region, key id, …)
│   │   ├── SecretStore       async wrapper over QtKeychain Read/Write/DeletePasswordJob;
│   │   │                     service "QBucket", key = account UUID + secret kind
│   │   │                     (secretAccessKey, sessionToken, proxyPassword)
│   │   └── KeychainCredentialsProvider   implements s3::CredentialsProvider on top of SecretStore
│   ├── models/
│   │   ├── AccountTreeModel  accounts → buckets (lazy) for the sidebar
│   │   ├── ObjectTableModel  one prefix; canFetchMore/fetchMore pages ListObjectsV2
│   │   ├── ObjectSortProxyModel  folders first, natural sort, filter
│   │   ├── VersionTableModel ListObjectVersions, same paging pattern
│   │   └── TransferModel     rows of TransferQueue jobs
│   ├── controllers/
│   │   ├── SessionController     one s3::Session per account, feature probing cache
│   │   ├── OperationController   the mutation gate (§12): plan/preview/execute/verify/report
│   │   ├── Journal               JSONL audit log; implements s3::RequestObserver
│   │   ├── BrowserController     location (account/bucket/prefix), history, view mode
│   │   ├── TransferController    UI-facing facade over TransferQueue
│   │   └── SettingsController    QSettings INI (ported, keys renamed)
│   ├── platform/win/         VirtualFileDrag (IDataObject with FILEDESCRIPTORW/FILECONTENTS)
│   └── ui/
│       ├── Theme, DarkFusionStyle, IconFactory   ported from xyz-profiler
│       ├── MainWindow           toolbar, sidebar | object view | detail pane, transfer dock
│       ├── PathBar              clickable breadcrumb + editable path on click
│       ├── ObjectRowDelegate    icon, name, size, badges (storage class, version, lock)
│       ├── DetailPane           tabs: Properties / Headers & Meta / Tags / Versions / Access
│       ├── TransferPanel        progress rows, speed, ETA, pause/cancel/retry
│       ├── AccountDialog        preset picker, endpoint test, credentials
│       ├── BucketPropertiesDialog  prefSidebar-style category list (see §5)
│       ├── JsonEditor / XmlEditor  QPlainTextEdit + QSyntaxHighlighter + validation
│       ├── PresignDialog        expiry, method, copy URL
│       └── SettingsDialog       General / Transfers / Network / Appearance
└── tests/                    app tests: test_object_model, test_operation_controller,
                              test_journal, test_account_store (library tests live in the library)
```

Dependency direction: **`qbucket-s3` (library) ← app** `domain` ←
`accounts` ← `controllers`/`models` ← `ui`. The library never includes
anything from `src/`; the app includes only the library's public headers.

### 3.0 The reusable S3 library (`qbucket-s3`)

Goal: another Qt project can drop the library in via `add_subdirectory`,
`FetchContent` or `find_package(QBucketS3)` and get a robust, threaded,
provider-aware S3 client — without QBucket's UI, settings or keychain.

```
libs/qbucket-s3/
├── CMakeLists.txt            standalone-capable: project(qbucket-s3 VERSION x.y.z), builds on its own
├── README.md                 usage, threading rules, provider profiles, versioning policy
├── CHANGELOG.md              semver, independent of the app version
├── cmake/QBucketS3Config.cmake.in   install(EXPORT) → find_package(QBucketS3)
├── include/qbucket/s3/       PUBLIC headers (the API contract)
│   ├── Result.h              Result<T> (expected-style, no exceptions) + Error
│   ├── Types.h               BucketInfo, ObjectEntry, ObjectMeta, ListPage, VersionEntry,
│   │                         MultipartUploadInfo, bucket config structs (Cors, Lifecycle, …)
│   ├── Endpoint.h            URL, region, addressing style, TLS options (pinned certificate)
│   ├── Credentials.h         CredentialsProvider interface + StaticCredentials,
│   │                         EnvCredentials, AwsSharedFileCredentials
│   ├── (S3Client)            private in M1 (src/client/): async, one thread, own QNAM —
│   │                         Session is the only public entry point
│   ├── Session.h             thread-safe proxy: worker threads, RequestId, value-type results
│   ├── Presigner.h           presigned URLs without a network round-trip
│   ├── RequestObserver.h     hook: every request/response (redacted) → e.g. an app's journal
│   ├── Capabilities.h        Feature enum, CapabilityState, CapabilitySet (server + bucket scope)
│   ├── CapabilityProber.h    read-only probes, optional deep probe, response interpretation
│   ├── Logging.h             Q_LOGGING_CATEGORY qbucket.s3.* (redaction built in)
│   ├── providers/            ProviderProfile, ProviderRegistry, Capabilities,
│   │                         GenericS3Profile, HetznerProfile, AwsProfile, …
│   └── transfer/             Uploader, Downloader, Copier, PartPlanner, TransferQueue,
│                             ResumeStore (interface) + JsonFileResumeStore
├── src/                      PRIVATE: SigV4, Uri, XmlParsers, XmlWriters, RetryPolicy,
│                             AdaptiveConcurrency, FileRangeDevice, profile implementations
├── tests/                    all protocol/transfer/provider tests + fixtures per provider
│   └── support/FakeS3Server  fault-injecting test server (exported as qbucket::s3_testsupport)
└── examples/s3ls/            tiny CLI over the public API — documentation that compiles
```

**CMake targets**

| Target | Contents | Depends on |
|---|---|---|
| `qbucket::s3` | types, SigV4, client, session, presigner, providers | Qt6::Core, Qt6::Network |
| `qbucket::s3_transfer` | multipart upload/download/copy, queue, resume | `qbucket::s3`, Qt6::Concurrent |
| `qbucket::s3_testsupport` | `FakeS3Server` for consumers' own tests | `qbucket::s3`, Qt6::Network |

**Rules that keep it reusable**

- **Dependencies:** Qt6 Core / Network / Concurrent only. No Widgets, no
  QtKeychain, no `QSettings`, no app types, no hard-coded paths.
- **Public vs. private:** only `include/qbucket/s3/**` is API. Public
  `QObject` classes use **PIMPL** so QNAM/parser details stay private and
  the ABI can evolve. Export macro via `generate_export_header`; static by
  default, shared on request (`QBUCKET_S3_BUILD_SHARED`).
- **Namespace** `qbucket::s3` (transfer: `qbucket::s3::transfer`).
- **Inversion points instead of app knowledge:**
  - credentials → `CredentialsProvider` (QBucket plugs in its keychain-backed one),
  - auditing → `RequestObserver` (QBucket's journal),
  - resume persistence → `ResumeStore` interface (QBucket: JSON under AppLocalData),
  - logging → Qt logging categories, consumers filter as they like.
- **Safety lives in the library where it is protocol-level** (Content-MD5,
  200-with-error parsing, idempotent-only retries, listing loop detection,
  redaction, integrity verification) — so every consumer gets it. **Policy
  lives in the app** (read-only accounts, confirmations, plan/preview,
  environment markers).
- **Thread-safety documented per class** in the headers: `S3Client` is
  thread-affine; `Session`, `Presigner`, `ProviderRegistry` are thread-safe.
- **Versioning:** own semver + CHANGELOG; breaking API changes bump the
  major version. Pre-1.0 while QBucket is pre-parity.
- **Standalone CI job** configures and builds `libs/qbucket-s3` on its own
  (no app, no QtKeychain) and runs its tests + the example — that is what
  enforces the boundary.
- Kept in this repo (monorepo) for now; can be split into its own
  repository later with `git subtree split` without changes.

### 3.1 Provider profiles (quirk isolation)

Provider profiles are part of the library (`include/qbucket/s3/providers/`),
so every consumer gets the quirk handling.

Rule: **the protocol core never checks which provider it talks to.** No
`if (endpoint.contains("hetzner"))` anywhere in the library's client /
transfer code or in the app.
Every deviation from plain S3 is a profile hook or a capability flag.

`ProviderProfile` (abstract; `GenericS3Profile` implements the S3 defaults,
concrete profiles override only what differs):

| Group | Members |
|---|---|
| Metadata | `id()`, `displayName()`, `regions()`, `endpointFor(region)`, `defaultAddressing()`, `signingRegion(region)` |
| Capabilities | `capabilities()` → `Capabilities` flags; `storageClasses()`; `cannedAcls()` |
| Limits | `minPartSize()`, `maxParts()`, `maxPartSize()`, `maxPresignExpiry()`, `maxDeleteBatch()`, `maxKeysPerList()` |
| Request quirks | `prepareRequest(S3RequestContext&)` — add/strip headers, force path-style for certain buckets, choose `UNSIGNED-PAYLOAD` vs. signed hash, `Content-MD5` policy |
| Response quirks | `normalizeListing(ListResult&)` (missing `Owner`, ETag quoting, timestamp formats), `mapError(S3Error&)` (provider-specific codes → our categories, `NotImplemented` detection, retryability) |
| Fallbacks | `listApi()` (V2 or V1), `supportsBatchDelete()`, copy-in-place metadata behaviour |

How the pieces fit:

- The app's `Account` stores a `providerId`; `SessionController` resolves
  it through `ProviderRegistry` and hands the profile to the `Session`. The client calls
  the hooks at fixed points (before signing, after parsing, on error).
- **Static capabilities** from the profile are only the fallback; the
  **dynamic capability safeguard** (§3.3) probes the server on connect and
  its definitive results win.
- A user-entered custom endpoint gets `GenericS3Profile`, plus per-account
  overrides (addressing style, region, "disable feature X") stored on the
  account — the escape hatch for providers without a profile.
- Each profile has its own test file (`test_provider_hetzner.cpp`, …) with
  fixtures captured from that provider under `tests/data/<provider>/`.
- Adding a provider = one new profile class + its fixtures + one line in
  `ProviderRegistry`. Nothing else changes.

### 3.2 Hetzner profile (first-class)

- Endpoints `https://{region}.your-objectstorage.com`, regions `fsn1`,
  `nbg1`, `hel1` (verify the current list in M1 against Hetzner's docs).
- Ceph RGW underneath — expect RGW behaviour for listing, errors and
  multipart.
- Capability matrix (versioning, object lock, lifecycle, CORS, policy, ACL,
  website, replication, encryption, storage classes) is **established in M1**
  from Hetzner's documentation and confirmed by probing a real project; the
  result is checked in as `docs/providers/hetzner.md` and encoded in
  `HetznerProfile::capabilities()`.
- Hetzner is the default integration-test target (§7) and the default
  preset in the account dialog.

**Preliminary findings** from Hetzner's "supported actions" page
(2026-10-20, to be confirmed by probing in M1):

- **CopyObject only within the same bucket** → cross-bucket copy/move must
  fall back to a streamed download + upload through the transfer engine
  (`HetznerProfile` capability `CrossBucketCopy = false`).
- Encryption: **SSE-C only** (no SSE-S3 / SSE-KMS); SSE-C objects cannot be
  copied.
- ACLs: only canned `private` / `public-read`, set at bucket creation.
- Object lock: enabled via header at bucket creation; legal hold + retention
  supported.
- Lifecycle: only `NoncurrentVersionExpiration` with `NoncurrentDays`
  (plus object expiry per the product page).
- Bucket policy supported except the `NotAction` element.
- Conditional PUT/DELETE on versioned buckets listed as an exception →
  `ConditionalWrites` must be probed, not assumed.
- Listed as exceptions with unclear status: website, replication, logging,
  tagging, notifications, inventory, GetObjectAttributes. CORS and storage
  classes not mentioned → probe.

### 3.3 Dynamic capability safeguard

On connect, QBucket **asks the server what it can do** and makes
unsupported actions impossible — not just greyed out, but refused by the
mutation gate. Static profile knowledge (§3.1) is only the fallback.

**Model** (library: `include/qbucket/s3/Capabilities.h`)

- `Feature` enum — one entry per S3 feature the app can use (versioning,
  object lock, legal hold, retention, lifecycle, CORS, policy, ACL, tagging,
  bucket encryption, SSE-C, website, replication, logging, notifications,
  multipart listing, object versions listing, conditional writes,
  cross-bucket copy, batch delete, presign, …).
- Per feature a `CapabilityState`:

  | State | Meaning | Effect |
  |---|---|---|
  | `Supported` | probe got a valid answer (incl. "supported, not configured") | action allowed |
  | `Unsupported` | server said no (501, 405, provider-specific "not implemented") | **action impossible** |
  | `NotPermitted` | 403 — server may support it, these credentials may not use it | action disabled, reason "no permission" |
  | `Unknown` | not probeable read-only, or probe failed (5xx, timeout) | see resolution rules below |

  plus `source` (probed / profile / learned) and `checkedAt`.
- Two scopes: **server/account** capabilities (probed once per connect) and
  **bucket** capabilities + **bucket state** (probed when a bucket is opened:
  versioning enabled/suspended, object lock enabled, default encryption, …).
  Bucket state matters as much as server support: legal hold is only
  offered in buckets created with object lock.

**Prober** (library: `CapabilityProber`, runs on the browse thread)

- **Read-only probes only by default** — safe on production. Bucket probes
  are the GET subresources: `?versioning`, `?object-lock`, `?lifecycle`,
  `?cors`, `?policy`, `?policyStatus`, `?acl`, `?tagging`, `?encryption`,
  `?website`, `?replication`, `?logging`, `?notification`, `?uploads`,
  `?versions&max-keys=1`, `?publicAccessBlock`, `?ownershipControls`;
  on one existing object (if any): `?tagging`, `?acl`, `?retention`,
  `?legal-hold`, GetObjectAttributes.
- Fast: probes run in parallel (bounded), a bucket open costs ~1 s once,
  then the result is cached (per account + bucket, persisted with
  timestamp, re-probed in the background on the next connect; manual
  "Re-check" anytime).
- **Response interpretation** (with `ProviderProfile::mapError` hooks):

  | Response | Result |
  |---|---|
  | 200 with the **expected root element** | `Supported` (+ parsed state) |
  | 404 `NoSuch…Configuration`, `NoSuchTagSet`, `ObjectLockConfigurationNotFoundError`, … | `Supported`, not configured |
  | 501 `NotImplemented`, 405, provider-mapped "unsupported" codes | `Unsupported` |
  | 403 `AccessDenied` | `NotPermitted` |
  | 5xx, timeout, connection error | `Unknown` — **never** flipped to `Unsupported` |
  | 200 with an **unexpected** root element (server ignored the subresource and returned e.g. a listing) | `Unsupported` |

  The last row is a real pitfall: some servers silently ignore unknown
  subresources. Checking the root element catches it.
- **Write-only features** (conditional writes, cross-bucket copy, SSE-C,
  object lock at creation, multipart minimum part size) cannot be probed
  read-only. Optional **deep probe**: creates a temporary
  `qbucket-probe-<random>` bucket, tests there, deletes it, reports
  loudly if cleanup fails. Off by default; on accounts marked
  *Production* it needs an explicit confirmation.

**Resolution rules** (effective state per feature)

1. A definitive probe result (`Supported` / `Unsupported` / `NotPermitted`)
   wins.
2. Otherwise the provider profile's static value (§3.1/§3.2).
3. Otherwise (`GenericS3Profile`, not probeable): the action is allowed but
   marked **"not verified on this server"**; the first real call's outcome
   is **learned** into the cache (a 501 makes it `Unsupported` from then on).
4. **Robustness first:** for destructive or irreversible operations, rule 3
   does not apply — `Unknown` blocks them until a probe (or deep probe)
   has confirmed support.

**Enforcement — three layers**

1. **Library:** every `Session` call maps to `Feature`s; with a
   `CapabilitySet` attached, a call on an `Unsupported` feature returns
   `Error::Unsupported` immediately, without sending a request.
2. **App gate:** every operation declares `requires()` (e.g. *move across
   buckets* → `CrossBucketCopy` or the streamed fallback's `GetObject` +
   `PutObject` + `DeleteObject`). `OperationController` refuses anything
   whose requirements aren't met — this is the binding check.
3. **UI:** actions, menu entries, detail tabs and bucket-property
   categories are hidden or disabled with the **reason** as tooltip
   ("Not supported by this server", "No permission", "Bucket created
   without object lock", "Checking…"). While a probe is still running,
   mutating actions that depend on it show "Checking…" and stay disabled.

Where a fallback exists, the operation switches to it instead of failing
and says so in the preview ("Server can't copy across buckets — will
download and re-upload, 12.4 GB").

**Re-validation — capabilities are never "known forever"**

Servers change: a provider upgrades its RGW and gains features, or
disables one. QBucket picks that up on its own.

- **Two time scales**, both configurable in Settings → Network:

  | Scope | Changes when | Default re-check |
  |---|---|---|
  | Server / account features | provider upgrades, config changes | every **24 h**, and on every new connect when older than that |
  | Bucket state (versioning, object lock, policy, lifecycle, …) | any time, by anyone with access | on bucket open when older than **10 min**, and **always** while planning a destructive operation (§12.2) |
  | `Unknown` results | transient errors | sooner, with backoff: 5 min → 30 min → 2 h |

- **Background re-probe in long-running sessions:** a timer re-checks
  connected accounts whose results have expired — low priority on the
  browse thread, rate-limited, never blocks the UI, skipped while the
  account is idle/offline.
- **Change triggers that force an immediate re-probe:**
  - **server fingerprint changed** — the `Server` header, the set and
    format of response headers (`x-amz-request-id` / `x-amz-id-2`
    patterns), error-body format; stored with each probe result,
  - a feature marked `Supported` suddenly answers 501 / "not implemented"
    → narrowed to `Unsupported` at once, then a full re-probe,
  - the user presses "Re-check".
- **Asymmetric application** (robustness first):
  - **Downgrade** (`Supported` → `Unsupported` / `NotPermitted`) takes
    effect **immediately** — locking is the safe direction. Operations
    already running finish per item and report the failures.
  - **Upgrade** (`Unsupported` → `Supported`) takes effect after one
    definitive, root-element-validated answer — the newly available
    actions simply appear.
- **The user is told what changed:** a non-intrusive notification
  ("Hetzner fsn1 now supports: Website, Bucket logging" / "No longer
  available: …"), an entry in the journal/log, and a **change history**
  on the "Server capabilities" page (old → new, timestamp, fingerprint).
- **Profile drift detection:** when probe results contradict the static
  provider profile (e.g. `HetznerProfile` says no cross-bucket copy, a deep
  probe says yes), the capabilities page flags "profile outdated" and the
  export includes it — that's the signal to update the profile in a
  release.

**Visibility:** each account gets a **"Server capabilities"** page: every
feature with state, source, time checked, and the raw response code —
exportable as text for bug reports and for maintaining the provider
profiles.

**Rule for all future work:** a new feature or operation is only complete
with its `Feature` entry, its probe (or a documented reason why it's
write-only), its `requires()` declaration, and tests against a backend that
supports it (Ceph/VersityGW) **and** one that doesn't (Garage).

### Threading model — the UI never blocks

**Hard rule: no action, on any data size, may block or freeze the UI.**
The GUI thread only handles widgets, painting, models receiving finished
batches, and short settings writes. Everything else runs on worker threads:

| Thread | Lives there | Why |
|---|---|---|
| **GUI thread** | widgets, delegates, models (append ready-made row batches), dialogs | must stay responsive |
| **Browse thread** (1 per account) | `S3Client` + its own QNAM for listing, HEAD, metadata, bucket config; **XML parsing** | low latency, never queued behind transfers |
| **Transfer threads** (pool, default 4) | one QNAM each — also lifts QNAM's 6-connections-per-host limit; part uploads/downloads, copies, bulk-delete batches | throughput |
| **I/O pool** (`QThreadPool` / `QtConcurrent`) | MD5/SHA-256 hashing, local directory walks (folder uploads with 1M files), plan files, journal writes, sorting large row sets | CPU / disk work |

Rules:

- `QNetworkAccessManager` is thread-affine: each S3 worker thread owns its
  own QNAM and `S3Client`; no QNAM is shared across threads.
- The GUI talks to the workers through the library's thin **`s3::Session`
  proxy**: each
  call posts a request (queued invocation) with a `RequestId` and returns
  immediately; the result arrives as a **value type** (implicitly shared
  Qt containers, cheap to pass) in a **callback run on the caller's
  context object's thread** (`listObjects(params, context, callback)`) —
  exactly once, never synchronously, dropped when the context is gone.
  Cancellation by `RequestId` delivers *Cancelled* at once and aborts the
  request on the worker.
- Workers never touch widgets or models; models are only mutated on the
  GUI thread with whole batches.
- Shared state between threads is avoided; where unavoidable (adaptive
  concurrency counters, cancellation flags) it's atomics or a single
  mutex-guarded object with a documented owner.
- No `QDialog::exec()` around long work, no `QEventLoop::exec()`, no
  `waitForFinished()` on the GUI thread. Long operations show non-modal
  progress; confirmations stay modal (they don't wait on I/O).
- Large sorts / filters over loaded rows run in the I/O pool and swap in
  the result; small ones stay inline.
- **Orderly shutdown:** cancel running work, let workers flush the journal
  and resume state, then join threads with a timeout; a still-running
  transfer is persisted for resume, not killed mid-write.
- **Stall watchdog (debug + bench builds):** a GUI-thread heartbeat timer
  logs every event-loop stall > 50 ms with the active operation, so
  regressions get caught during development, not by users.

## 4. Main window layout

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ File  Edit  View  Bucket  Object  Tools  Help                               │
├─────────────────────────────────────────────────────────────────────────────┤
│ [⬆ Upload] [⬇ Download] [+ Folder] [🗑]  ◀ ▶ ↑  🔍 Filter…  [Folders|Flat|Versions] ☀ ⚙ │
├───────────────┬──────────────────────────────────────────────┬──────────────┤
│ ACCOUNTS      │ hetzner › pbs-backup › vm › 101              │ Details      │
│ ▾ Hetzner fsn1│ Name                Size    Modified   Class │ ─────────────│
│   ● pbs-backup│ 📁 2026-10/            —       —        —    │ Properties   │
│   ○ media     │ 📄 index.json      12 KB   10-18 22:01 STD   │ Headers/Meta │
│ ▾ MinIO lab   │ 📄 disk-0.img     40 GB   10-18 22:14 STD   │ Tags         │
│   ○ test      │ … loading more (fetchMore)                   │ Versions     │
│ + Add account │                                              │ Access       │
├───────────────┴──────────────────────────────────────────────┴──────────────┤
│ Transfers (2 active · 1 queued) │ Log                                       │
│ ⬆ disk-0.img  ████████░░  78 %  112 MB/s  ETA 1:12   ⏸ ✕                   │
├─────────────────────────────────────────────────────────────────────────────┤
│ 1 204 objects loaded · 38.2 GB · 3 selected                        ● online │
└─────────────────────────────────────────────────────────────────────────────┘
```

- Sidebar = `QTreeView` over `AccountTreeModel`; detail pane is a fixed-width
  `QScrollArea` like xyz-profiler's `MovieDetailWidget`.
- `Folders | Flat | Versions` is the xyz-profiler segmented control
  (`#viewSeg` / `#segText` QSS).
- Transfer panel is a `QDockWidget` (bottom, tabbed with Log), collapsible.
- Status bar mirrors xyz-profiler: counts left, connection state right.

## 5. Design system port

Copy, rename namespace, keep tokens 1:1:

- `Theme.h/.cpp` — `Palette` struct with the exact light/dark values and the
  `#2563EB` accent. Drop the film-specific helpers (`ageColor`,
  `formatBadge`, `coverGradientColors`, stars, loan) and add:
  - `storageClassBadge(QString)` → label + colour (STANDARD neutral,
    IA/ONEZONE teal, GLACIER*/DEEP_ARCHIVE blue-violet, REDUCED amber)
  - `transferStateColor(state)` (running = accent, done = green,
    failed = red, paused = text2)
  - `fileTypeColor(extension)` for the file icon tint (archive, image,
    video, text/code, other)
- `DarkFusionStyle` — QSS template verbatim (menu bar, toolbar `#tbPrimary`,
  segmented controls, `#prefSidebar`/`#prefCat` for the settings and bucket
  properties dialogs, inputs, tables, tabs, scrollbars, status bar). Keep
  the lessons in its comments: no catch-all `QWidget {}` rule, install Fusion
  once, set the font only when it changed.
- `IconFactory` — same 24×24 stroke icons and `QPixmapCache` path. Keep
  add, edit, trash, search, list, grid, sun, moon, settings, chevron(s),
  close, sort, check, filter, refresh, import; add **bucket, folder,
  folderOpen, file, upload, download, copy, link, lock, unlock, key, tag,
  history, cloud, server, pause, globe, shield**.
- Theme switch: toolbar sun/moon button + Settings → Appearance
  (Dark / Light / System), persisted, all custom painters read
  `Theme::current()`.

## 6. Milestones

Each milestone ends with: ctest green, a usable build, and `CLAUDE.md` status
updated.

### M0 — Skeleton & design system — **M** (2–3 days) — ✅ done (devlog 001)

- CMake top-level with the Qt auto-detection from xyz-profiler;
  `qt_standard_project_setup`, AUTOMOC/RCC/UIC. *(QtKeychain moved to M1,
  where `SecretStore` first uses it — it brings libsecret into the Linux and
  Flatpak builds, which is better validated together with the code that
  needs it.)*
- Port `Theme`, `DarkFusionStyle`, `IconFactory` + new icons
- `SettingsController` (theme, window/splitter state with the 300 ms debounce)
- Empty `MainWindow` with the full layout from §4 (placeholder models),
  theme toggle, About dialog, app icon (`make_icon.ps1` pattern)
- i18n scaffold (`qbucket_de.ts`), `main.cpp` translator loading as in
  xyz-profiler (incl. `qtbase_*.qm`)
- GitHub Actions: Windows build + ctest (copy `build-windows.yml`), Linux
  build + ctest
- **Flatpak skeleton from day one:** manifest
  `packaging/flatpak/io.github.theinvisible.QBucket.yml`,
  `org.kde.Platform` runtime matching our Qt 6 minimum, a
  `.desktop` file, AppStream metainfo, icon; CI job building it with
  `flatpak/flatpak-github-actions`. Building inside the sandbox early
  surfaces portal and permission issues long before release.

**Done when:** the empty app starts on Windows and as a Flatpak on Linux,
theme switches live, CI is green.

### M1 — S3 protocol core — **L** (1–1.5 weeks) — stages 1–14 done (devlog 003), Hetzner run open

Implemented in 15 stages on `main`; changes against the text below:
Session API is callback + context (§3 threading); `S3Client` is private;
`tests/docker/` lives under `libs/qbucket-s3/`; the Ceph backend is the
stop-gap image (no HTTPS yet → no SSE-C probe); only **one** Hetzner region
is available, so the done criterion verifies one region and the others per
docs; QtKeychain is found first, FetchContent second, a manifest module in
the Flatpak; 405 means "unsupported" only inside the capability prober;
the TSan job is deferred until a TSan-instrumented Qt is built in CI.

- `SigV4Signer`: header signing + query presigning; validated against the AWS
  SigV4 test suite vectors and the S3 documentation examples
  (`AKIAIOSFODNN7EXAMPLE`)
- `S3Uri`: path-style vs. virtual-hosted; S3 key encoding (`/` kept, no
  double encoding, Unicode keys, spaces, `+`)
- `S3Client` basics: ListBuckets, ListObjectsV2 (delimiter, continuation
  token, `encoding-type=url`), HeadObject, GetObject, PutObject,
  DeleteObject, DeleteObjects (batches of 1000 with Content-MD5),
  HeadBucket, GetBucketLocation
- Error parsing, `RetryPolicy` (exponential backoff + jitter; 500/503/
  SlowDown/RequestTimeout), clock-skew correction from the `Date` header on
  `RequestTimeTooSkewed`, region redirect handling (301/`x-amz-bucket-region`)
- **Library skeleton first** (§3.0): `libs/qbucket-s3` with its own
  standalone-capable CMake project, public/private header split, PIMPL,
  export macro, `install(EXPORT)` + `QBucketS3Config.cmake`, the
  `examples/s3ls` CLI, and the **standalone CI job** (library alone, no app,
  no QtKeychain). Everything below in this milestone lands in the library,
  except accounts/keychain/dialog.
- `CredentialsProvider` interface + Static/Env/AwsSharedFile providers;
  `RequestObserver` hook; logging categories with redaction
- Providers: `ProviderProfile`, `ProviderRegistry`, `GenericS3Profile`,
  `HetznerProfile`, `AwsProfile`; `S3Client` calls the hooks (§3.1)
- **Capability safeguard (library part, §3.3):** `Feature`/`CapabilitySet`,
  `CapabilityProber` with the full response-interpretation table,
  persisted cache with expiry + fingerprint, change detection (diff of two
  `CapabilitySet`s), library-level `Error::Unsupported` short-circuit; tests
  against Ceph RGW, VersityGW **and** Garage (the "unsupported" side), plus
  `FakeS3Server` cases for 403 / 5xx / ignored-subresource responses. The
  Hetzner capability matrix below is produced by this prober.
- **Hetzner capability matrix**: docs + probing against a real Hetzner
  project → `docs/providers/hetzner.md` + `HetznerProfile::capabilities()`;
  capture Hetzner response fixtures for the parser tests
- QtKeychain via FetchContent (moved here from M0), incl. libsecret in the
  Linux CI and the Flatpak manifest
- `AccountStore` + `SecretStore` (QtKeychain): every secret — secret access
  key, session token, proxy password — goes through `SecretStore` only.
  QtKeychain jobs are async, which fits the event-loop model: the session
  connects once the secret has been read. If no keychain backend is
  available (e.g. a Linux box without Secret Service), the account works
  in **"ask once per session"** mode (secret held in memory only) and the
  UI says why — never a silent plaintext fallback. Deleting an account
  deletes its keychain entries. The app hands secrets to the library
  through `KeychainCredentialsProvider`.
- `AccountDialog` with the provider preset
  list from `ProviderRegistry` (Hetzner preselected, region dropdown fills
  the endpoint) and a "Test connection" button (ListBuckets, falls back to
  HeadBucket for external buckets)
- Integration test harness (opt-in, see §7) + `tests/docker/compose.yml`
  with the single-node Ceph RGW image (own Containerfile; a community image
  as stop-gap until it's ready), VersityGW and Garage

**Done when:** the Docker backends come up healthy with one command and the
integration suite passes against Ceph RGW; listing a bucket works against
Hetzner (one region available; the others per Hetzner's docs),
all unit tests green, the Hetzner capability matrix is checked in, and
`s3ls` lists a Hetzner bucket from a standalone library build.

### M2 — Browser UI — **L** (1 week)

- `AccountTreeModel` (accounts → buckets, lazy), "add external bucket"
- `ObjectTableModel` with `canFetchMore`/`fetchMore` (1000 keys per page),
  folders first, natural sort proxy, live filter on loaded rows +
  server-side prefix search
- `PathBar` breadcrumb, back/forward/up, history per session, F5 refresh
- `ObjectRowDelegate` (icon, size humanised, date, storage-class badge)
- `DetailPane`: Properties (HeadObject), Headers & Meta (read-only in M2)
- Context menu, keyboard navigation (Enter, Backspace, Del, F2, Ctrl+C/V)
- Create folder (zero-byte `prefix/` object), delete (recursive via
  paginated list + DeleteObjects), rename (copy + delete)
- Flat view (no delimiter) as a mode of the same model

**Done when:** a 100k-object prefix scrolls smoothly in a release build and
navigation feels like a file manager.

### M3 — Transfer engine — **L** (1–1.5 weeks)

- `PartPlanner` (pure, tested): threshold 64 MiB, part size ≥ 8 MiB,
  ≤ 10 000 parts, ≤ 5 GiB per part
- `UploadTask`: single PUT or CreateMultipartUpload → UploadPart via
  `FileRangeDevice` (streaming, no part in RAM) → CompleteMultipartUpload;
  `UNSIGNED-PAYLOAD` over HTTPS, signed payload hash over plain HTTP
- `DownloadTask`: streaming to `<name>.qbpart`, Range resume, rename on
  success, mtime from `Last-Modified`
- Folder upload/download (recursive walk, structure preserved)
- `ResumeStore`: survive app restart; on start offer to resume unfinished
  jobs, validate with ListParts
- Integrity: single-part ETag == MD5 check; multipart ETag
  (MD5-of-MD5s + `-N`) check when part size is known
- `TransferPanel` + `TransferModel`: speed, ETA, pause/resume/cancel/retry,
  "clear finished"
- D&D **in**: files and folders from Explorer/Dolphin/Nautilus onto the object
  view or a sidebar bucket
- Incomplete multipart uploads view (ListMultipartUploads + Abort)

**Done when:** a 50 GB upload survives killing the app and resumes; parallel
transfers don't block browsing.

### M4 — Object operations — **M** (1 week)

- Copy/move via clipboard and internal D&D: CopyObject, UploadPartCopy for
  > 5 GiB, across buckets and accounts (cross-account = download + upload
  stream through the transfer queue)
- Edit headers and user metadata (Content-Type, Cache-Control,
  Content-Disposition, Content-Encoding, Expires, x-amz-meta-*) via
  CopyObject with `MetadataDirective=REPLACE`; bulk edit for multi-selection
- Object tags (Get/PutObjectTagging), storage class change (copy in place)
- ACL: canned ACL picker + grant table (Get/PutObjectAcl); hidden when the
  provider reports ACLs unsupported
- `PresignDialog`: GET/PUT, expiry (max 7 days), copy to clipboard, bulk
  export as list
- Folder size / object count calculation as a cancellable background job

### M5 — Versioning — **M** (3–4 days)

- Get/PutBucketVersioning (enable/suspend) in bucket properties
- `Versions` view mode: `VersionTableModel` (ListObjectVersions, paged),
  version rows grouped under the key, delete markers greyed
- Per-object Versions tab in the detail pane
- Download a specific version, restore (copy old version over latest),
  delete a version permanently, remove a delete marker (= undelete)

### M6 — Bucket management — **L** (1.5 weeks)

`BucketPropertiesDialog` with the xyz-profiler preferences-sidebar look.
Each category is driven by the capability safeguard (§3.3): unsupported
categories are disabled with the reason, `NotPermitted` ones read-only
with "no permission", and every write is checked against `requires()` in
`OperationController`. Order of work follows the
Hetzner capability matrix — what Hetzner supports gets built first:

| Category | API | Editor |
|---|---|---|
| General | location, creation date, object count/size (on demand) | read-only |
| Versioning | (M5) | toggle |
| Policy | Get/Put/DeleteBucketPolicy | `JsonEditor` (highlighting, `QJsonDocument` validation, templates: public read, deny insecure transport, restrict to IP) |
| CORS | Get/Put/DeleteBucketCors | rule table + raw XML tab |
| Lifecycle | Get/Put/DeleteBucketLifecycleConfiguration | rule form (prefix/tag filter, expiration, noncurrent expiration, abort incomplete MPU, transitions) + raw XML |
| Encryption | Get/PutBucketEncryption | SSE-S3 / SSE-KMS picker |
| Object Lock | Get/PutObjectLockConfiguration | mode + default retention; per-object retention & legal hold in the detail pane |
| Tagging | Get/Put/DeleteBucketTagging | key/value table |
| Logging | Get/PutBucketLogging | target bucket + prefix |
| Website | Get/Put/DeleteBucketWebsite | index/error document, redirects |
| Replication | Get/Put/DeleteBucketReplication | raw XML (form later) |
| Access | Get/PutBucketAcl, GetBucketPolicyStatus | grant table |

Plus: create bucket (name validation, region, object lock at creation),
delete bucket (optionally empty it first with a typed confirmation).

`SessionController` merges profile capabilities with cached per-account
probe results (§3.1).

### M7 — Drag out & platform polish — **M** (1 week)

- Windows: `platform/win/VirtualFileDrag` — custom `IDataObject` with
  `CFSTR_FILEDESCRIPTORW` + `CFSTR_FILECONTENTS` so Explorer pulls the bytes
  only on drop (streamed `IStream` backed by a blocking range download on a
  worker thread); folders expand into descriptor lists
- Linux / fallback: download to a temp dir first, then `QDrag` with file
  URLs; guarded by a size threshold with a confirmation prompt
- Open/edit object in the default app (download to temp, watch, offer
  re-upload on change)
- Bookmarks (account + bucket + prefix), recent locations
- Import accounts from `~/.aws/credentials` / `~/.aws/config` and rclone config

### M8 — Packaging & release — **M** (3–4 days)

- **Linux / Flatpak (primary):** finalise the manifest from M0, finish-args
  reviewed (`--share=network`, `--socket=wayland`/`fallback-x11`,
  `--talk-name=org.freedesktop.secrets` only if the Secret portal path is not
  enough), AppStream metainfo with screenshots and release notes,
  **Flathub submission**. Flatpak bundle attached to GitHub Releases too.
- Windows: windeployqt + Inno Setup (copy xyz-profiler's `installer.iss`
  with a new stable AppId) + ZIP; GitHub Release on tag
- AppImage: optional, only if there is demand
- Optional single binary: a static Qt build is possible under LGPL-3.0, but
  with an MIT app it obliges us to ship relinkable object files. Decide in M8
  whether that is worth it, or stay with the windeployqt folder + installer.
- README with screenshots (light + dark), provider compatibility table

### Post-parity backlog

Folder compare/sync (dry-run diff first), bandwidth limit, tabs, STS
AssumeRole / SSO, Glacier restore, requester pays, a `qbucket-cli` on top of
`qbucket::s3` + `qbucket::s3_transfer` (grown from `examples/s3ls`),
splitting `qbucket-s3` into its own repository once its API is stable.

### Timeline

| | Effort | Cumulative |
|---|---|---|
| M0 | 2–3 d | ~0.5 wk |
| M1 | 1–1.5 wk | ~2 wk |
| M2 | 1 wk | ~3 wk |
| M3 | 1–1.5 wk | ~4.5 wk |
| M4 | 1 wk | ~5.5 wk |
| M5 | 3–4 d | ~6 wk |
| M6 | 1.5 wk | ~7.5 wk |
| M7 | 1 wk | ~8.5 wk |
| M8 | 3–4 d | ~9 wk |

M0–M3 is the "daily driver"; everything after adds breadth. The table
above excludes the robustness work from §12 — with it, add ~2 weeks
(daily driver ≈ 6 weeks, parity ≈ 11 weeks; see §13.5).

## 7. Testing strategy

Protocol, provider and transfer tests live in `libs/qbucket-s3/tests/`
(fixtures in `libs/qbucket-s3/tests/data/<provider>/`); app tests (models,
OperationController, journal, accounts) in `tests/`.

**Unit (always run, no network):**

- `test_sigv4` — AWS SigV4 test-suite vectors + S3 doc examples, presigned
  URL example
- `test_s3uri` — encoding edge cases (spaces, `+`, `%`, Unicode, `//`,
  trailing `/`), both addressing styles
- `test_xml_parsers` — fixtures under `tests/data/` captured from AWS,
  Hetzner, MinIO, Ceph (each provider's quirks: missing `Owner`,
  `encoding-type` handling, quoted ETags, `x-amz-bucket-region`)
- `test_part_planner`, `test_resume_store`
- `test_object_model` — `fetchMore` paging + sort/filter against a fake
  `S3Client` served by a local `QTcpServer` stub
- `test_provider_<id>` — one per profile: endpoint building, capability
  flags, request/response hooks against that provider's fixtures under
  `tests/data/<provider>/`

**Integration (opt-in, `QBUCKET_IT_*` env vars, skipped when unset):**

- `it_s3_backend` — full CRUD, multipart (incl. abort + resume), copy > 5 GiB
  (skippable), versioning, tagging, policy round-trip
- **Hetzner Object Storage is the primary target** — every milestone's
  integration run must pass there. A local S3 server is secondary (fast
  offline loop); R2, B2, Ceph etc. whenever a profile for them is added.
  Results recorded in a provider compatibility table in the README.
- Use a dedicated Hetzner test bucket with versioning + object lock enabled
  and a lifecycle rule that expires everything after a day, so a crashed
  test run cleans itself up.
- **Local backends: Docker on Linux** (main development and test platform
  early on), via `tests/docker/compose.yml`. Each backend has a role:

  | Backend | Image | Role |
  |---|---|---|
  | **Ceph RGW (real single-node cluster)** | own image `tests/docker/ceph/Containerfile`, built on the **official `quay.io/ceph/ceph`** (pinned version + digest) | closest to Hetzner (Hetzner runs RGW): listing, errors, multipart, versioning, object lock, lifecycle, policy, SSE-C |
  | **VersityGW** | `versity/versitygw` (POSIX backend, versioning dir) | broad, actively maintained S3 implementation: versioning, object lock, policies, multipart |
  | **Garage** | `dxflrs/garage` | deliberately limited (no versioning/ACL/object lock …): exercises the "not supported by this provider" and capability-narrowing paths |
  | `FakeS3Server` | in-process | fault injection (§12.6) |

  Not used: **MinIO** (community repo archived 2026, no official images
  anymore; Chainguard rebuilds exist but are a frozen codebase),
  **LocalStack** (needs an auth token since 2026.03), **zgw-posix** (RGW
  front-end on a POSIX backend, small project — not the real thing).

  **Ceph test image** (`tests/docker/ceph/`):
  - Base: official `quay.io/ceph/ceph`, Squid v19.2.x pinned by digest; a
    Tentacle v20 build arg for a second matrix entry. Which RGW version
    Hetzner runs is unknown — the image version is a build arg so it can
    follow once we know more.
  - One container, supervised processes: `ceph-mon`, `ceph-mgr`, one
    `ceph-osd` (BlueStore on a file-backed device in a volume), `radosgw`
    (beast frontend, HTTP 8080 + HTTPS 8443 with a generated test cert —
    SSE-C needs HTTPS).
  - Bootstrap script: create the cluster, pool defaults sized for one OSD
    (`osd_pool_default_size = 1`, `mon_allow_pool_size_one = true`), RGW
    zone, a test user with fixed access/secret keys from env vars, and a
    second, restricted user for permission-denied tests.
  - Docker `HEALTHCHECK` (`ceph health` + an S3 `ListBuckets` against RGW),
    so `docker compose up --wait` blocks until it's usable.
  - Resources: plan ~2–4 GB RAM and ~10 GB disk for the volume.
  - Prior art to borrow from (both Apache-2.0, both on the official image):
    `benemon/ceph-aio` (weekly CI, healthcheck, dated tags) and
    `teran/ceph-container` (`ghcr.io/teran/ceph-container/ceph:v19.2.6`,
    `v20.2.4`). Either can serve as a stop-gap image in M1 until our own
    image exists; attribution kept if code is reused.
- Hetzner stays the reference: anything green locally but red on Hetzner
  means a missing profile quirk, not a test to relax.

## 8. Known protocol pitfalls (to handle by design)

- **Checksums:** send `Content-MD5` where S3 requires an integrity header
  (DeleteObjects, PutBucketLifecycleConfiguration, PutObjectLockConfiguration,
  PutBucketCors) — the newer `x-amz-checksum-crc32` defaults that broke the
  AWS SDKs against third-party providers are deliberately not used.
- **Addressing:** many providers need path-style; bucket names with dots
  break virtual-hosted TLS. Preset decides, account can override.
- **Region:** `us-east-1` fallback for signing; follow 301 + header to the
  bucket's region and cache it per bucket.
- **ETags** are quoted and are not MD5 for multipart or SSE-KMS objects —
  integrity check only where meaningful.
- **Folders** don't exist: a prefix may have a `prefix/` marker object or
  not; deleting the last object can make a "folder" vanish. Model refreshes
  accordingly.
- **Listing consistency:** after writes, refresh the affected prefix instead
  of patching rows optimistically, except for the row just uploaded.
- **Self-signed TLS** (internal Ceph/MinIO): per-account "trust this
  certificate" pinning, never a global "ignore SSL errors".
- **Clock skew:** compute offset from the server `Date` header and sign with
  corrected time.

## 9. Flatpak considerations

Linux ships as a Flatpak first, so these get validated in M0–M3, not at
release time:

- **Secrets:** QtKeychain → libsecret. Inside the sandbox libsecret uses the
  Secret portal (file-backed keyring in the app's data dir) when available;
  fall back to `--talk-name=org.freedesktop.secrets`. Test on GNOME and KDE.
- **File access:** file dialogs go through the FileChooser portal
  automatically with Qt; no blanket `--filesystem=home`. Downloads default to
  `xdg-download` (needs `--filesystem=xdg-download`).
- **Drag & drop in from the host file manager** crosses the sandbox via the
  document / file-transfer portal — verify early in M3 that Qt receives
  usable paths, not just `/run/user/…/doc/…` stubs that break folder uploads.
- **Drag out** (M7) from a sandbox to the host is the hardest case; the
  temp-download fallback must work there.
- **Self-signed certificates / custom CAs:** the runtime's CA bundle is
  used; per-account pinning (§8) covers internal endpoints.

## 10. Decisions log

- 2026-10-19 — Stack: C++20 / Qt 6 Widgets, own S3 client (vs. Rust/egui,
  Go/Gio, Go/miqt).
- 2026-10-19 — License **MIT**. Linux packaging **Flatpak first**. Provider
  priority **Hetzner first**, others through provider profiles.
- 2026-10-19 — All passwords / secret keys stored with **QtKeychain** via
  `SecretStore`, no plaintext fallback.
- 2026-10-19 — **Production-data rule:** robustness first (one mutation
  gate, plan → preview → execute → verify → report, journal, integrity
  checks), performance on very large buckets second (§12, §13).
- 2026-10-20 — **S3 layer as a reusable library** `libs/qbucket-s3`
  (targets `qbucket::s3`, `qbucket::s3_transfer`, `qbucket::s3_testsupport`;
  Qt Core/Network/Concurrent only; credentials, auditing, resume storage
  via interfaces; protocol safety in the library, policy in the app) (§3.0).
  Library stays Qt-based (Core/Network): a Qt-free variant would cost
  ~2–3 weeks (curl/TLS/proxy/async layer + adapter) for little real reuse.
- 2026-10-20 — **Dynamic capability safeguard:** features are probed
  (read-only) on connect / bucket open; unsupported actions are impossible,
  enforced in library, `OperationController` and UI; `Unknown` never
  unlocks destructive operations (§3.3).
- 2026-10-20 — **Capabilities are re-validated automatically:** server
  features every 24 h, bucket state after 10 min and before every
  destructive operation, immediate re-probe on server fingerprint change;
  downgrades apply at once, upgrades after a validated answer; the user is
  notified of every change (§3.3).
- 2026-10-20 — Early development and testing on **Linux with Docker**:
  a **real single-node Ceph cluster with RGW** (own image on the official
  `quay.io/ceph/ceph`, Hetzner-like), VersityGW (broad features), Garage
  (limited provider), then Hetzner as the reference (§7).
- 2026-10-19 — **Multithreaded, UI never blocks:** S3 clients with their own
  QNAM on worker threads (browse thread per account, transfer pool), I/O
  pool for hashing/disk/sorting, GUI thread only paints (§3 threading
  model).
- 2026-10-19 — Flatpak / AppStream app id **`io.github.theinvisible.QBucket`**
  (permanent once on Flathub; also used for the `.desktop` file, metainfo
  and icon names).
- 2026-10-20 — **Session API = callback + context** (`RequestId`,
  exactly-once queued delivery on the context's thread), not signals per
  operation or QFuture. `S3Client` stays private before 1.0.
- 2026-10-20 — Early phase: work **directly on `main`**, one commit per M1
  stage with green ctest.
- 2026-10-20 — Ceph test backend: **stop-gap teran/ceph-container** image
  first, own Containerfile later. Known backend deviations are recorded in
  `tests/data/it-expectations/` and asserted by the integration test.
- 2026-10-20 — Object keys with `.` / `..` path segments are **refused**:
  QNAM normalises them on the wire, the request would address another
  object.

## 11. Open questions

*(none at the moment)*

## 12. Data safety & robustness (cross-cutting, non-negotiable)

QBucket is used against **production data** almost all the time. When
safety and convenience or speed conflict, safety wins: **check once too
often rather than once too little.** Every milestone is reviewed against
this section.

### 12.1 One gate for every mutation

- The UI **never** calls a mutating `S3Client` method directly. All
  writes, deletes, copies, metadata/ACL/policy changes go through
  `controllers/OperationController`, which enforces read-only mode,
  confirmation, planning, journaling and verification in one place.
  In the app, the library's mutating API (`Session` writes, `transfer`
  classes) is only used from `controllers/` — enforced by code review and
  a grep check in CI. The library itself stays policy-free (§3.0).
- **Read-only accounts:** an account flag that disables every mutation at
  the controller level (not just greyed-out buttons). Recommended default
  for newly added production accounts; switching it off needs a
  confirmation.
- **Environment marker per account** (e.g. *Production* / *Staging* /
  *Test*, with colour) shown in the sidebar, path bar and every
  confirmation dialog. Production adds a second confirmation step for
  destructive operations.

### 12.2 Plan → preview → execute → verify → report

Every bulk or destructive operation (delete, recursive delete, move,
rename, copy with overwrite, metadata replace, upload with overwrite, bucket
config change) runs in five steps:

1. **Plan:** resolve the exact set of affected keys/versions by listing —
   streamed to an on-disk plan file for large sets, never guessed. Detect
   conflicts (target exists, object lock / legal hold, versioned bucket,
   delete markers) during planning.
2. **Preview:** confirmation dialog with the precise scope — object count,
   total size, sample keys, account + environment, bucket versioning state
   ("recoverable via versions" vs. **"permanent"**). Recursive deletes above
   a threshold and bucket deletion require typing the bucket/prefix name.
3. **Execute:** journaled (see 12.4), cancellable, resumable; per-item
   results, partial failures never abort silently.
4. **Verify:** re-check the outcome — HEAD after upload/copy (size, ETag),
   re-list after recursive delete to confirm nothing is left, re-read a
   bucket config after writing it.
5. **Report:** summary with succeeded / skipped / failed and "retry
   failed"; full detail in the journal.

### 12.3 Safe semantics for risky operations

- **Move / rename = copy → verify → delete source.** The source is deleted
  only after the copy is verified. Never a blind delete-after-copy.
- **Overwrite protection on upload/copy:** target existence is checked; a
  conflict dialog offers overwrite / skip / keep both / compare (size,
  mtime, ETag) with "apply to all". Where the provider supports conditional
  writes (`If-None-Match: *`, capability flag `ConditionalWrites` in the
  profile) they're used to close the race window; otherwise HEAD-then-PUT
  and the window is documented.
- **Metadata / header edits via copy-in-place:** a `REPLACE` copy drops
  everything not resent. So: read the full current state first (HEAD +
  tagging + ACL + storage class + SSE), merge the edit, then copy with
  `x-amz-copy-source-if-match: <etag>` so a concurrent change aborts
  instead of being clobbered. The preview mentions that versioned buckets
  get a new version.
- **Object lock / legal hold** are respected. Governance bypass
  (`x-amz-bypass-governance-retention`) is never sent implicitly — only per
  operation, after an explicit extra confirmation.
- **Versioned buckets:** "delete" creates a delete marker and says so;
  permanently deleting versions is a separate action with its own
  confirmation.
- **Cancelling a multipart upload** asks: keep for resume or abort on the
  server. Nothing is left orphaned silently; the incomplete-uploads view
  shows what remains.
- **Local side:** downloads go to `<name>.qbpart` and are renamed only
  after verification; an existing local file is never overwritten without
  the conflict dialog.
- **Presigned URLs:** long expiries and PUT presigns show a warning.

### 12.4 Journal & audit log

- Every mutating request is appended to a local **JSONL journal**:
  timestamp, account, environment, bucket, key, versionId, operation,
  result, HTTP status, `x-amz-request-id`. Shown in the Log tab, exportable.
- The journal is also the resume source for interrupted bulk operations.
- **Secrets never appear in logs:** `Authorization` headers, signatures in
  presigned URLs, session tokens are redacted at the logging layer.

### 12.5 Protocol-level correctness

- **Integrity on upload:** `Content-MD5` on every single PUT and every
  part, so the server rejects corrupted bodies. The hash is computed in a
  pre-pass on a worker thread (`QtConcurrent`), the part is then streamed.
  On by default ("Verify uploads"), can be switched off per account only
  with a warning.
- **Integrity on download:** size always; MD5 vs. ETag for single-part
  non-KMS objects; for multipart, MD5-of-part-MD5s when the part layout is
  derivable (`partNumber=1` HEAD, `x-amz-mp-parts-count`). Unverifiable
  cases are reported as "size verified only", not as verified.
- **HTTP 200 with an `<Error>` body:** CopyObject, UploadPartCopy and
  CompleteMultipartUpload can fail that way — bodies are always parsed,
  status codes alone are never trusted.
- **Retries only where safe:** idempotent requests are retried with
  backoff. CompleteMultipartUpload after a lost response is resolved by
  HEAD (did it land?) instead of a blind retry.
- **Listing correctness:** follow `IsTruncated` + continuation tokens to the
  end; a repeated token or a page that makes no progress aborts with an
  error instead of looping; an error is never shown as an empty folder;
  `encoding-type=url` keys are decoded exactly once.
- **Stale views:** cached listings are marked as such, and every
  destructive operation re-lists its scope during planning — never acts on
  what the view happens to show.

### 12.6 Testing for robustness

- **Fault-injection test server** (`libs/qbucket-s3/tests/support/FakeS3Server`,
  exported as `qbucket::s3_testsupport`, a
  `QTcpServer`): 500/503/SlowDown, connection reset mid-body, truncated
  XML, 200-with-error, slow/stalled responses, repeated continuation
  tokens, clock skew, wrong Content-Length. Every client/transfer path is
  tested against it.
- **Crash/resume tests:** kill mid-upload/mid-bulk-delete, restart, resume
  from journal, verify the end state.
- **Destructive integration tests** only run against buckets named
  `qbucket-it-*`; the harness refuses anything else.
- **Build hygiene:** warnings as errors in CI (`/W4`, `-Wall -Wextra`),
  `QT_NO_CAST_FROM_ASCII`, `QT_NO_NARROWING_CONVERSIONS_IN_CONNECT`,
  ASan + UBSan job on Linux, clang-tidy job.

## 13. Performance on very large buckets (cross-cutting)

Robustness first, but QBucket must stay fast with **millions of objects**
in a bucket and 100k+ in a single prefix. Budgets are measured in
**release builds** only.

### 13.1 Listing & models

- Never load a whole bucket: `ObjectTableModel` pages ListObjectsV2
  (1000 keys) via `fetchMore`, and prefetches the next page when the view
  nears the end. Flat view of a huge bucket works the same way.
- Compact row storage: a `std::vector` of plain structs (key, size, mtime
  as `qint64`, storage class as enum, flags) — no `QVariantMap`, no
  per-row `QObject`. Display strings are formatted lazily in `data()`.
- Inserts are batched per page (`beginInsertRows` once per page).
  `uniformRowHeights`, fixed `Interactive` column widths, no
  `ResizeToContents`.
- Sorting: by name is free (S3 returns lexicographic order). Other sort
  columns sort only the loaded rows and say so in the header tooltip; no
  `dynamicSortFilter` re-sorting on every appended page.
- XML parsing happens on the browse thread; only the finished row batch
  crosses to the GUI thread.
- Listing cache per prefix with revalidation for back/forward navigation —
  instant display, marked stale until refreshed (see 12.5).

### 13.2 Bulk operations

- Streaming pipelines: list page → plan file → act in batches → next page.
  Memory stays bounded regardless of whether 1 000 or 50 million keys are
  affected.
- DeleteObjects in batches of 1000 with several batches in flight;
  copies and HEAD checks in parallel.
- **Adaptive concurrency:** additive increase / multiplicative decrease on
  `SlowDown` / 503 / timeouts, per account — fast on a quiet endpoint,
  polite on a throttling one.
- Folder size / count calculations run as cancellable background jobs
  with progress, never on open.

### 13.3 Transfers

- Streaming I/O only (`FileRangeDevice` for parts, chunked writes for
  downloads), no whole-file or whole-part buffering in RAM.
- Transfers run on the transfer thread pool, one QNAM per thread, so the
  6-connections-per-host limit applies per thread, not globally. Browsing
  uses its own thread and QNAM and never waits behind transfers.
- Part size and concurrency tuned so a single large upload can saturate a
  1 Gbit/s link to Hetzner; checked by a benchmark.

### 13.4 Budgets & benchmarks

| Metric (release build) | Target |
|---|---|
| First rows visible after a listing response | < 100 ms |
| GUI-thread work per 1000-key page | < 10 ms |
| Memory per 100k loaded rows | < 40 MB |
| Scrolling a 1M-row flat listing | no visible stutter |
| Theme switch with 100k rows loaded | < 150 ms |
| Longest GUI-thread stall during any operation (listing, 1M-file folder upload, bulk delete, sort) | < 50 ms |
| Single 10 GB upload to Hetzner (1 Gbit/s link) | ≥ 80 % of link speed |

A `bench/` target (not part of ctest) drives these against `FakeS3Server`
with synthetic million-key listings; results are recorded per milestone so
regressions show up.

### 13.5 Where this lands in the milestones

| Milestone | Robustness / performance work added |
|---|---|
| M0 | Warnings-as-errors, Qt strictness defines, ASan/UBSan + clang-tidy CI jobs, stall watchdog |
| M1 | Library skeleton + standalone CI job, worker-thread `S3Client` + `s3::Session` proxy (TSan job on Linux CI), `FakeS3Server` fault injection, 200-with-error handling, safe-retry rules, listing loop detection, log redaction |
| M2 | Capability enforcement in `OperationController` (`requires()`) + UI reasons/tooltips + "Server capabilities" page with change history, background re-validation timer + change notifications, `OperationController` skeleton, read-only + environment marker, plan/preview/verify for delete/rename, compact model, paging budgets, `bench/` |
| M3 | Content-MD5 per part, download verification, `.qbpart` atomic finalize, conflict dialog, journal + crash/resume tests, QNAM pool, adaptive concurrency |
| M4 | Copy→verify→delete moves, metadata merge with `copy-source-if-match`, conditional writes capability |
| M5 | Versioning-aware confirmations, separate permanent-delete flow |
| M6 | Read-back verification of every bucket config write, object-lock/governance safeguards |

**Effect on the timeline:** roughly **+2 weeks** overall (fault-injection
server, operation planning, journal, verification paths). Parity estimate
moves from ~9 to **~11 weeks** full-time; the daily driver (M0–M3) from
~4.5 to **~6 weeks**.
