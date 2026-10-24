# NovaComp — System Architecture & Technical Overview

NovaComp is a cloud IDE for **Nova**, a statically typed language for machine learning with a Python-like
syntax and built-in tensors, autograd and GPU-cluster training. This document describes how the system is
built. For running it locally see [Setup and verification](docs/SETUP.md); for production see
[Deployment](docs/DEPLOY.md); for the language itself see [Nova syntax](docs/SYNTAX.md); for what comes next
see the [Roadmap](docs/ROADMAP.md).

---

## 1. What Is NovaComp?

Users write Nova code in a browser-based editor. The backend compiles and runs it in a sandboxed,
network-less container and streams the output back. The platform also supports GPU-accelerated model
training on ASU's SOL HPC cluster, user accounts, cloud file storage, a per-user workspace with a terminal,
and an agentic AI coding assistant (NovaBot) powered by Claude via AWS Bedrock.

```python
class Linear:
    Tensor W
    Tensor b

    def init(int in_f, int out_f):
        self.W = xavier(in_f, out_f)
        self.b = zeros(1, out_f)

    def forward(Tensor x) -> Tensor:
        return x @ self.W + self.b

ignite:
    Linear layer = Linear(2, 4)
    Tensor y = sigmoid(layer.forward(ones(3, 2)))
    print(y.rows)
```

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        USER BROWSER                         │
│              React 19 + TypeScript + Vite (SPA)             │
└───────────────────────────┬─────────────────────────────────┘
                            │ HTTPS: REST / SSE / WebSocket
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                  AWS EC2 (Ubuntu)                           │
│  TLS proxy ──▶ server container (Node.js + Express)         │
│                 │                                           │
│                 │ docker run, one container per job         │
│                 ▼                                           │
│  ┌────────────────────────────┐  ┌──────────┐ ┌──────────┐ │
│  │ worker containers          │  │  AWS S3  │ │ DynamoDB │ │
│  │ the Nova compiler, sandboxed│  │  files   │ │ users,   │ │
│  │ (no network, read-only, cap │  └──────────┘ │ files,   │ │
│  │  drop, CPU/memory/time caps)│               │ NovaBot  │ │
│  └────────────────────────────┘  ┌──────────┐ └──────────┘ │
│                                  │ Bedrock  │              │
│                                  │ (Claude) │              │
│                                  └──────────┘              │
└───────────────────────────┬─────────────────────────────────┘
                            │ SSH tunnel (port 2223)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│              ASU SOL HPC GPU Cluster                        │
│         SLURM Job Scheduler → GPU Compute Nodes             │
│              (HuggingFace / PyTorch training)               │
└─────────────────────────────────────────────────────────────┘
```

The server and worker images are built from the same commit by CI, pushed to GHCR, and run on the EC2 host
with Docker Compose (`deploy/compose.prod.yml`).

---

## 3. Frontend (`frontend/`)

**Stack:** React 19 · TypeScript · Vite · Monaco Editor · xterm.js · Axios · Vitest · Playwright

### 3.1 Pages

| Page | File | Purpose |
|------|------|---------|
| Login / Sign Up | `src/pages/LoginPage.tsx` | Email + password; a JWT on success |
| IDE | `src/pages/IDEPage.tsx` | Editor, file sidebar, output, terminal, NovaBot |
| Share | `src/pages/SharePage.tsx` | Read-only view of a shared snippet |

### 3.2 IDE Layout

```
┌──────────────────── Header / Toolbar ─────────────────────────────────┐
│ NovaComp / file.nova  ✓   [Run] [Compare] [GPU*] [Share] [Settings] … │
├──────────┬────────────────────────────────┬───────────────────────────┤
│  File    │    Monaco editor               │   NovaBot                 │
│ sidebar  │    (Nova syntax, autosave)     │   AI coding assistant     │
│ (search) ├────────────────────────────────┤   streaming chat +        │
│          │  Output / Compare / GPU status │   tool events             │
│          │  Terminal (WebSocket)          │                           │
└──────────┴────────────────────────────────┴───────────────────────────┘
                                    * shown only when the server reports GPU as enabled
