# NovaComp — System Architecture & Technical Overview

---

## 1. What Is NovaComp?

NovaComp is a cloud-hosted Integrated Development Environment (IDE) for a custom programming language called **Nova**. Users write Nova code in a browser-based editor, which is compiled and executed on a remote EC2 server. The platform also supports GPU-accelerated machine learning training on ASU's SOL HPC cluster, user authentication, cloud file storage, real-time streaming output, and an agentic AI coding assistant powered by Claude via AWS Bedrock.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        USER BROWSER                         │
│              React + TypeScript + Vite (SPA)                │
└───────────────────────────┬─────────────────────────────────┘
                            │ HTTP / SSE / WebSocket
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                  AWS EC2 (Ubuntu Server)                     │
│              Node.js + Express  (server.js)                  │
│                                                             │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────────┐  │
│  │  C++ Nova   │  │   AWS S3     │  │     DynamoDB       │  │
│  │  Compiler   │  │ (File Store) │  │ Users, Files,      │  │
│  └─────────────┘  └──────────────┘  │ Agent History,     │  │
│                                     │ Agent Memory       │  │
│  ┌──────────────────────────────┐   └────────────────────┘  │
│  │  AWS Bedrock (Claude)        │                           │
│  │  Agentic tool-use loop       │                           │
│  └──────────────────────────────┘                           │
└───────────────────────────┬─────────────────────────────────┘
                            │ SSH Tunnel (port 2223)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│              ASU SOL HPC GPU Cluster                        │
│         SLURM Job Scheduler → GPU Compute Nodes             │
│              (DistilBERT / HuggingFace Training)            │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Frontend

**Stack:** React 18 · TypeScript · Vite · Monaco Editor · TailwindCSS · Axios

### 3.1 Pages

| Page | File | Purpose |
|------|------|---------|
| Login / Sign Up | `src/pages/LoginPage.tsx` | Tabbed auth form — email + password, JWT on success |
| IDE | `src/pages/IDEPage.tsx` | Main editor, file browser, output panel, terminal, NovaBot |
| Share | `src/pages/SharePage.tsx` | Read-only view of shared code snippets |

### 3.2 IDE Layout

```
┌──────────────────── Toolbar ──────────────────────────────────────────┐
│ NovaComp / filename.nova  ✓  [Save] [Run] [Compare] [GPU] [Share] ... │
├──────────┬────────────────────────────────┬───────────────────────────┤
│          │                                │                           │
│  File    │    Monaco Code Editor          │   NovaBot                 │
│ Browser  │    (Nova language syntax)      │   AI Coding Assistant     │
│          │    Auto-saves to S3            │   (Claude via Bedrock)    │
│ sidebar  ├────────────────────────────────┤                           │
│          │  Output / Compare / GPU status │   Tool events + streaming │
│          │  Terminal (WebSocket PTY)       │   chat history            │
└──────────┴────────────────────────────────┴───────────────────────────┘
```

### 3.3 State Management

All state lives in `IDEPage` — no Redux or Zustand. Key state variables:

| State | Type | Purpose |
|-------|------|---------|
| `code` | string | Current editor content |
| `files` | FileEntry[] | Sidebar file list from S3/DynamoDB |
| `currentFile` | string | Open file name |
| `output` | OutputState | Run/compare results |
| `loading` | boolean | Spinner during execution |
| `token` | string | JWT from AuthContext |
| `gpuStatus` | enum | GPU job lifecycle state |
| `autoSaveStatus` | `'idle'⎮'saving'⎮'saved'` | Auto-save indicator in toolbar |

**Auto-save:** 2 seconds after the user stops typing, the editor content is silently persisted to S3. A spinner then green checkmark (✓) appears next to the filename in the toolbar. Content is fully restored on page reload via `localStorage` + S3.

**Auth state** is global via `AuthContext` (React Context + `localStorage`), providing `token`, `user`, and `logout()` to any component.

### 3.4 API Layer (`src/api/`)

All API calls go to `VITE_API_URL` (set in `.env`, baked in at build time by Vite).

