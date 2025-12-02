#!/usr/bin/env python3
import os
import sys
import json
import re
import urllib.request
import urllib.error
import subprocess

# Configuration
GRAMINE_DEFAULT_OWNER = "gramineproject"
GRAMINE_REPO = "gramine"
OPENSSL_REPO = "openssl/openssl"
NODEJS_DIST_URL = "https://nodejs.org/dist/index.json"
NODEJS_REPO = "nodejs/node"

# GitLab Environment Variables
CI_API_V4_URL = os.environ.get("CI_API_V4_URL", "https://gitlab.com/api/v4")
CI_PROJECT_ID = os.environ.get("CI_PROJECT_ID")
CI_JOB_TOKEN = os.environ.get("CI_JOB_TOKEN")
CI_COMMIT_SHA = os.environ.get("CI_COMMIT_SHA")
CI_COMMIT_BEFORE_SHA = os.environ.get("CI_COMMIT_BEFORE_SHA")
CI_PIPELINE_SOURCE = os.environ.get("CI_PIPELINE_SOURCE")

# Inputs (from environment variables in GitLab)
FORCE_REBUILD = os.environ.get("FORCE_REBUILD", "false").lower() == "true"
INPUT_GRAMINE_REF = os.environ.get("GRAMINE_REF", "")
INPUT_GRAMINE_OWNER = os.environ.get("GRAMINE_OWNER", "auto")

def log(msg):
    print(f"[CHECK-VERSIONS] {msg}", file=sys.stderr)

def github_api_get(path):
    url = f"https://api.github.com/{path}"
    req = urllib.request.Request(url)
    # Add User-Agent to avoid rate limiting or rejection
    req.add_header("User-Agent", "python-urllib/check-versions")
    # Optional: Add GitHub Token if provided in env for higher rate limits
    if os.environ.get("GITHUB_TOKEN"):
        req.add_header("Authorization", f"Bearer {os.environ.get('GITHUB_TOKEN')}")
    
    try:
        with urllib.request.urlopen(req) as response:
            return json.loads(response.read().decode())
    except urllib.error.HTTPError as e:
        log(f"GitHub API error for {url}: {e}")
        return None

def gitlab_api_get(path):
    if not CI_PROJECT_ID or not CI_JOB_TOKEN:
        log("Warning: CI_PROJECT_ID or CI_JOB_TOKEN not set, skipping GitLab API calls")
        return None
        
    url = f"{CI_API_V4_URL}/projects/{CI_PROJECT_ID}/{path}"
    req = urllib.request.Request(url)
    req.add_header("JOB-TOKEN", CI_JOB_TOKEN)
    
    try:
        with urllib.request.urlopen(req) as response:
            return json.loads(response.read().decode())
    except urllib.error.HTTPError as e:
        if e.code != 404:
            log(f"GitLab API error for {url}: {e}")
        return None

def read_version_file(path):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""

def check_package_exists(package_name, package_version, filename):
    # Check GitLab Generic Package Registry
    # GET /projects/:id/packages/generic/:package_name/:package_version/:file_name
    # Note: The API to *download* is the check. If we can get metadata or download, it exists.
    # A better way is listing packages, but checking specific file is direct.
    
    if not CI_PROJECT_ID or not CI_JOB_TOKEN:
        return False

    url = f"{CI_API_V4_URL}/projects/{CI_PROJECT_ID}/packages/generic/{package_name}/{package_version}/{filename}"
    req = urllib.request.Request(url, method="HEAD")
    req.add_header("JOB-TOKEN", CI_JOB_TOKEN)
    
    try:
        with urllib.request.urlopen(req) as response:
            return response.status == 200
    except urllib.error.HTTPError:
        return False

# --- OpenSSL Logic ---
def parse_openssl_tag(name):
    # Matches: openssl-3.0.14 or openssl-1.1.1w
    m = re.match(r"^openssl-(\d+)\.(\d+)\.(\d+)([a-z])?$", name)
    if not m:
        return None
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    letter = ord(m.group(4)) if m.group(4) else 0
    return {"name": name, "major": major, "minor": minor, "patch": patch, "letter": letter}