```

The IDE also has a command palette, resizable and responsive panels, persistent editor settings, starter
examples, and keyboard shortcuts.

### 3.3 State

There is no Redux or Zustand. `IDEPage` composes a few hooks, each owning one concern:

| Hook | Owns |
|------|------|
| `useWorkspace` | the file list, open file, drafts, autosave (with retry; a failed save keeps the draft) |
| `useRunner` | run / compare / streaming output |
| `useGpuJob` | GPU capability (`/api/capabilities`) and the GPU job lifecycle |
| `useEditorSettings` | editor preferences, persisted in `localStorage` |
| `useWorkspaceShortcuts` | keyboard shortcuts |

The layout is split into components under `src/components/workspace/` (header, toolbar, sidebar,
editor panel, status bar, dialogs). **Auth state** is global in `AuthContext`: the JWT lives in
`sessionStorage` (one tab, cleared when the tab closes), parsed by `lib/session.ts`.

### 3.4 API Layer (`src/api/`)

All requests go to `VITE_API_URL` (baked in at build time) through the shared Axios client in `client.ts`,
which attaches the token and logs out on `401`.

| File | Functions | Transport |
|------|-----------|-----------|
| `auth.ts` | sign up, log in | Axios |
| `compiler.ts` | run, run-stream, compare, compile bytecode, GPU submit / status / results, capabilities | Axios / `fetch` SSE |
| `files.ts` | list, load, save, delete | Axios |
| `share.ts` | share a snippet, load a shared one | Axios |
| `agent.ts` | NovaBot chat, history, memory | `fetch` SSE |

Streaming (`run-stream`, NovaBot chat) uses `fetch` with a `ReadableStream` to parse Server-Sent Events,
not `EventSource`, because the requests need a POST body.

---

## 4. Backend (`server/`)

**Stack:** Node.js 22 · Express 5 · JWT · bcryptjs · AWS SDK v3 · ws · ssh2

### 4.1 Structure

`src/server.js` starts the server on **port 3001**: it sweeps stale job containers left by an earlier run of
this instance, then listens for HTTP and WebSocket traffic. `src/app.js` installs the middleware in order
(origin check, CORS, JSON limit, static files, rate limits, auth gates) and mounts the routers in
`src/routes/`. `src/config.js` reads and validates the environment; in production it refuses to start
without explicit `https://` origins and a `NOVA_WORKER_ID`.

### 4.2 API Endpoints

Everything except sign-up, log-in, `/health` and reading a shared snippet requires `Authorization: Bearer <JWT>`.

| Area | Routes | Notes |
|------|--------|-------|
| Auth | `POST /auth/signup`, `POST /auth/login`, `GET /auth/me` | bcrypt passwords in DynamoDB; 1-hour JWTs; 10 requests/min |
| Execution | `POST /run`, `POST /run-stream`, `POST /compare`, `POST /compile-bytecode` | Run in a worker container; `run-stream` is SSE; `503` when execution capacity is full |
| Workspace | `POST /fs/write`, `DELETE /fs/delete`, `POST /fs/mkdir`, `POST /fs/rename`, `GET /fs/read`, `GET /fs/tree` | Per-user directory on the server; byte and entry quotas; 120 requests/min per user |
| Cloud files | `GET /files`, `POST /files/save`, `GET /files/load`, `DELETE /files/delete` | S3 content (`{userId}/…`), DynamoDB metadata |
| GPU / SOL | `POST /gpu/submit`, `GET /gpu/status/:jobId`, `GET /gpu/results/:jobId` | Only when GPU is enabled |
| NovaBot | `POST /agent/chat`, `GET`/`DELETE /agent/history`, `GET /agent/memory` | Tool-use loop streamed over SSE |
| Sharing | `POST /api/share`, `GET /api/share/:id` | Creating needs sign-in; reading does not |
| Other | `GET /api/capabilities`, `GET /health` | Capabilities (GPU on/off) for the UI; `/health` returns only `{ status: 'ok' }` |

