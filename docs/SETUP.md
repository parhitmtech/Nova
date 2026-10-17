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

An empty API URL uses the same origin; Vite proxies API and terminal requests to localhost:3001. Set `NOVA_ALLOWED_ORIGINS` to the exact frontend origins. Production behind a TLS proxy must include the public HTTPS origin explicitly. CORS and WebSocket checks share this list.

For trusted local programs only, set `NOVA_EXECUTION_MODE=local`. Production rejects local execution. Local mode removes backend credentials from the process environment and applies time/output/concurrency limits, but does **not** provide operating-system filesystem, memory, or network isolation.

## Restricted execution

The worker has no network, a read-only root filesystem, no capabilities, no new privileges, and a non-root UID. Each job receives only its own temporary directory, one CPU, 256MB RAM, 32 processes, a 16MB maximum file size, and a bounded temporary filesystem. Program output is capped at 256KB. Web execution is limited to 30 seconds; `// @ml` cannot increase this. At most five compiler workers run per backend process. A stopped stream kills the process group and removes its container.

Every web-triggered compiler path uses `server/src/services/execution.js`. Backend AWS/JWT/SSH credentials are not copied into worker environments. Program-emitted file markers may upload only regular, non-symlink files inside that job's directory, with bounded file size and count.

GPU and provider execution remain disabled in the secure production configuration. The existing GPU adapter is available only with **both** `NOVA_ENABLE_GPU=true` and explicit trusted local mode in a nonproduction environment; SOL support also requires its optional SSH dependencies and credentials. A separate authenticated provider worker is needed before exposing GPU jobs publicly. Shared debug and Git endpoints report unavailable until they have isolated user sessions.

## Production builds

Build the frontend with `npm run build`. When running Node directly on a Linux host with Docker, the server serves `frontend/dist` by default (set `NOVA_STATIC_DIR` to serve a build from elsewhere). Serve only that build; backend source and workspace directories are private.

`docker build --target server -t novacomp-server:local .` also produces a non-root server image containing the frontend and Docker client. When using it, mount the jobs directory at the **same absolute path on the host and in the server** (`/var/lib/novacomp/jobs`) because the host daemon resolves worker bind mounts. Give it UID 1000 ownership. Configure socket access for the server's user, or use a dedicated worker host. Docker daemon access is a privileged operator capability; use a dedicated execution host with minimal AWS privileges rather than sharing it with unrelated workloads. Never mount the daemon socket or credentials into program workers.

Set `NODE_ENV=production`, supply a new deployment secret through your secret manager, configure the public origin, and use HTTPS. Environment files and keys are excluded from the Docker build context.

The local signing secret was rotated during this change. Rotating the live deployment secret remains an operator step. The previous secret still exists in old Git commits: after coordinating with collaborators, scrub those commits and any forks/caches that held it. Removing tracking alone does not remove historical values. No history was rewritten automatically.

## Tests

```sh
make -C compiler test     # build + golden language tests (compiler/tests/*.nova)

cd server
npm test                 # real HTTP/WebSocket security + execution tests
npm run test:worker      # real production Docker worker smoke test

cd ../frontend
npm run lint
npm run build
npx playwright install chromium   # once, if no local Chrome is available
npm run test:e2e
```

Browser tests exercise the real React/Monaco UI against controlled API responses. They verify keyboard settings, file-switch saves, save-error recovery, stdin, disabled GPU feedback, binary safeguards, and mobile navigation. They do not contact AWS or SOL. GitHub Actions runs builds, all these tests, and both container builds.

Keyboard shortcuts: Ctrl/Cmd+S saves, Ctrl/Cmd+Enter runs, Ctrl/Cmd+K opens commands. Dialogs support Tab/Shift+Tab and Escape. Files are saved before switching, failed saves keep the draft open, and binary model/bytecode artifacts cannot enter autosave. Binary downloads need a dedicated authorized backend endpoint and are not represented as a working feature.
