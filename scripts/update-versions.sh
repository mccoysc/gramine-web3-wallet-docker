#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSIONS_FILE="$REPO_ROOT/prebuilt/versions.json"

COMPONENT=""
VERSION=""
ARTIFACT=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --component)
            COMPONENT="$2"
            shift 2
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --artifact)
            ARTIFACT="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [ -z "$COMPONENT" ] || [ -z "$VERSION" ] || [ -z "$ARTIFACT" ]; then
    echo "Usage: $0 --component <gramine|openssl|nodejs> --version <version> --artifact <artifact-name>"
    exit 1
fi

TIMESTAMP=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

case $COMPONENT in
    gramine)
        jq --arg ref "$VERSION" \
           --arg artifact "$ARTIFACT" \
           --arg timestamp "$TIMESTAMP" \
           '.gramine.ref = $ref | .gramine.artifact = $artifact | .gramine.built_at = $timestamp' \
           "$VERSIONS_FILE" > "$VERSIONS_FILE.tmp" && mv "$VERSIONS_FILE.tmp" "$VERSIONS_FILE"
        ;;
    openssl)
        jq --arg version "$VERSION" \
           --arg artifact "$ARTIFACT" \
           --arg timestamp "$TIMESTAMP" \
           '.openssl.version = $version | .openssl.artifact = $artifact | .openssl.built_at = $timestamp' \
           "$VERSIONS_FILE" > "$VERSIONS_FILE.tmp" && mv "$VERSIONS_FILE.tmp" "$VERSIONS_FILE"
        ;;
    nodejs)
        OPENSSL_VERSION=$(jq -r '.openssl.version' "$VERSIONS_FILE")
        jq --arg version "$VERSION" \
           --arg artifact "$ARTIFACT" \
           --arg openssl_version "$OPENSSL_VERSION" \
           --arg timestamp "$TIMESTAMP" \
           '.nodejs.version = $version | .nodejs.artifact = $artifact | .nodejs.openssl_version = $openssl_version | .nodejs.built_at = $timestamp' \
           "$VERSIONS_FILE" > "$VERSIONS_FILE.tmp" && mv "$VERSIONS_FILE.tmp" "$VERSIONS_FILE"
        ;;
    *)
        echo "Unknown component: $COMPONENT"
        exit 1
        ;;
esac

echo "Updated $COMPONENT to version $VERSION in versions.json"
cat "$VERSIONS_FILE"