WebSockets: `/repl` (an interactive Nova session) and `/terminal` (the workspace shell). Each must send
`{ type: 'auth', token }` as its first message; a token in the URL is rejected.

### 4.3 Code Execution

```
Browser POST /run-stream  (JWT)
        │
        ├── validate the source (length and content checks)
        ├── reserve an execution slot: 5 batch slots total, at most 2 per user
        │   (REPL sessions: a separate pool of 2, one per user) — none free → 503
        ├── write the source to a per-job directory
        ├── docker run --rm --network=none --read-only --cap-drop=ALL
        │       --security-opt=no-new-privileges --cpus=1 --memory=256m --pids-limit=32
        │       (+ 16 MiB file-size and open-file limits)
        │       timeout --signal=KILL <deadline>  /opt/nova/nova <source>
        │
        ├── stream stdout → SSE chunks to the browser (output size is capped)
        │       NOVA_SAVE_FILE: lines → the saved file is uploaded to the user's S3 storage
        ├── on exit: a "done" event with the elapsed time; a kill is reported as
        │   "timed out" or "out of memory"
        └── release the slot, remove the job directory
```

The deadline is enforced inside the container, so a job is stopped even if the server dies. `local`
execution mode (no Docker) exists for development and is refused in production.

### 4.4 AWS Infrastructure

| Service | Resource | Purpose |
|---------|----------|---------|
| S3 | `novacomp-files` | File content (`.nova`, `.nbc`, `.tar.gz` models), shared snippets |
| DynamoDB | `novacomp-users` | User accounts (email key, bcrypt hash) |
| DynamoDB | `novacomp-files-meta` | File metadata (userId + file path, updatedAt) |
| DynamoDB | `novacomp-agent-history` | NovaBot conversation history (30-day TTL) |
| DynamoDB | `novacomp-agent-memory` | NovaBot long-term memory per user |
| Bedrock | Claude via an inference profile | NovaBot: tool use and streaming |
| CloudWatch | `NovaComp/Bedrock` | Token usage per user |

### 4.5 Workspace Terminal

The terminal is a small built-in shell, not a system shell. It runs on the server, is confined to the user's
workspace directory, and supports `ls`, `cd`, `pwd`, `cat`, `touch`, `mkdir`, `rm`, `cp`, `mv`, `echo`,
`clear`, `help` and `novacomp` (compile and run a file in a worker container). Writes go through the same
quota-checked workspace store as the `/fs` API and are rate-limited per connection.

---

## 5. NovaBot — Agentic AI Coding Assistant

NovaBot is a Claude-powered coding assistant in the IDE's right panel. Its system prompt
(`server/src/agent/system-prompt.js`) teaches the current Nova syntax, and every complete example in it is
type-checked against the compiler. It can read, write and run code for the user.

### 5.1 Architecture

```
User message → POST /agent/chat
    │
    ├── Load conversation history and user memory from DynamoDB
    ├── Build a personalized system prompt (experience level, past patterns, preferences)
    │
    └── Agentic tool-use loop (max 5 rounds):
            ├── Bedrock InvokeModelWithResponseStream (Claude)
            ├── If stop_reason = tool_use:
            │     ├── run_nova_code    → compile & run in a worker container
            │     ├── read_user_file   → load from S3
            │     ├── write_user_file  → save to S3, emit file_written
            │     ├── list_user_files  → list from DynamoDB
            │     └── explain_error    → analyze a compiler error, suggest a fix
            └── Loop until stop_reason = end_turn or max rounds reached
    │
    ├── Save the turn to history, update user memory
    └── Stream SSE events to the browser
```

### 5.2 SSE Event Types

| Event | Payload | Description |
|-------|---------|-------------|
| `text` | `{ text }` | Streaming text from Claude |
| `tool_start` | `{ id, name }` | A tool call began |
| `tool_result` | `{ id, name, result }` | A tool call finished |
| `file_written` | `{ fileName }` | NovaBot wrote a file to the workspace |
| `done` | — | Turn complete |
| `error` | `{ error }` | Failure message |

