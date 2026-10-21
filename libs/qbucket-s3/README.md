# qbucket-s3

A threaded, provider-aware S3 client library for Qt 6 — the protocol core of
[QBucket](https://github.com/theinvisible/qbucket), usable on its own.

- Depends on **Qt 6.5+ Core and Network** only; the transfer engine
  (`qbucket::s3_transfer`) adds Concurrent. No Widgets, no QSettings, no
  keychain.
- Own SigV4 signing on `QNetworkAccessManager` — no AWS SDK.
- Provider quirks live in provider profiles, never in the protocol core.
- Errors are values (`Result<T>` / `Error`), no exceptions.

> Status: **pre-1.0, under construction** (QBucket milestones M1–M4). The
> API changes between minor versions; see [CHANGELOG.md](CHANGELOG.md).

Two targets:

- **`qbucket::s3`** — `Session` (worker thread, optional transfer lanes),
  SigV4, every object and multipart request, ranged / part-number reads
  streamed into files, uploads streamed from file ranges, copies with
  `MetadataDirective::Replace` (edits in place), object tags and canned
  ACLs (with write-only capability features a read probe never unlocks),
  presigned GET / PUT links, capabilities, provider profiles (storage
  classes, canned ACLs).
- **`qbucket::s3_transfer`** — `Uploader`, `Downloader`, `Copier` (multipart
  in parallel, Content-MD5 on every part, ETag verification, pause / cancel),
  `AdaptiveConcurrency` (AIMD on SlowDown / 503), `PartPlanner`, hashing,
  and `ResumeStore` / `JsonFileResumeStore`: a transfer killed with SIGKILL
  continues where it stopped (`examples/s3cp`, `tests/test_transfer_crash`).

## Building

Standalone:

```sh
cmake -S libs/qbucket-s3 -B build-s3 -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
cmake --build build-s3
ctest --test-dir build-s3 --output-on-failure
```

Options:

| Option | Default | Meaning |
|---|---|---|
| `QBUCKET_S3_BUILD_SHARED` | `OFF` | shared instead of static library |
| `QBUCKET_S3_BUILD_TESTS` | on when top-level | build the test suite |
| `QBUCKET_S3_INSTALL` | on when top-level | install rules + CMake package |
| `QBUCKET_S3_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` / `/WX` |
| `QBUCKET_S3_SANITIZE` | empty | `address,undefined` or `thread` (GCC/Clang) |

## Using it

As a subdirectory (preset options with `set()` before adding it):

```cmake
set(QBUCKET_S3_BUILD_TESTS OFF)
add_subdirectory(third_party/qbucket-s3)
target_link_libraries(myapp PRIVATE qbucket::s3)            # or qbucket::s3_transfer
```

From an installed copy:

```cmake
find_package(QBucketS3 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE qbucket::s3)
```

Headers are included as `<qbucket/s3/…>`; everything is in namespace
`qbucket::s3` (transfers: `qbucket::s3::transfer`). `examples/s3ls` lists,
`examples/s3cp` uploads and downloads with resume:

```sh
export AWS_ACCESS_KEY_ID=… AWS_SECRET_ACCESS_KEY=…
s3cp --provider hetzner --region nbg1 --resume-dir ~/.s3cp --id iso upload big.iso my-bucket isos/big.iso
s3cp --provider hetzner --region nbg1 --resume-dir ~/.s3cp resume iso      # after a crash or kill
```

## Testing

Unit tests need nothing but Qt: `ctest --test-dir build-s3`. They run the
real client against `testing::FakeS3Server` (target `qbucket::s3_testsupport`,
available to your own tests too), which verifies SigV4 signatures from the
received bytes and injects faults, and `testing::FakeObjectStore`, a stateful
store behind it (objects with data, multipart, ranges, part numbers, copies,
listings, scripted faults, a discard mode for uploads of any size).

Integration tests run against real S3 servers. Local ones in Docker:

```sh
libs/qbucket-s3/tests/docker/it-up.sh          # Ceph RGW, VersityGW, Garage
cmake -S libs/qbucket-s3 -B build-s3 -DQBUCKET_S3_IT_LOCAL=ON
cmake --build build-s3 && ctest --test-dir build-s3 -L docker
libs/qbucket-s3/tests/docker/it-down.sh        # stop + delete data
```

Any other endpoint (Hetzner, AWS, …) via environment variables — see the
header of `tests/it_s3_backend.cpp`. The test only touches buckets named
`qbucket-it-*`, and only below a per-run prefix.

## Rules

- Public API is `include/qbucket/s3/**`; everything under `src/` is private
  and may change at any time.
- Thread-safety is documented on every public class.
- Protocol-level safety (Content-MD5, 200-with-error bodies, idempotent-only
  retries, listing loop detection, secret redaction) is the library's job;
  policy (confirmations, read-only accounts) belongs to the application.

## License

MIT — see [LICENSE](LICENSE).
