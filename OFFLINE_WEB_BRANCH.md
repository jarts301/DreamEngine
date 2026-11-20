# This is the offline-web branch. Never merge it into main.

`offline-web` is a separate line of the engine for Linux machines with no internet, GPU, display or
sound card (ADR 0039). It deletes most of what `main` is: the GPU backends, SDL3, audio, Android,
vcpkg and the CI workflow. Merging it into `main` would remove all of that there.

Changes may go the other way: merge `main` into `offline-web`, or cherry-pick single commits from
`offline-web` onto `main`, such as a fix to shared code.

This file is how the guards recognize the branch, so keep it:

- `main` has a GitHub workflow, `.github/workflows/never-merge-offline-web.yml`, that fails on any
  pull request or push to `main` that contains this file or the branch's first commit,
  `1c97bd61a1694ac06d3c2cf42255fa26327c1001`.
- `main` has git hooks in `tools/git-hooks/` that refuse, in a clone where they are installed, to
  move `main` onto such a commit or to push one to `main`.