### 5.3 User Memory

After each turn the server records the user's experience level (`beginner` / `intermediate` /
`advanced`), preferred topics, common mistakes and interaction preferences, and injects them into the system
prompt on later requests, so NovaBot adapts to each user without fine-tuning.

### 5.4 Bedrock Client (`server/src/agent/bedrock-client.js`)

Wraps `@aws-sdk/client-bedrock-runtime` behind an Anthropic-SDK-like interface (`createMessage()` for the
tool-use loop, `streamMessage()` for plain streaming). On EC2 it authenticates with the instance's IAM
role, so there are no API keys.

---

## 6. The Nova Compiler (`compiler/`)

Written in **C++17**; `make -C compiler` builds `compiler/build/nova`. The server runs it once per job inside
a worker container (or as an interactive `--repl` session).

### 6.1 Pipeline

```
Nova source
   │
   ▼
Lexer (frontend/lexer.cc)          tokens, then layout passes:
   │                               indentation → blocks, x += e → x = x + (e), def main() → entry point
   ▼
Modules (frontend/imports.cc)      orbit a.b → a/b.nova next to the file, else the standard library
   │
   ▼
Parser (frontend/parser*.cc)       one pass, type-checked, emits a linked-list IR (InstructionNode)
   │                               generic functions/classes are compiled once per type used
   ├── with --optimize: constant folding, copy propagation, dead-code elimination,
   │                    loop-invariant code motion, inlining   (opt/)
   ├── tensor-free pass: frees each tensor after its last use   (opt/tensor_release.cc)
   │
   ▼
Back ends
   ├── interpreter (default)                     runtime/
   ├── bytecode (.nbc, --emit-bytecode) + the standalone VM, novavm
   ├── x86-64 assembly (--emit-asm), with Chaitin-Briggs register allocation (regalloc/)
   ├── CUDA source (--emit-cuda)
   └── x86-64 JIT: experimental, off unless built with -DNOVA_JIT (it miscompiles general code)
```

Also: `--check` (type-check only), `--dump-ir`, `--debug` (step debugger), automatic memoization of pure
recursive functions.

### 6.2 The Language

The full reference is [docs/SYNTAX.md](docs/SYNTAX.md). In short:

- **Python-like layout:** a `:` and an indented block; `#` comments; no semicolons. The entry point is an
  `ignite:` block (or `def main()`); a file pulled in with `orbit` does not run its `ignite` block.
- **Static types, written first:** `int`, `float`, `double`, `bool`, `string`, `Tensor`; block-scoped
  variables; no shadowing.
- **Control flow:** `if` / `elif` / `else`, `while`, `do` … `while`, C-style `for`, `switch` / `case`,
  `break`, `continue`.
- **Functions:** `def f(int x) -> int`, several return values (`-> int, int`), and **generic functions** —
  a parameter without a type makes `def scale(x, k)` compile once per argument-type combination.
- **Classes and structs:** fields, methods, `init` constructors (`Linear l = Linear(2, 4)`), `extends`,
  and **generic classes** (`class Box<T>`, used as `Box<int>`).
- **Tensors (1–6 dimensions) and autograd:** `+ - * / @`, `t[i, j]`, `.T`, `.rows`, `.cols`, `.rank`,
  `zeros`, `ones`, `randn`, `xavier`, `reshape`, activations, losses, `requires_grad`, `backward`,
  `tensor_grad`.
- **Containers:** built-in `vector<T>` and `map<K, V>` with `push`, `pop`, indexing, `has`, `remove`,
  `sort`, `contains`, `index_of`, `binary_search`, `min`, `max`, `sum`.
- **Standard library (`compiler/stdlib/`):** `orbit std.math` (generic `abs`, `min`, `max`, `clamp`),
  `orbit novatorch.nn` / `novatorch.optim` / `novatorch.data`, `orbit collections`.
