# Hetzner Object Storage fixtures

Captured 2026-10-20 from a real project, location **nbg1**, bucket
`qbucket-it-test` (no versioning, no object lock, no lifecycle rule), with
`s3ls --provider hetzner --region nbg1 --dump-responses … probe`.

- `probe-<subresource>-<status>.xml` — the read-only capability probes.
  Owner IDs are replaced by `OWNER-ID-REDACTED`; request IDs are kept (they
  name the RGW cluster, `nbg1-prod1-ceph5`). No request headers were ever
  recorded.
- `error-access-denied-sporadic.xml` — a `403 AccessDenied` with an empty
  message that Hetzner returned for ~10–30 % of otherwise identical, valid
  requests shortly after the bucket was created (see
  `docs/providers/hetzner.md`).
- `put-encryption-400-proxy.xml` — `PUT /?encryption` (SSE-S3 default
  encryption) answered by Hetzner's front proxy (32-hex request id, not an
  RGW `tx…` id) with InvalidArgument.
- **`DELETE /?encryption` was answered `409 BucketNotEmpty`** — the proxy
  executed it as **DeleteBucket**. The bucket survived only because it held
  objects. Never send subresource writes Hetzner does not list as supported;
  never write-probe a bucket without a guard object in it.
- `lock-*.xml` — bucket `qbucket-it-lock`, created with object lock (and
  therefore versioning) via rclone: `?object-lock` (enabled, no default
  retention), `?versioning` (Enabled), and `?retention` / `?legal-hold` on an
  object without either — RGW answers 404 `ObjectLockConfigurationNotFoundError`
  there (AWS: `NoSuchObjectLockConfiguration`).
- `error-no-such-bucket-stale-gateway.xml` — 2026-10-21: one RGW gateway
  (`2fa69feb`) answered DELETEs on `qbucket-it-lockcfg`, created about an
  hour before, with `404 NoSuchBucket` and an empty message, the others
  normally. `error-no-such-bucket.xml` is the proxy's answer for a bucket
  that really doesn't exist (with a message).