def get_latest_openssl():
    tags = github_api_get(f"repos/{OPENSSL_REPO}/tags?per_page=100")
    if not tags:
        return None
    
    candidates = []
    for t in tags:
        name = t["name"]
        if re.search(r"-(alpha|beta|pre|rc)", name, re.IGNORECASE):
            continue
        parsed = parse_openssl_tag(name)
        if parsed:
            candidates.append(parsed)
            
    if not candidates:
        return None
        
    # Sort: Major DESC, Minor DESC, Patch DESC, Letter DESC
    candidates.sort(key=lambda x: (x["major"], x["minor"], x["patch"], x["letter"]), reverse=True)
    return candidates[0]["name"]

# --- Node.js Logic ---
def parse_node_tag(name):
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", name)
    if not m:
        return None
    return {"name": name, "major": int(m.group(1)), "minor": int(m.group(2)), "patch": int(m.group(3))}

def get_latest_node_lts():
    max_major = 22 # GCC 11 compatibility
    
    try:
        with urllib.request.urlopen(NODEJS_DIST_URL) as response:
            releases = json.loads(response.read().decode())
            
        candidates = []
        for r in releases:
            if not r.get("lts"): continue
            if re.search(r"-(rc|beta|alpha|nightly)", r["version"], re.IGNORECASE): continue
            
            parsed = parse_node_tag(r["version"])
            if parsed and parsed["major"] <= max_major:
                candidates.append(parsed)
                
        if not candidates:
            return None
            
        candidates.sort(key=lambda x: (x["major"], x["minor"], x["patch"]), reverse=True)
        return candidates[0]["name"]
        
    except Exception as e:
        log(f"Failed to fetch Node.js dist: {e}")
        return None

# --- Gramine Logic ---
def get_gramine_owner():
    if INPUT_GRAMINE_OWNER != "auto":
        return INPUT_GRAMINE_OWNER
    
    # Try to find 'gramine' repo under current project namespace if possible?
    # For GitLab, we might just default to upstream unless specified.
    # The GHA logic checked if the user had a fork.
    # Here we'll default to mccoysc or gramineproject.
    # Let's assume upstream unless specified.
    return "mccoysc" # Default to mccoysc as per GHA logic fallback

def get_latest_gramine(owner, ref):
    if ref:
        commit = github_api_get(f"repos/{owner}/{GRAMINE_REPO}/commits/{ref}")
        if commit:
            return commit["sha"]
    else:
        repo = github_api_get(f"repos/{owner}/{GRAMINE_REPO}")
        if repo:
            default_branch = repo["default_branch"]
            commit = github_api_get(f"repos/{owner}/{GRAMINE_REPO}/commits/{default_branch}")
            if commit:
                return commit["sha"]
    return None

# --- Dockerfile Change Logic ---
def check_dockerfile_changed():
    if CI_PIPELINE_SOURCE == "merge_request_event":
        # Hard to check changed files in MR without API.
        # Assuming we can use git diff if fetch-depth is enough.
        pass
    
    # Use git diff if available
    if CI_COMMIT_BEFORE_SHA and CI_COMMIT_BEFORE_SHA != "0000000000000000000000000000000000000000":
        try:
            cmd = ["git", "diff", "--name-only", CI_COMMIT_BEFORE_SHA, CI_COMMIT_SHA]
            output = subprocess.check_output(cmd).decode()
            for line in output.splitlines():
                if line == "Dockerfile" or line.startswith("scripts/") or line.startswith("config/"):
                    return True
        except subprocess.CalledProcessError:
            pass
            
    return False

