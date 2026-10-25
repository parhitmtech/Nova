# Setup, security, and frontend verification

NovaComp now requires authenticated execution and isolates each user's local workspace. Programs run in a restricted Docker worker by default. The frontend includes searchable files, a keyboard command palette, saved editor preferences, starter examples, explicit save failures, input controls, cancellation, and responsive panels.

## Local development

Use Node 22.12 or newer, `make`, and a C++17 compiler. The project no longer tracks installed dependencies or compiled binaries. The repository has three parts: `compiler/` (the Nova compiler, built with `make` into `compiler/build/nova`), `server/` (the Node backend) and `frontend/` (the React app).

```sh
make -C compiler                # builds compiler/build/nova
cd server
npm ci
cp .env.example .env
```

Set `JWT_SECRET` to a new random secret of at least 32 bytes. Generate it with `node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"`. Configure your AWS bucket, tables, Bedrock permissions, and credentials or instance role. Old tokens will require a new sign-in because token verification now checks identity, issuer, audience, and expiration.

Build the execution image from the repository root:

```sh
docker build --target worker -t novacomp-worker:local .
```

Start the backend with `npm start` from `server/`. It finds the compiler at `compiler/build/nova` and the stdlib under `compiler/`; override with `NOVA_COMPILER_PATH` and `NOVA_HOME`. Then start the frontend:

```sh
cd frontend
npm ci
VITE_API_URL= npm run dev
```

An empty API URL uses the same origin; Vite proxies API and terminal requests to localhost:3001. Set `NOVA_ALLOWED_ORIGINS` to the exact frontend origins. Production startup fails unless this list contains at least one valid explicit origin, and in production every listed origin must use `https:` (plain-http origins such as `http://localhost:5173` are accepted only outside production). Configured origins form an exclusive allowlist for CORS and WebSockets.

Set `NOVA_TRUST_PROXY` to the proxy's addresses/CIDRs (for example `loopback` for a local TLS proxy) or a verified hop count (for example `1`). It defaults to `false`; unrestricted `true` is rejected, and a production server logs a warning at startup when it is unset. The proxy must overwrite forwarded headers and the backend must not be directly reachable by clients. This lets Express, and the WebSocket upgrade limits, use the actual client IP for rate limits. See [Express proxy configuration](https://expressjs.com/en/guide/behind-proxies/).

For trusted local programs only, set `NOVA_EXECUTION_MODE=local`. Production rejects local execution. Local mode removes backend credentials from the process environment and applies time/output/concurrency limits, but does **not** provide operating-system filesystem, memory, or network isolation.

## Restricted execution

The worker has no network, a read-only root filesystem, no capabilities, no new privileges, and a non-root UID. Each job receives only its own temporary directory, one CPU, 256MB RAM, 32 processes, a 16MB maximum file size, and a bounded temporary filesystem. Program output is capped at 256KB. Web execution is limited to 30 seconds; `// @ml` cannot increase this. Each backend has five batch slots, with at most two per user, plus a separate two-slot REPL pool with at most one REPL per user. Idle REPLs therefore cannot occupy batch slots. Exhausted HTTP execution quotas return 503 with a busy error. REPL inactivity expires after five minutes (reset on each input), with an independent 30-minute maximum lifetime. A stopped stream kills the process group and removes its container. A job killed inside its container before its deadline (exit status 137) is reported as running out of memory; one killed at its deadline is reported as timed out.