| File | Functions | Transport |
|------|-----------|-----------|
| `auth.ts` | `login()`, `register()` | Axios POST |
| `compiler.ts` | `runCode()`, `runCodeStream()`, `compareCode()`, `submitGpuJob()`, `getGpuStatus()`, `getGpuResults()`, `compileBytecode()` | Axios / `fetch` SSE |
| `files.ts` | `listFiles()`, `loadFile()`, `saveFile()`, `deleteFile()` | Axios |
| `share.ts` | `shareCode()`, `getSharedCode()` | Axios |
| `agent.ts` | `sendAgentMessage()`, `loadAgentHistory()`, `clearAgentHistory()`, `loadAgentMemory()` | `fetch` SSE |

**Real-time streaming** (`runCodeStream`, `sendAgentMessage`) uses the browser's `fetch` API with `ReadableStream` to parse Server-Sent Events line by line — not `EventSource`, because it needs a POST body.

---

## 4. Backend

**Stack:** Node.js · Express 5 · JWT · bcryptjs · AWS SDK v3 · ssh2 · node-pty · WebSocket

### 4.1 Server Entry Point (`Compiler_PJ_3/server.js`)

The server runs on **port 3001** and serves:
- Static files from `/dist` (the built React app)
- All REST API endpoints
- A WebSocket server for the terminal

### 4.2 API Endpoints

#### Auth

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/register` | Hash password with bcrypt, store in DynamoDB `novacomp-users` |
| POST | `/login` | Verify password, return signed JWT (24h expiry) |

#### Code Execution

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/run` | Buffered execution — spawns compiler binary, returns full stdout when done |
| POST | `/run-stream` | **Streaming execution** — SSE, sends stdout chunks in real time. Intercepts `NOVA_SAVE_FILE:` and `NOVA_S3_FILE:` markers to upload files to S3 |
| POST | `/compare` | Runs code twice (normal + optimized AST), returns timing comparison |
| POST | `/compile-bytecode` | Returns compiled `.nbc` bytecode as binary blob |

#### File Storage

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/files` | List user's files from DynamoDB (`novacomp-files-meta` table) |
| GET | `/files/:name` | Fetch file content from S3 (`novacomp-files` bucket) |
| POST | `/files/:name` | Upload file content to S3, register in DynamoDB |
| DELETE | `/files/:name` | Remove from S3 and DynamoDB |

#### GPU / SOL HPC

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/gpu/submit` | Detects job type (CUDA / HF / SOL), submits accordingly |
| GET | `/gpu/status/:jobId` | Returns job state |
| GET | `/gpu/results/:jobId` | Returns job output |

#### AI Agent (NovaBot)

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/agent/chat` | Agentic tool-use loop — streams SSE events (`text`, `tool_start`, `tool_result`, `file_written`, `done`, `error`) |
| GET | `/agent/history` | Load full conversation history from DynamoDB |
| DELETE | `/agent/history` | Clear conversation history |
| GET | `/agent/memory` | Retrieve per-user memory (experience level, preferences, patterns) |

#### Sharing

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/share` | Saves code snippet to S3 with UUID, returns shareable URL |
| GET | `/share/:id` | Fetches shared snippet |

### 4.3 Code Execution Flow (Run-Stream)

```
Browser POST /run-stream
        │
        ├── Validate source (length, content checks)
        ├── Optional JWT auth (for file uploads)
        ├── Generate pre-signed S3 URL (if authenticated, for model upload)
        ├── Write source to temp file
        ├── Spawn: ./compiler <tempfile>
        │         env: NOVA_MODEL_UPLOAD_URL, NOVA_MODEL_S3_KEY
        │
        ├── Stream stdout → SSE chunks to browser
        │         intercept NOVA_SAVE_FILE: → upload local file to S3
        │         intercept NOVA_S3_FILE:  → register S3 key in DynamoDB
        │
        ├── On close: send done event with elapsed ms
        └── Cleanup temp file
```

### 4.4 AWS Infrastructure

| Service | Resource | Purpose |
|---------|----------|---------|
| S3 | `novacomp-files` | File content (`.nova`, `.nbc`, `.tar.gz` models) |
| DynamoDB | `novacomp-users` | User accounts (email PK, hashed password) |
| DynamoDB | `novacomp-files-meta` | File metadata (userId + filePath composite key, updatedAt) |
| DynamoDB | `novacomp-agent-history` | Per-user conversation history (TTL: 30 days) |
| DynamoDB | `novacomp-agent-memory` | Per-user long-term memory (experience level, preferences, observed patterns) |
| Bedrock | `us.anthropic.claude-sonnet-4-6` | LLM for NovaBot — tool use + streaming via inference profile |
| CloudWatch | `NovaComp/Bedrock` namespace | Token usage metrics per user (input/output tokens per request) |

