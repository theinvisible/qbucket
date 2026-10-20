# Devlog 004 — M1: Hetzner validation

**Date:** 2026-10-20

## What was done

Stage 15 of M1: the library against a real Hetzner Object Storage project
(location nbg1, bucket `qbucket-it-test`), then encoding what was found.

- `s3ls --provider hetzner --region nbg1 buckets` / `probe` from the
  standalone library build; `it_s3_backend` passes **12/12** (2 skipped:
  no restricted user, no repeated-slash deviation) — object round trip with
  MD5, all 16 problem keys including control characters, paging, batch
  delete, presigned GET, wrong secret, clock skew, capability snapshot.
- Fixtures: 18 probe responses and two error bodies in
  `libs/qbucket-s3/tests/data/hetzner/` (owner ids redacted, captured with
  `s3ls --dump-responses`).
- Capability matrix: `docs/providers/hetzner.md`, snapshot in
  `it-expectations/hetzner.json`.

## Findings

1. **Sporadic `403 AccessDenied` with an empty message** for 10–30 % of
   valid requests on the freshly created bucket — reproduced with an
   independent Python signer, so not a client bug. 0 of 100 after ~20
   minutes: a start-up effect of new buckets. Handled generically:
   `Error::retryable`, set by `HetznerProfile::mapError` for exactly this
   shape; `RetryPolicy` retries it for idempotent requests, the category
   stays `NotPermitted`. A genuine denial still ends as one.
2. **`DELETE /?encryption` was executed as DeleteBucket.** While checking
   whether the "Supported" read answer for `?encryption` holds for writes
   (Hetzner documents no SSE-S3/KMS), the front proxy answered the PUT
   itself (`400 InvalidArgument`, 32-hex request id) and the DELETE with
   `409 BucketNotEmpty`. The bucket survived only because objects from an
   earlier, interrupted test run were still in it. Consequences:
   - `HetznerProfile::reviewProbe` overrides `bucket.encryption` to
     Unsupported; the library's short-circuit refuses such requests.
   - New rule (CLAUDE.md safety rules, PLAN.md §3.3): **a read probe never
     unlocks a write**; destructive subresource writes need confirmed write
     support; write/deep probes only on a scratch bucket with a guard
     object. No M1 operation sends such writes; M6 must respect this.
3. Virtual-hosted addressing, signing with the location as region, RGW
   underneath (`tx…-nbg1-prod1-ceph5`) and no `Server` header — as the
   profile assumed.
4. Website 405, bucket tagging / replication / logging / notifications /
   ownership controls 501 — while object tagging works.

## Pitfall in our own test

`VERIFY_OK(put(...))` evaluated its argument up to three times — every
check sent the request again. Invisible on the local backends, it turned
the sporadic 403s into confusing failures. The macro now evaluates once.

## M1 status

The done criteria of PLAN.md §6 are met for the one available region:
Docker backends with one command, integration suite green against Ceph RGW
(and VersityGW, Garage), listing and the full suite against Hetzner, all
unit tests green, capability matrix checked in, `s3ls` lists a Hetzner
bucket from a standalone build. **CI has not run M1 yet** (not pushed).

## Open

- First CI run of M1 (Windows MSVC, FetchContent QtKeychain + libsecret,
  Flatpak `qtkeychain` module, integration workflow).
- fsn1 / hel1; a bucket with versioning and object lock; write
  verification of CrossBucketCopy and SSE-C — on a guarded scratch bucket.
- Map which subresource writes Hetzner's proxy intercepts, before M6.
- The test bucket keeps the leftover objects on purpose: they are its
  guard against a misinterpreted DeleteBucket.
