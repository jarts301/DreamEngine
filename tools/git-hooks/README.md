# Git hooks

The `offline-web` branch (ADR 0039, on that branch) is a separate line of the engine for machines
with no GPU, internet or sound, and it deletes most of what `main` has. **It must never be merged
into `main`.** Merge `main` into it, or cherry-pick single commits, instead.

Three guards stop it, each recognizing the branch by its marker file, `OFFLINE_WEB_BRANCH.md`, or
its first commit, `1c97bd61a1694ac06d3c2cf42255fa26327c1001`:

| Guard | Where | What it stops |
|---|---|---|
| `reference-transaction` hook | this clone, once installed | `main` moving onto offline-web by merge, fast-forward, pull, reset or rebase |
| `pre-push` hook | this clone, once installed | pushing offline-web into `main` on a remote |
| `.github/workflows/never-merge-offline-web.yml` | GitHub | marks any pull request or push to `main` that brings it in as failed |

Install the hooks once in each clone:

```sh
sh tools/git-hooks/install.sh
```

Hooks only protect clones that install them, and a failed check only warns. For GitHub to refuse
the merge itself, protect `main` there: Settings, Rules (or Branches), a rule for `main` that
requires pull requests and the status check **offline-web stays out of main**, with direct pushes
blocked.