Files are namespaced per user: `{userId}/{fileName}` in S3.

### 4.5 WebSocket Terminal

`node-pty` spawns a real PTY shell on the EC2 server. The browser connects via WebSocket, and keystrokes are forwarded directly to the shell. This gives users a real bash terminal inside the IDE.

---

## 5. NovaBot — Agentic AI Coding Assistant

NovaBot is a Claude-powered coding assistant embedded in the IDE right panel. It understands the Nova language deeply and can autonomously read, write, and execute code on behalf of the user.

### 5.1 Architecture

```
User message → POST /agent/chat
    │
    ├── Load conversation history from DynamoDB
    ├── Load user memory from DynamoDB
    ├── Build personalized system prompt
    │       (injects experience level, past patterns, preferences)
    │
    └── Agentic tool-use loop (max 5 rounds):
            │
            ├── Bedrock InvokeModelWithResponseStream (Claude)
            │
            ├── If stop_reason = tool_use:
            │     ├── run_nova_code    → compile & run, stream output
            │     ├── read_user_file  → load from S3
            │     ├── write_user_file → save to S3, emit file_written SSE
            │     ├── list_user_files → list from DynamoDB
            │     └── explain_error   → analyze compiler error + suggest fix
            │
            └── Loop until stop_reason = end_turn or max rounds reached
    │
    ├── Save assistant turn to DynamoDB history
    ├── Update user memory (patterns, level, preferences)
    └── Stream SSE events to browser
```

### 5.2 SSE Event Types

| Event | Payload | Description |
|-------|---------|-------------|
| `text` | `{ text }` | Streaming text chunk from Claude |
| `tool_start` | `{ id, name }` | A tool call has begun |
| `tool_result` | `{ id, name, result }` | Tool completed with output |
| `file_written` | `{ fileName }` | NovaBot wrote a file to the user's workspace |
| `done` | — | Turn complete |
| `error` | `{ error }` | Failure message |

### 5.3 User Memory

After each conversation turn, the server extracts and persists:
- **Experience level** (`beginner` / `intermediate` / `advanced`)
- **Preferred topics** (e.g. ML, algorithms, tensor ops)
- **Common mistakes** observed in the user's code
- **Interaction preferences** (verbosity, code-first vs. explanation-first)

This memory is injected into the system prompt on every subsequent request, making NovaBot progressively more tailored to each user.

### 5.4 NovaBot Tools

| Tool | Description |
|------|-------------|
| `run_nova_code` | Execute Nova source code and return stdout/stderr |
| `read_user_file` | Read a file from the user's S3 workspace |
| `write_user_file` | Write or update a file in the user's S3 workspace |
| `list_user_files` | List all files in the user's workspace |
| `explain_error` | Analyze a Nova compiler/runtime error and suggest a fix |

### 5.5 Bedrock Client (`bedrock_client.js`)

Wraps `@aws-sdk/client-bedrock-runtime` to match the Anthropic SDK interface:
- `createMessage()` — non-streaming, used for the tool-use loop
- `streamMessage()` — streaming text, used for simple Q&A
- Uses IAM role auth on EC2 — no API keys needed

---

## 6. The Nova Compiler (`compiler.cc`)

Written in **C++**, compiled to a binary called `compiler`. The server spawns it as a child process for every run.

### Pipeline

```
Nova source code
      │
      ▼
   Lexer (lexer.cc)              → tokens
      │
      ▼
   Parser (parser2.cc)           → AST (InstructionNode tree)
      │
      ├── Constant Folding
      ├── Dead Code Elimination (DCE)
      ├── Loop Invariant Code Motion (LICM)
      ├── Chaitin-Briggs Graph-Coloring Register Allocation
      │
      ▼
   Code Generation
      ├── Interpreter (tree-walk)
      ├── JIT (x86-64 machine code, mmap + SEH deoptimization fallback)
      ├── x86-64 native codegen via NASM
      └── Bytecode (bytecode.cc → .nbc files)
      │
      ▼
   Execution / Output → stdout (streamed back to server)
```

