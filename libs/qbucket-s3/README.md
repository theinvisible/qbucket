# qbucket-s3

A threaded, provider-aware S3 client library for Qt 6 — the protocol core of
[QBucket](https://github.com/theinvisible/qbucket), usable on its own.

- Depends on **Qt 6.5+ Core and Network** only (the transfer engine later adds
  Concurrent). No Widgets, no QSettings, no keychain.
- Own SigV4 signing on `QNetworkAccessManager` — no AWS SDK.
- Provider quirks live in provider profiles, never in the protocol core.
- Errors are values (`Result<T>` / `Error`), no exceptions.

> Status: **pre-1.0, under construction** (QBucket milestone M1). The API
> changes between minor versions; see [CHANGELOG.md](CHANGELOG.md).

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
target_link_libraries(myapp PRIVATE qbucket::s3)
```

From an installed copy:

```cmake
find_package(QBucketS3 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE qbucket::s3)
```

Headers are included as `<qbucket/s3/…>`; everything is in namespace
`qbucket::s3`.

## Rules

- Public API is `include/qbucket/s3/**`; everything under `src/` is private
  and may change at any time.
- Thread-safety is documented on every public class.
- Protocol-level safety (Content-MD5, 200-with-error bodies, idempotent-only
  retries, listing loop detection, secret redaction) is the library's job;
  policy (confirmations, read-only accounts) belongs to the application.

## License

MIT — see [LICENSE](LICENSE).
