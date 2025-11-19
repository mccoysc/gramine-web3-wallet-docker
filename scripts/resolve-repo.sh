#!/usr/bin/env sh

set -e

url="$(git remote get-url origin 2>/dev/null || echo "")"

if [ -z "$url" ]; then
    echo "Warning: Could not detect git remote, using defaults" >&2
    echo "IMAGE_OWNER=mccoysc"
    echo "IMAGE_REPO=gramine-web3-wallet-docker"
    exit 0
fi

owner="$(printf '%s' "$url" | sed -E 's#.*github.com[/:]([^/]+)/.*#\1#')"
repo="$(printf '%s' "$url" | sed -E 's#.*github.com[/:][^/]+/([^/.]+)(\.git)?$#\1#')"

if [ -z "$owner" ] || [ -z "$repo" ]; then
    echo "Warning: Could not parse git remote URL: $url" >&2
    echo "IMAGE_OWNER=mccoysc"
    echo "IMAGE_REPO=gramine-web3-wallet-docker"
    exit 0
fi

echo "IMAGE_OWNER=$owner"
echo "IMAGE_REPO=$repo"