### Nova Language Features

- Statically typed: `int`, `float`, `double`, `string`, `tensor`
- Control flow: `if/elif/else`, `while`, `do-while`, `for` (C-style), `switch/case`
- Functions with single and multiple return values
- Classes with inheritance (`extends`), `self`, `init` constructor
- Structs (lightweight value types)
- Arrays: `array(n)` with runtime bounds checking
- Automatic memoization for pure recursive functions
- Type casting: `int(x)`, `float(x)`, `double(x)`
- Built-in tensor operations (30+): creation, arithmetic, activations, autograd, loss functions
- `import hf` — HuggingFace dataset and model API
- `import sol` — SOL HPC GPU cluster API
- JIT compilation (x86-64, Chaitin-Briggs register allocation, K=12)
- Bytecode export (`.nbc`)

---

## 7. SOL GPU Cluster Integration

The most architecturally complex feature.

### How the Tunnel Works

```
Developer's machine (Windows)
   .\Start-SolTunnel.ps1
   → ssh -NR 2223:login.sol.rc.asu.edu:22 ubuntu@<EC2-IP>
                        │
                        └── EC2 port 2223 → SOL login node port 22
```

The EC2 server SSHes into `localhost:2223`, which tunnels to SOL's login node. No user ever touches SOL credentials directly. The tunnel is managed by `Start-SolTunnel.ps1` / `Stop-SolTunnel.ps1` which run the SSH process as a hidden background job on Windows, auto-reconnecting on failure.

### Training Job Lifecycle

```
Nova: sol.finetune(model, task, dataset, epochs, lr, batch_size)
  │
  ├── generateTrainScript() → Python training script (HuggingFace Trainer)
  ├── generateSlurmScript() → SLURM batch script (GPU partition, 2h limit)
  ├── SFTP upload both scripts to SOL scratch
  ├── ssh exec: sbatch job.slurm → returns SLURM job ID
  │
Nova: sol.wait(job)
  ├── Poll squeue every 60s
  ├── PENDING → "Training job in queue, waiting for GPU server..."
  ├── RUNNING → "GPU server acquired!" + live log snippet every 60s
  └── DONE    → show training summary (loss, runtime, epochs)
  │
  [On SLURM compute node, train.py runs:]
  ├── HuggingFace Trainer.train()
  ├── Save model → tar -czf model.tar.gz
  └── curl PUT model.tar.gz → pre-signed S3 URL (direct, bypasses EC2)
  │
Nova: sol.predict(job, "text")
  └── SSH exec Python inference script on SOL login node

Nova: sol.save(job, "name")
  └── Reads SLURM log for NOVA_MODEL_UPLOADED marker
      → emits NOVA_S3_FILE: for server to register in DynamoDB
      → model appears in user's file browser
```

### Security Model

- SOL SSH key is **only on EC2** (`SOL_PRIVATE_KEY_PATH`)
- `sol.set_key()` is a **no-op** in the compiler — users cannot redirect to a different key
- Internal cluster paths and usernames are never exposed to users or NovaBot
- Model upload goes directly SLURM → S3 via pre-signed URL (24h expiry) — EC2 never handles the payload

---

## 8. Authentication & Authorization

| Layer | Mechanism |
|-------|-----------|
| Password storage | bcrypt (cost factor 10) |
| Session token | JWT signed with `JWT_SECRET`, 24h expiry |
| API protection | `authMiddleware` checks `Authorization: Bearer <token>` header |
| File isolation | S3 keys prefixed with `userId` — users can only access their own files |
| Rate limiting | express-rate-limit on all endpoints |
| Source validation | Max length + dangerous pattern checks before compilation |

---

## 9. End-to-End Data Flow

```
User types Nova code → Monaco Editor (browser)
    ↓ [Run clicked]
POST /run-stream  (with JWT token)
    ↓
Server validates + spawns compiler binary
    ↓
Compiler executes Nova → stdout streamed via SSE to browser
    ↓ (if sol.finetune present)
Compiler SSHes through tunnel → uploads SLURM job to SOL
    ↓
SOL GPU trains model → uploads model.tar.gz directly to S3
    ↓
Server registers model in DynamoDB → sends 'done' SSE event
    ↓
Frontend fetchFiles() → new model appears in file browser

User asks NovaBot a question
    ↓
POST /agent/chat → load history + memory from DynamoDB
    ↓
Bedrock tool-use loop → Claude calls tools autonomously
    ↓
SSE stream: text chunks + tool_start/tool_result events → browser
    ↓
History + memory saved back to DynamoDB
```