- **Integrations:** `hf.*` (HuggingFace datasets and models) and `sol.*` (training on the SOL cluster).

Containers are references: they can be passed to functions, returned and assigned (sharing one container).
They can also be class fields and generic-class type arguments (`Box<vector<int>>`). Objects are references
too: they can be passed, returned, assigned, stored in fields and containers (`vector<Node>`), and used as
type arguments (`Box<Layer>`). Known limit, being worked on: a container is never freed yet, so one created
in a loop keeps its elements alive.

---

## 7. SOL GPU Cluster Integration

### 7.1 How the Tunnel Works

```
Developer's machine (Windows)
   tools\sol-tunnel\Start-SolTunnel.ps1
   → ssh -NR 2223:login.sol.rc.asu.edu:22 ubuntu@<EC2-IP>
                        │
                        └── EC2 port 2223 → SOL login node port 22
```

The EC2 server SSHes into `localhost:2223`, which tunnels to SOL's login node, so no user ever handles SOL
credentials. `Start-SolTunnel.ps1` / `Stop-SolTunnel.ps1` run the SSH process as a hidden background job on
Windows and reconnect on failure.

### 7.2 Training Job Lifecycle

```
Nova: sol.finetune(model, task, dataset, epochs, lr, batch_size)
  ├── generate a Python training script (HuggingFace Trainer) and a SLURM batch script
  ├── upload both to SOL scratch over SFTP
  └── sbatch job.slurm → SLURM job ID

Nova: sol.wait(job)
  ├── poll squeue every 60 s: PENDING → "waiting for a GPU", RUNNING → live log excerpts
  └── DONE → training summary (loss, runtime, epochs)

  [On the SLURM compute node]
  ├── Trainer.train(), then tar -czf model.tar.gz
  └── curl PUT model.tar.gz → pre-signed S3 URL (straight to S3, not through EC2)

Nova: sol.predict(job, "text")   → runs an inference script on the SOL login node
Nova: sol.save(job, "name")      → registers the uploaded model; it appears in the file sidebar
```

### 7.3 Security Model

- The SOL SSH key exists **only on EC2** (`SOL_PRIVATE_KEY_PATH`); `sol.set_key()` is a no-op.
- Cluster paths and usernames are never shown to users or to NovaBot.
- Models go from the compute node straight to S3 through a pre-signed URL.
- GPU features are off unless enabled on the server; the UI hides them when they are off.

---

## 8. Security

| Area | Mechanism |
|------|-----------|
| Passwords | bcrypt |
| Sessions | JWT signed with `JWT_SECRET` (a short, low-variety or placeholder secret is refused at startup), 1-hour expiry, kept in `sessionStorage` |
| API | `requireAuth` checks `Authorization: Bearer <token>` |
| WebSockets | the token is sent in the first message (never in the URL); unauthenticated sockets are limited per IP and dropped after 5 s; session caps count authenticated sockets only; a session closes when its token expires |
| Origins | production requires an explicit `https://` allowlist (`NOVA_ALLOWED_ORIGINS`) |
| Proxies | `NOVA_TRUST_PROXY` (an address list or a hop count) so rate limits see the real client address |
| Execution | one network-less, read-only, capability-free container per job, with time, CPU, memory, PID and file-size limits; per-user and global slot quotas |
| Workspace | every path is resolved inside the user's directory (traversal and symlinks rejected); byte and entry quotas; rate limits on the HTTP API and the terminal |
| Files | S3 keys are prefixed with the user's ID |
| Rate limiting | `express-rate-limit` on auth, execution, sharing and NovaBot |
| Source | maximum length and content checks before compilation |
| Repository | CI runs gitleaks and a hygiene check that rejects tracked secrets, dependencies, runtime data and binaries |

---

## 9. End-to-End Data Flow

