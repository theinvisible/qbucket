# AWS SigV4 test suite (vendored)

`v4/` is copied unchanged from
[awslabs/aws-c-auth](https://github.com/awslabs/aws-c-auth), directory
`tests/aws-signing-test-suite/v4/`, commit
`84a26ea184d9ccd322c8164759510b3604ab13fd`.

License: Apache-2.0 — see `LICENSE` and `NOTICE` in this directory. The rest
of qbucket-s3 is MIT; these files keep their own license.

Used by `tests/test_sigv4.cpp`. The suite targets a generic service; the
`*-normalized` cases (path normalisation, which S3 never does) and cases
with `omit_session_token` are skipped there.
