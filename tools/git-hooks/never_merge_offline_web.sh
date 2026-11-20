#!/bin/sh
# Shared by the hooks here: exits 0 when commit $1 holds the offline-web branch (ADR 0039), which
# must never reach main. It is recognized by its marker file or its first commit.
offline_web_first_commit=1c97bd61a1694ac06d3c2cf42255fa26327c1001
git cat-file -e "$1:OFFLINE_WEB_BRANCH.md" 2>/dev/null && exit 0
git cat-file -e "$offline_web_first_commit" 2>/dev/null &&
    git merge-base --is-ancestor "$offline_web_first_commit" "$1" 2>/dev/null && exit 0
exit 1
