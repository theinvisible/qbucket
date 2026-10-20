#!/bin/sh
# PLAN.md §12.1 — one gate for every mutation: the library's writing API
# (Session::putObject, deleteObject(s), copyObject, the multipart calls,
# createBucket, deleteBucket) is called from src/controllers/ only, where
# every call runs inside a Job of the OperationController. Run from the
# repository root.
set -eu
pattern='(->|\.)(putObject|deleteObjects?|copyObject|createMultipartUpload|uploadPart|uploadPartCopy|completeMultipartUpload|abortMultipartUpload|createBucket|deleteBucket)[[:space:]]*\('
hits=$(grep -rnE "$pattern" src --include='*.cpp' --include='*.h' | grep -v '^src/controllers/' || true)
# The transfer engine's writers (Uploader, Copier) as well.
writers='qbucket/s3/transfer/(Uploader|Copier)\.h'
hits="$hits$(grep -rnE "$writers" src --include='*.cpp' --include='*.h' | grep -v '^src/controllers/' || true)"
if [ -n "$hits" ]; then
    echo "Mutating S3 calls outside src/controllers/ (PLAN.md §12.1):"
    echo "$hits"
    exit 1
fi
echo "mutation gate: ok"