def main():
    # 1. Gramine
    gramine_owner = get_gramine_owner()
    latest_gramine_sha = get_latest_gramine(gramine_owner, INPUT_GRAMINE_REF)
    if not latest_gramine_sha:
        log("Failed to resolve Gramine SHA")
        sys.exit(1)
        
    latest_gramine_sha_short = latest_gramine_sha[:8]
    current_gramine_sha = read_version_file("prebuilt/gramine/VERSION") or read_version_file(".gramine-version")
    
    gramine_changed = latest_gramine_sha != current_gramine_sha
    gramine_pkg_name = "gramine"
    gramine_pkg_version = latest_gramine_sha_short
    gramine_filename = f"gramine-install-{latest_gramine_sha_short}.tar.gz"
    gramine_exists = check_package_exists(gramine_pkg_name, gramine_pkg_version, gramine_filename)
    
    needs_gramine = FORCE_REBUILD or gramine_changed or (INPUT_GRAMINE_REF != "") or not gramine_exists
    
    log(f"Gramine: latest={latest_gramine_sha_short}, exists={gramine_exists}, needs_build={needs_gramine}")

    # 2. OpenSSL
    latest_openssl = get_latest_openssl()
    current_openssl = read_version_file("prebuilt/openssl/VERSION")
    
    openssl_changed = latest_openssl != current_openssl
    openssl_pkg_name = "openssl"
    openssl_pkg_version = latest_openssl
    openssl_filename = f"openssl-install-{latest_openssl}.tar.gz"
    openssl_exists = check_package_exists(openssl_pkg_name, openssl_pkg_version, openssl_filename)
    
    needs_openssl = FORCE_REBUILD or openssl_changed or not openssl_exists
    
    log(f"OpenSSL: latest={latest_openssl}, exists={openssl_exists}, needs_build={needs_openssl}")

    # 3. Node.js
    latest_node = get_latest_node_lts()
    current_node = read_version_file("prebuilt/nodejs/VERSION")
    
    node_changed = latest_node != current_node
    node_pkg_name = "nodejs"
    node_pkg_version = latest_node
    node_filename = f"node-install-{latest_node}.tar.gz"
    node_exists = check_package_exists(node_pkg_name, node_pkg_version, node_filename)
    
    needs_node = FORCE_REBUILD or node_changed or not node_exists
    
    log(f"Node.js: latest={latest_node}, exists={node_exists}, needs_build={needs_node}")

    # 4. Docker Image
    dockerfile_changed = check_dockerfile_changed()
    needs_image = dockerfile_changed or gramine_changed or openssl_changed or node_changed
    
    log(f"Image: needs_build={needs_image} (dockerfile={dockerfile_changed})")

    # Output to build.env
    with open("build.env", "w") as f:
        f.write(f"NEEDS_GRAMINE={str(needs_gramine).lower()}\n")
        f.write(f"NEEDS_OPENSSL={str(needs_openssl).lower()}\n")
        f.write(f"NEEDS_NODE={str(needs_node).lower()}\n")
        f.write(f"NEEDS_IMAGE={str(needs_image).lower()}\n")
        
        f.write(f"GRAMINE_OWNER={gramine_owner}\n")
        f.write(f"GRAMINE_SHA={latest_gramine_sha}\n")
        f.write(f"GRAMINE_SHA_SHORT={latest_gramine_sha_short}\n")
        f.write(f"OPENSSL_VERSION={latest_openssl}\n")
        f.write(f"NODE_VERSION={latest_node}\n")
        
        # Package Registry Helpers
        f.write(f"GRAMINE_PKG_NAME={gramine_pkg_name}\n")
        f.write(f"GRAMINE_PKG_VERSION={gramine_pkg_version}\n")
        f.write(f"GRAMINE_FILENAME={gramine_filename}\n")
        
        f.write(f"OPENSSL_PKG_NAME={openssl_pkg_name}\n")
        f.write(f"OPENSSL_PKG_VERSION={openssl_pkg_version}\n")
        f.write(f"OPENSSL_FILENAME={openssl_filename}\n")
        
        f.write(f"NODE_PKG_NAME={node_pkg_name}\n")
        f.write(f"NODE_PKG_VERSION={node_pkg_version}\n")
        f.write(f"NODE_FILENAME={node_filename}\n")

if __name__ == "__main__":
    main()