The worker command runs under GNU `timeout --signal=KILL` (the image's entrypoint is replaced by `/usr/bin/timeout`, which then starts `/opt/nova/nova`), so its deadline survives an API-server crash. Job containers carry `nova.job=true` and `nova.worker=<NOVA_WORKER_ID>` labels. Before listening, the server sweeps containers matching both labels and refuses startup if Docker cleanup fails. The sweep removes only containers that have exited, or that are older than the longest job deadline (30 minutes plus a minute of slack); a running job is never removed, even one belonging to another live instance. Set a stable, unique `NOVA_WORKER_ID` for each backend instance sharing a Docker daemon; production refuses to start without one. Legacy containers without these labels must be removed once by the operator. Docker receives the file-size ulimit as `16777216:16777216` bytes ([Docker ulimit reference](https://docs.docker.com/reference/cli/docker/container/run/#ulimit)).

Every web-triggered compiler path uses `server/src/services/execution.js`. Backend AWS/JWT/SSH credentials are not copied into worker environments. Program-emitted file markers may upload only regular, non-symlink files inside that job's directory, with bounded file size and count.

GPU and provider execution remain disabled in the secure production configuration. The existing GPU adapter is available only with **both** `NOVA_ENABLE_GPU=true` and explicit trusted local mode in a nonproduction environment; SOL support also requires its optional SSH dependencies and credentials. The authenticated `/api/capabilities` endpoint reports `{ gpu, ml }`; the IDE hides GPU actions unless `gpu` is true. ML provider registration and annotation-based extended deadlines are unavailable (`ml: false`), and untrusted `NOVA_S3_FILE` markers are ignored. A separate authenticated provider worker is needed before exposing GPU jobs publicly. Shared debug and Git endpoints report unavailable until they have isolated user sessions.

## Production builds

Build the frontend with `npm run build`. When running Node directly on a Linux host with Docker, the server serves `frontend/dist` by default (set `NOVA_STATIC_DIR` to serve a build from elsewhere). Serve only that build; backend source and workspace directories are private.

`docker build --target server -t novacomp-server:local .` also produces a non-root server image containing the frontend and Docker client. When using it, mount the jobs directory at the **same absolute path on the host and in the server** (`/var/lib/novacomp/jobs`) because the host daemon resolves worker bind mounts. Give it UID 1000 ownership. Configure socket access for the server's user, or use a dedicated worker host. Docker daemon access is a privileged operator capability; use a dedicated execution host with minimal AWS privileges rather than sharing it with unrelated workloads. Never mount the daemon socket or credentials into program workers. `DOCKER_HOST`, `DOCKER_CONFIG`, `DOCKER_CONTEXT`, `DOCKER_TLS_VERIFY`, `DOCKER_CERT_PATH`, and `DOCKER_API_VERSION` are preserved for job-launch, removal, and sweep clients only. The Docker client also receives `HOME` and `SSH_AUTH_SOCK` for contexts/SSH transports; configure any required SSH client and credential helper on the API host. These are never passed to the compiler container. Remote workers must have the jobs directory available at the same absolute path as the API host.

Set `NODE_ENV=production`, supply a new deployment secret through your secret manager, configure the public HTTPS origin, `NOVA_WORKER_ID` and `NOVA_TRUST_PROXY`, and use HTTPS. Environment files and keys are excluded from the Docker build context.

Secret rotation on running deployments cannot be verified from this repository. Before deploying, rotate `JWT_SECRET` in every deployment secret manager and restart every replica to invalidate old tokens. Record completion per deployment; the known historical default is rejected at startup. The previous secret still exists in old Git commits: after coordinating with collaborators, scrub those commits and any forks/caches that held it. Removing tracking alone does not remove historical values. No history was rewritten automatically.

## Workspace and session limits

`/fs/*` is limited to 120 requests per authenticated user per minute. Persistent local workspaces default to 50 MiB and 1,000 entries per user, including directories to limit inode consumption. Configure `NOVA_WORKSPACE_MAX_BYTES` and `NOVA_WORKSPACE_MAX_ENTRIES` as positive integers. Quota failures return HTTP 413 without changing file contents. The same storage accounting covers terminal writes, copies, appends, directories, deletes, compiled metadata and S3 hydration (whose downloads stop once they exceed the quota). Each workspace's usage totals are measured once and then updated by every mutation, so a write does not walk the tree. Checks and mutations are synchronous in one backend process; a workspace must not be shared by multiple writable API replicas without a shared quota service or filesystem quotas. Terminal file writes and shell mutations (`touch`, `echo >`, `cp`, `mv`, `mkdir`, `rm`, `novacomp`) are also rate limited per socket (a burst of 20, then 5 per second). Docker job artifacts have their separate limits described above.

`/health` publicly returns only `{ "status": "ok" }`. It is a liveness endpoint, not a detailed diagnostics endpoint.

New login/signup tokens expire after one hour. The browser stores them in tab-scoped `sessionStorage` and discards old `localStorage` credentials; a new sign-in is required. Terminal/REPL WebSocket URLs contain no credentials. The first frame must be `{ "type": "auth", "token": "..." }` within five seconds; no compiler or shell session starts until validation succeeds, and a failed authentication drops the connection immediately. Query-string tokens are rejected. Unauthenticated sockets are bounded globally and per client IP, and only authenticated sockets count toward the session limits, so anonymous clients cannot lock signed-in users out. Connections close at JWT expiration. Use HTTPS/WSS in production. `sessionStorage` is still accessible to same-origin JavaScript, so script injection prevention remains necessary.

## Tests

```sh
make -C compiler test     # build + golden language tests (compiler/tests/*.nova)

cd server
npm test                 # real HTTP/WebSocket security + execution tests
npm run test:worker      # real Docker execution, >16 KiB artifacts and independent deadlines

cd ../frontend
npm run lint
npm run build
npx playwright install chromium   # once, if no local Chrome is available
npm run test:e2e
```

Browser tests exercise the real React/Monaco UI against controlled API responses. They verify keyboard settings, file-switch saves, save-error recovery, stdin, hidden disabled GPU actions and tab-scoped tokens, binary safeguards, and mobile navigation. They do not contact AWS or SOL. GitHub Actions runs builds, all these tests, and both container builds.

Keyboard shortcuts: Ctrl/Cmd+S saves, Ctrl/Cmd+Enter runs, Ctrl/Cmd+K opens commands. Dialogs support Tab/Shift+Tab and Escape. Files are saved before switching, failed saves keep the draft open, and binary artifacts (models, archives) cannot enter autosave. Binary downloads need a dedicated authorized backend endpoint and are not represented as a working feature.

## Required CI checks

The workflow reports the exact eight merge-gate names: Repo hygiene, Backend unit tests, Compiler (g++), Compiler (clang++), Compiler (ASan + UBSan), Frontend lint and build, Frontend e2e (Playwright), and Docker images. These run for pushes, pull requests, and merge groups without path filters. The compiler jobs build with `make` and run the language suite (also with `--optimize`) on both toolchains; the sanitizer job builds an instrumented compiler and runs the suite under AddressSanitizer and UndefinedBehaviorSanitizer. Leak detection is disabled because the instruction graph is currently retained until process exit; leak checking is not covered by this job.

Repo hygiene rejects tracked files that `.gitignore` excludes, tracked environment/private-key files, installed dependencies, runtime workspace/job data (`server/workspace`, `server/_tmp`, `server/dist`), build output (`frontend/dist`, `compiler/build`), generated native binaries (by name or by ELF/PE/Mach-O header), and leaked secrets (gitleaks). Runtime workspaces remain private and untracked.