```
User types Nova code → Monaco editor
    ↓ [Run]
POST /run-stream (JWT) → slot reserved → worker container runs the compiler
    ↓
stdout streamed back over SSE
    ↓ (if the program calls sol.finetune)
the compiler reaches SOL through the tunnel → SLURM trains on a GPU → model uploaded to S3
    ↓
model registered → appears in the file sidebar

User asks NovaBot a question
    ↓
POST /agent/chat → history + memory loaded → Bedrock tool-use loop (runs code in workers, reads/writes files)
    ↓
text and tool events streamed over SSE; history and memory saved
```

---

## 10. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| One worker container per job | User code never runs in the server process; no network, read-only filesystem and hard limits contain bad or hostile programs |
| Deadline enforced inside the container | A job stops on time even if the server crashes or restarts |
| SSE for run output | The request needs a POST body; SSE over `fetch` streams it cleanly |
| WebSocket auth in the first message | Tokens in URLs end up in proxy and server logs |
| JWT in `sessionStorage`, 1-hour expiry | Limits how long a stolen token is useful and keeps it out of other tabs |
| Pre-signed S3 URL for model upload | Large models go from SLURM straight to S3, not through EC2 |
| SSH tunnel to SOL | SOL does not expose SSH publicly; the tunnel through EC2 is the viable path |
| Bedrock instead of the Anthropic API | IAM-role auth on EC2 (no keys), CloudWatch metrics |
| DynamoDB for agent history and memory | TTL support and it fits the existing AWS stack |
| Generics compiled per type (like C++ templates) | Untyped Nova code stays statically typed and runs at typed speed |
| One-pass compiler with a linked-list IR | Simple to extend; optimizations run as separate passes on the IR |
| No Redux / Zustand | A single page; hooks and React Context are enough |

---

## 11. Development

### 11.1 Prerequisites

- Node.js 22.12+ and `make`
- g++ or clang++ with C++17 (libssh2 optional: `sudo apt-get install libssh2-1-dev` enables SOL/SSH)
- Docker, for the sandboxed execution mode (local mode works without it)
- AWS credentials (or an EC2 IAM role) with S3, DynamoDB and Bedrock access, for the cloud features

### 11.2 Compiler

```bash
make -C compiler          # → compiler/build/nova
make -C compiler test     # the golden language tests in compiler/tests/
make -C compiler novavm   # the standalone bytecode VM
```

`NOVA_HOME` must point at `compiler/` (the directory holding `stdlib/`) when running `nova` by hand on a
file that uses `orbit`; the test runner and the server set it themselves.

### 11.3 Backend

```bash
cd server
npm ci
cp .env.example .env      # then fill in the values
npm start                 # uses ../compiler/build/nova and ../compiler/stdlib
npm test                  # unit and security tests (node:test)
npm run test:worker       # the Docker worker checks (needs Docker)
```

### 11.4 Frontend

```bash
cd frontend
npm ci
npm run dev               # development server
npm run build             # production build → frontend/dist (served by the backend)
npm run lint && npm run format:check && npm test && npm run test:e2e
```

### 11.5 SOL Tunnel

```powershell
.\tools\sol-tunnel\Start-SolTunnel.ps1   # hidden background process, reconnects automatically
.\tools\sol-tunnel\Stop-SolTunnel.ps1
```

### 11.6 CI and Deployment

Every push and pull request runs `.github/workflows/ci.yml`. Its 8 jobs are the checks `main` requires:
Repo hygiene, Backend unit tests, Compiler (g++), Compiler (clang++), Compiler (ASan + UBSan), Frontend
lint and build, Frontend e2e (Playwright), and Docker images. The language tests run twice in each compiler
job, with and without `--optimize`. When CI passes on `main`, `.github/workflows/deploy.yml` builds the
server and worker images, pushes them to GHCR and rolls them out to the EC2 host with
`deploy/deploy.sh` (see [docs/DEPLOY.md](docs/DEPLOY.md)).

---

## 12. Repository Structure