---

## 10. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| SSE over WebSocket for run output | POST body needed for source code; SSE over `fetch` handles this cleanly |
| Pre-signed S3 URL for model upload | Avoids routing 200MB through EC2 — SLURM node uploads directly to S3 |
| SSH tunnel instead of VPN | SOL does not expose SSH publicly; tunnel through EC2 is the only viable path |
| Bedrock over direct Anthropic API | IAM role auth on EC2 — no API keys to manage; CloudWatch metrics for free |
| DynamoDB for agent history + memory | TTL support for auto-expiry; fits the existing AWS stack; no extra infrastructure |
| Agentic tool-use loop (max 5 rounds) | Lets Claude autonomously run, fix, and re-run code without user intervention |
| Per-user memory injection | Makes NovaBot progressively more useful without fine-tuning |
| Auto-save debounce (2s) | Eliminates data loss on page reload without spamming S3 on every keystroke |
| DynamoDB + S3 for files | S3 for content (cheap, scalable), DynamoDB for metadata (fast list and lookup) |
| JWT in localStorage | Stateless auth; simple for a single-server deployment |
| No Redux / Zustand | State scope is a single page; React Context + `useState` is sufficient |

---

## 11. Local Development Setup

### Prerequisites

- Node.js 18+
- g++ with libssh2 (`sudo apt-get install libssh2-1-dev`)
- AWS credentials configured (or EC2 IAM role with S3, DynamoDB, Bedrock access)
- SSH key for SOL at `/home/ubuntu/.ssh/sol_key`

### Backend

```bash
cd Compiler_PJ_3
npm install
g++ -O2 -o compiler compiler.cc lexer.cc inputbuf.cc parser2.cc bytecode.cc autograd.cc -lssh2
node server.js
```

### Frontend

```bash
cd novacomp-frontend
npm install
npm run dev       # development
npm run build     # production build → dist/
```

### SOL Tunnel

```powershell
# Start (runs as hidden background process, auto-reconnects)
.\Start-SolTunnel.ps1

# Stop
.\Stop-SolTunnel.ps1
```

---

## 12. Repository Structure

```
NovaComp/
├── Start-SolTunnel.ps1         # SOL tunnel manager (Windows background process)
├── Stop-SolTunnel.ps1
│
├── Compiler_PJ_3/
│   ├── compiler.cc             # Nova compiler (lexer → parser → codegen → JIT)
│   ├── compiler.h
│   ├── parser2.cc
│   ├── lexer.cc / lexer.h
│   ├── bytecode.cc / .h        # Bytecode VM + .nbc format
│   ├── autograd.cc / .h        # Reverse-mode autograd engine
│   ├── Cuda_ffi.cc / .h        # CUDA runtime FFI layer
│   ├── server.js               # Express backend
│   ├── bedrock_client.js       # AWS Bedrock wrapper (createMessage, streamMessage)
│   ├── nova_system_prompt.js   # NovaBot system prompt + tool schemas
│   ├── package.json
│   ├── .env                    # AWS config, JWT secret, SOL config, Bedrock model ID
│   └── dist/                   # Built React app (served as static files)
│
└── novacomp-frontend/
    ├── src/
    │   ├── pages/
    │   │   ├── IDEPage.tsx     # Main IDE (editor, file browser, NovaBot, auto-save)
    │   │   ├── LoginPage.tsx
    │   │   └── SharePage.tsx
    │   ├── api/
    │   │   ├── compiler.ts
    │   │   ├── files.ts
    │   │   ├── auth.ts
    │   │   ├── share.ts
    │   │   └── agent.ts        # NovaBot API (chat, history, memory)
    │   ├── components/
    │   │   ├── NovaBot.tsx     # AI assistant panel (streaming, tool events, memory badge)
    │   │   └── TerminalPanel.tsx
    │   ├── contexts/
    │   │   └── AuthContext.tsx
    │   └── lib/
    │       └── novaLanguage.ts # Monaco syntax highlighting for Nova
    ├── .env                    # VITE_API_URL
    └── vite.config.ts
```
