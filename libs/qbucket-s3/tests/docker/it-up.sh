#!/bin/sh
# Starts the local S3 test backends and bootstraps them; returns once all
# are usable. Idempotent — safe to run again on a running stack.
set -eu
cd "$(dirname "$0")"

docker compose up -d --wait ceph versitygw garage

# Garage: one-node layout + the test key (exec form, the image has no shell).
. ./env/garage.env
node_id=$(docker compose exec -T garage /garage node id -q | cut -d@ -f1)
if ! docker compose exec -T garage /garage layout show | grep -q "$(echo "$node_id" | cut -c1-16)"; then
    docker compose exec -T garage /garage layout assign -z dc1 -c 1G "$node_id"
    docker compose exec -T garage /garage layout apply --version 1
fi
if ! docker compose exec -T garage /garage key info "$GARAGE_KEY_ID" >/dev/null 2>&1; then
    docker compose exec -T garage /garage key import --yes -n qbucket-it "$GARAGE_KEY_ID" "$GARAGE_SECRET"
fi
docker compose exec -T garage /garage key allow --create-bucket "$GARAGE_KEY_ID" >/dev/null

# Ceph: a second user without access to the test bucket (NotPermitted tests).
. ./env/ceph.env
if ! docker compose exec -T ceph radosgw-admin user info --uid=qbucket-it-restricted >/dev/null 2>&1; then
    docker compose exec -T ceph radosgw-admin user create --uid=qbucket-it-restricted \
        --display-name="qbucket restricted" \
        --access-key="$QBUCKET_IT_RESTRICTED_ACCESS_KEY" \
        --secret-key="$QBUCKET_IT_RESTRICTED_SECRET_KEY" >/dev/null
fi

echo "S3 test backends ready: ceph :7480, versitygw :7070, garage :3900"