```
NovaComp/
├── compiler/                   # the Nova language (C++17)
│   ├── Makefile                # make / make test / make novavm → build/
│   ├── include/                # compiler.h (IR, types, ops), lexer.h, tensor_ops.h, autograd.h, optimizer.h,
│   │                           # regalloc.h, codegen.h, bytecode.h, integrations.h …
│   ├── src/
│   │   ├── driver/             # main.cc (CLI), repl.cc, debugger.cc, novavm_main.cc (bytecode VM)
│   │   ├── frontend/           # lexer, inputbuf, imports (orbit); parser.cc (functions, generics) +
│   │   │                       # parser_{state,util,classes,builtins,statements,expressions,containers,objects}.cc
│   │   ├── runtime/            # interpreter.cc (dispatch) + interp_{core,calls,tensor,containers,objects,integrations}.cc,
│   │   │                       # state.cc, memory.cc, tensor.cc, autograd.cc, memoize.cc
│   │   ├── opt/                # constant_fold, copy_propagation, dead_code, licm, inline, tensor_release
│   │   ├── regalloc/           # liveness.cc, coloring.cc (Chaitin-Briggs)
│   │   ├── codegen/            # x86_asm.cc, jit.cc, cuda.cc, bytecode.cc (.nbc)
│   │   ├── ir/                 # dump.cc (--dump-ir)
│   │   └── integrations/       # sol.cc (SOL over SSH), hf.cc (HuggingFace)
│   ├── stdlib/                 # std.math, novatorch.{nn,optim,data}, collections, math, assert (*.nova)
│   ├── tests/                  # golden tests (*.nova + *.expected / *.error) and run.js
│   └── benchmarks/             # Nova vs. C vs. Python
│
├── server/                     # Node.js + Express backend
│   ├── src/
│   │   ├── server.js           # entry point: container sweep, HTTP server, WebSockets
│   │   ├── app.js              # middleware, rate limits, auth gates, capabilities, routers
│   │   ├── config.js           # environment and deployment checks, execution limits
│   │   ├── paths.js            # filesystem locations (overridable by environment)
│   │   ├── routes/             # auth, run, fs, files, gpu, share, agent, health
│   │   ├── ws/                 # index.js (upgrade routing, first-message auth), repl.js, terminal.js
│   │   ├── middleware/auth.js  # requireAuth (JWT)
│   │   ├── services/           # execution (worker containers, quotas), compiler-runner, security,
│   │   │                       # workspace-store (quotas), aws, sol, agent-store, agent-tools
│   │   ├── agent/              # bedrock-client.js, system-prompt.js (NovaBot)
│   │   └── lib/logger.js       # structured JSON logs
│   ├── test/                   # node:test suites (execution, security, workspace)
│   ├── scripts/test-worker.js  # Docker worker checks
│   └── .env.example
│
├── frontend/                   # React + Vite app
│   ├── src/
│   │   ├── pages/              # IDEPage, LoginPage, SharePage
│   │   ├── components/         # NovaEditor, NovaBot, TerminalPanel, BottomPanel, CommandPalette, Modal,
│   │   │   └── workspace/      # header, toolbar, file sidebar, editor panel, status bar, dialogs
│   │   ├── hooks/              # useWorkspace, useRunner, useGpuJob, useEditorSettings, useWorkspaceShortcuts
│   │   ├── api/                # client (shared Axios), auth, compiler, files, share, agent
│   │   ├── contexts/           # AuthContext
│   │   ├── lib/                # session, workspace, terminalText, novaLanguage (Monaco syntax for Nova)
│   │   └── styles/
│   ├── tests/                  # Playwright end-to-end tests
│   └── vite.config.ts
│
├── examples/                   # Nova example programs + generate_mnist_data.py
├── docs/                       # SETUP.md, DEPLOY.md, SYNTAX.md, ROADMAP.md, REVIEW.md
├── deploy/                     # compose.prod.yml + deploy.sh for the EC2 host
├── tools/                      # sol-tunnel/ (Start/Stop-SolTunnel.ps1), syntax/ (source migration scripts)
├── scripts/check-repo-hygiene.js
├── .github/workflows/          # ci.yml (8 required checks), deploy.yml
└── Dockerfile                  # worker and server images
```
