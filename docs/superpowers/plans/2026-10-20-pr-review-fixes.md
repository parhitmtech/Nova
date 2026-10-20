# PR security review fixes

Goal: address the ten findings in PR #1 without restoring removed credentials or artifacts.

- Execution (`execution.js`, worker Dockerfile, executor/worker tests): reproduce file limit and slot starvation; use 16777216 bytes, two batch slots per user, a separate two-slot REPL pool with one slot per user, container deadlines, Docker client env forwarding, and instance-scoped startup sweep. Verify real processes, Docker specs and real worker CI.
- Server/security (`server.js`, `security.js`, security tests): reproduce proxy/origin, health, symlink-root, storage and idle timer issues. Configure trusted proxy ranges, require explicit production origins, canonicalize trusted roots, enforce byte/entry quotas across HTTP and terminal writes, and expose safe capabilities.
- Session/UI (auth context, API client, terminal, IDE, browser tests): authenticate sockets with an initial message, restrict unauthenticated sockets, use one-hour JWTs in sessionStorage, and hide disabled GPU actions. Preserve local editor preferences.
- Verify backend/security/language tests and frontend lint/build/browser tests. Update deployment guidance. Commit on remote PR history and fast-forward push to PR branch.

Deployment limits: live secret rotation requires access to each deployment's secret manager; do not claim it from local validation. Docker tests require a daemon and run in CI if unavailable locally.

Verification: 36 backend security/execution/storage/API regressions and 17 language tests passed; frontend lint/build and all seven Playwright tests passed. Independent review found a malformed-upgrade capacity leak; it was fixed and covered by the final suite. Real Docker execution remains a CI check because Docker is unavailable locally.
