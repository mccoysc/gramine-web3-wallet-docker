#!/usr/bin/env sh

set -e

cd "$(dirname "$0")/.."

echo "Detecting repository owner from git remote..."
eval "$(scripts/resolve-repo.sh)"

export IMAGE_OWNER
export IMAGE_REPO
export IMAGE_TAG="${IMAGE_TAG:-latest}"

echo "Using Docker image: ghcr.io/${IMAGE_OWNER}/${IMAGE_REPO}:${IMAGE_TAG}"

docker compose up "$@"
