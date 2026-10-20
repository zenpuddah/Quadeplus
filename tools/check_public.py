"""Validate the staged public file inventory and content before publication."""

import json
from pathlib import PurePosixPath
import re
import subprocess

POLICY = "tools/public-files.json"
SELF = "tools/check_public.py"
PRIVATE = re.compile(
    r"(?i)quadeplus-dev|lexongine-dev|quant-project|\bopencode\b|\bchatgpt\b|"
    r"\bopenai\b|\bclaude\b|github copilot|as an ai|owner.accepted|\badvisor\b|"
    r"\bcouncil\b|/Users/|/var/folders/|/home/|\b(?:Task|Milestone)\s+\d+"
)
SECRETS = re.compile(
    r"BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY|gh[pousr]_[A-Za-z0-9]{20,}|sk-[A-Za-z0-9]{24,}"
)
FORBIDDEN = {"agents.md", "claude.md", ".opencode", ".claude", ".cursor", ".cursorrules"}


def allowed(path, policy):
    parts = PurePosixPath(path).parts
    if any(part.lower() in FORBIDDEN for part in parts):
        return False
    if path in policy["files"]:
        return True
    return (
        any(
            path.startswith(directory + "/") and PurePosixPath(path).suffix in extensions
            for directory, extensions in policy["source_directories"].items()
        )
        and path not in policy["excluded_files"]
    )


def content_issue(path, content):
    if SECRETS.search(content):
        return "credential marker"
    # The checker must name prohibited markers to detect them. Its own policy
    # literals are reviewed tooling, not exported development records.
    if path != SELF and PRIVATE.search(content):
        return "private/development marker"
    return None


def main():
    staged = subprocess.check_output(["git", "ls-files", "--stage", "-z"])
    entries = {}
    for entry in staged.split(b"\0"):
        if not entry:
            continue
        metadata, path = entry.split(b"\t", 1)
        mode, oid, stage = metadata.decode().split()
        if stage != "0" or mode not in {"100644", "100755"}:
            raise SystemExit(f"Unsupported staged entry: {path.decode()}")
        entries[path.decode()] = oid
    if POLICY not in entries:
        raise SystemExit("Stage the public file policy before running this check")

    def blob(path):
        return subprocess.check_output(["git", "cat-file", "blob", entries[path]]).decode("utf-8")

    policy = json.loads(blob(POLICY))
    missing = set(policy["files"]) - entries.keys()
    if missing:
        raise SystemExit(f"Missing required public files: {sorted(missing)}")
    for path in entries:
        if not allowed(path, policy):
            raise SystemExit(f"Unapproved public path: {path}")
        try:
            issue = content_issue(path, blob(path))
        except UnicodeDecodeError:
            issue = "non-text content"
        if issue:
            raise SystemExit(f"Publication review required: {path}: {issue}")
    print(f"Public content checks passed: {len(entries)} approved text files")


if __name__ == "__main__":
    main()
