# Review handoff for parhitmtech

This change is prepared for review on `codex/secure-workspace-ui`. Approval and passing GitHub Actions are required before merging into `main`.

Base: `main` at `45c96e1`, which contains v14.0 (`404a5ee`) plus a README-only commit. Independent code review found two regressions (duplicate-file creation and legacy password login); both were corrected and covered by passing regression tests.

## Changes to review

- Redesigned responsive IDE, login, shared-code view, and assistant with consistent typography, colors, spacing, and accessible controls.
- Searchable explorer, starter programs, keyboard command palette, persistent editor preferences, standard input, run cancellation, and source/bytecode exports.
- Serialized saves, visible save failures, draft preservation, accessible confirmation dialogs, and safeguards against overwriting binary artifacts.
- Authenticated API execution, validated session tokens, per-user workspace boundaries, rate limits, restricted origins, and bounded execution through isolated Docker workers.
- Reproducible compiler builds, container targets, GitHub Actions, and removal of tracked dependencies, environment files, and compiler binaries. Installed dependencies remain available locally.

## Verified locally

| Check | Result |
| --- | --- |
| Backend security and execution tests | 20 passed |
| Real compiler language tests | 17 passed |
| Browser workflow tests | 7 passed |
| Frontend ESLint and production build | Passed |
| Starter programs | All four executed successfully |
| Patch whitespace | Passed |

Browser tests use the real React/Monaco interface with controlled API responses; they do not validate live AWS or SOL integrations. The frontend build reports a large lazy-loaded Monaco chunk. Docker is unavailable on this development machine; CI builds both images and runs the real worker smoke test.

## Before production deployment

1. Require a green CI run, including Docker execution and image builds.
2. Supply a fresh deployment JWT secret and explicit allowed origins. Existing sessions must sign in again. Only the local secret was rotated; coordinate removal of historical secrets from Git history and any copies separately.
3. Configure the dedicated Docker worker host and shared job directory as described in [SETUP.md](SETUP.md). Docker daemon access is privileged.
4. Confirm the intentional capability changes: production GPU/provider execution and shared debugger/Git endpoints are unavailable until isolated implementations exist. Binary downloads also need an authorized backend endpoint.

No live deployment configuration or Git history was rewritten by this change. See [SETUP.md](SETUP.md) for setup, restrictions, and test commands.
