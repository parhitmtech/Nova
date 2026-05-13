# NovaComp — System Architecture & Technical Overview

---

## 1. What Is NovaComp?

NovaComp is a cloud-hosted Integrated Development Environment (IDE) for a custom programming language called **Nova**. Users write Nova code in a browser-based editor, which is compiled and executed on a remote EC2 server. The platform also supports GPU-accelerated machine learning training on ASU's SOL HPC cluster, user authentication, cloud file storage, and real-time streaming output.

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
│   ┌─────────────┐    ┌──────────────┐    ┌──────────────┐  │
│   │  C++ Nova   │    │   AWS S3     │    │  DynamoDB    │  │
│   │  Compiler   │    │ (File Store) │    │ (Users + Meta│  │
│   └─────────────┘    └──────────────┘    └──────────────┘  │
└───────────────────────────┬─────────────────────────────────┘
                            │ SSH Reverse Tunnel (port 2223)
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
| IDE | `src/pages/IDEPage.tsx` | Main editor, file browser, output panel, terminal |
| Share | `src/pages/SharePage.tsx` | Read-only view of shared code snippets |

### 3.2 IDE Layout

```
┌──────────────────── Toolbar ─────────────────────────────────┐
│ NovaComp / filename.nova   [Save] [Run] [Compare] [GPU] ...  │
├──────────┬───────────────────────────┬────────────────────────┤
│          │                           │                        │
│  File    │    Monaco Code Editor     │   Output Panel         │
│ Browser  │    (Nova language syntax) │   (SSE stream / stats) │
│          │                           │                        │
│ sidebar  ├───────────────────────────┤                        │
│          │    Terminal (optional)    │   GPU Status           │
│          │    WebSocket PTY          │                        │
└──────────┴───────────────────────────┴────────────────────────┘
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

**Auth state** is global via `AuthContext` (React Context + `localStorage`), providing `token`, `user`, and `logout()` to any component.

### 3.4 API Layer (`src/api/`)

All API calls go to `VITE_API_URL` (set in `.env`, baked in at build time by Vite).

| File | Functions | Transport |
|------|-----------|-----------|
| `auth.ts` | `login()`, `register()` | Axios POST |
| `compiler.ts` | `runCode()`, `runCodeStream()`, `compareCode()`, `submitGpuJob()`, `getGpuStatus()`, `getGpuResults()`, `compileBytecode()` | Axios / `fetch` SSE |
| `files.ts` | `listFiles()`, `loadFile()`, `saveFile()`, `deleteFile()` | Axios |
| `share.ts` | `shareCode()`, `getSharedCode()` | Axios |

**Real-time streaming** (`runCodeStream`) uses the browser's `fetch` API with `ReadableStream` to parse Server-Sent Events line by line — not `EventSource`, because it needs a POST body. Each SSE message is `data: {"type":"stdout","text":"..."}`.

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

Files are namespaced per user: `{userId}/{fileName}` in S3.

### 4.5 WebSocket Terminal

`node-pty` spawns a real PTY shell on the EC2 server. The browser connects via WebSocket, and keystrokes are forwarded directly to the shell. This gives users a real bash terminal inside the IDE.

---

## 5. The Nova Compiler (`compiler.cc`)

Written in **C++**, compiled to a binary called `compiler`. The server spawns it as a child process for every run.

### Pipeline

```
Nova source code
      │
      ▼
   Lexer (lexer.cc)        → tokens
      │
      ▼
   Parser (parser2.cc)     → AST (InstructionNode tree)
      │
      ├── Constant Folding
      ├── Dead Code Elimination
      ├── Graph-Coloring Register Allocation
      │
      ▼
   Code Generation
      ├── Interpreter (tree-walk)
      ├── JIT (x86-64 machine code via mmap)
      └── Bytecode (bytecode.cc → .nbc files)
      │
      ▼
   Execution / Output → stdout (streamed back to server)
```

### Nova Language Features

- Statically typed with type inference
- `import hf` — HuggingFace dataset and model API
- `import sol` — SOL HPC GPU cluster API
- Built-in ML training syntax (`sol.finetune`, `sol.wait`, `sol.predict`, `sol.save`)
- JIT compilation (x86-64, Linux only)
- Bytecode export (`.nbc`)

---

## 6. SOL GPU Cluster Integration

The most architecturally complex feature.

### How the Tunnel Works

```
Developer's machine
   ssh -NR 2223:login.sol.rc.asu.edu:22 ubuntu@<EC2-IP>
                        │
                        └── EC2 port 2223 → SOL login node port 22
```

The EC2 server uses **libssh2** (in the C++ compiler binary) to SSH into `localhost:2223`, which tunnels to SOL's login node. No user ever touches SOL credentials directly.

### Training Job Lifecycle

```
Nova: sol.finetune(model, task, dataset, epochs, lr, batch_size)
  │
  ├── generateTrainScript() → Python training script (HuggingFace Trainer)
  ├── generateSlurmScript() → SLURM batch script (GPU partition, 2h limit)
  ├── SFTP upload both scripts to SOL scratch
  │       /scratch/pmathu14/novacomp_jobs/{jobId}/
  ├── ssh exec: sbatch job.slurm → returns SLURM job ID
  │
Nova: sol.wait(job)
  ├── Poll squeue every 60s
  ├── PENDING → "Training job in queue, waiting for GPU server..."
  ├── RUNNING → "GPU server acquired!" + live log snippet every 60s
  └── DONE    → show training summary (loss, runtime, epochs)
  │
  [On SLURM compute node, train.py runs:]
  ├── pip install (skipped if packages already cached)
  ├── HuggingFace Trainer.train()
  ├── Save model (safetensors → pytorch_model.bin conversion)
  ├── tar -czf model.tar.gz
  └── curl PUT model.tar.gz → pre-signed S3 URL
            (direct upload, bypasses EC2 entirely)
  │
Nova: sol.predict(job, "text")
  └── SSH exec Python inference script on SOL login node

Nova: sol.save(job, "name.nbc")
  └── Reads SLURM log for NOVA_MODEL_UPLOADED marker
      → emits NOVA_S3_FILE: for server to register in DynamoDB
      → model appears in user's file browser
```

### Security Model

- SOL SSH key is **only on EC2** as an environment variable (`SOL_PRIVATE_KEY_PATH`)
- `sol.set_key()` is a **no-op** in the compiler — users cannot redirect to a different key
- Model upload goes directly SLURM → S3 via pre-signed URL (24h expiry) — EC2 never handles the 200MB payload

---

## 7. Authentication & Authorization

| Layer | Mechanism |
|-------|-----------|
| Password storage | bcrypt (cost factor 10) |
| Session token | JWT signed with `JWT_SECRET`, 24h expiry |
| API protection | `authMiddleware` checks `Authorization: Bearer <token>` header |
| File isolation | S3 keys prefixed with `userId` — users can only access their own files |
| Rate limiting | express-rate-limit on all endpoints |
| Source validation | Max length + dangerous pattern checks before compilation |

---

## 8. End-to-End Data Flow

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
```

---

## 9. Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| SSE over WebSocket for run output | POST body needed for source code; SSE over `fetch` handles this cleanly |
| Pre-signed S3 URL for model upload | Avoids routing 200MB through EC2 — SLURM node uploads directly to S3 |
| Reverse SSH tunnel instead of VPN | SOL does not expose SSH publicly; tunnel through EC2 is the only viable path |
| libssh2 in C++ compiler | Lets the compiler binary manage SSH/SFTP directly without a Node.js intermediary |
| DynamoDB + S3 for files | S3 for content (cheap, scalable), DynamoDB for metadata (fast list and lookup) |
| JWT in localStorage | Stateless auth; simple for a single-server deployment |
| No Redux / Zustand | State scope is a single page; React Context + `useState` is sufficient |

---

## 10. Local Development Setup

### Prerequisites

- Node.js 18+
- g++ with libssh2 (`sudo apt-get install libssh2-1-dev`)
- AWS credentials configured
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

### SOL Tunnel (must be running for GPU features)

```powershell
while ($true) {
    ssh -i "~/.ssh/Nova-Key.pem" -NR 2223:login.sol.rc.asu.edu:22 ubuntu@<EC2-IP>
    Start-Sleep 5
}
```

---

## 11. Repository Structure

```
NovaComp/
├── Compiler_PJ_3/
│   ├── compiler.cc         # Nova compiler (lexer → parser → codegen → JIT)
│   ├── compiler.h
│   ├── parser2.cc
│   ├── lexer.cc / lexer.h
│   ├── bytecode.cc / .h    # Bytecode VM
│   ├── autograd.cc / .h    # Autograd engine
│   ├── server.js           # Express backend
│   ├── package.json
│   ├── .env                # AWS keys, JWT secret, SOL config
│   └── dist/               # Built React app (served as static files)
│
└── novacomp-frontend/
    ├── src/
    │   ├── pages/
    │   │   ├── IDEPage.tsx
    │   │   ├── LoginPage.tsx
    │   │   └── SharePage.tsx
    │   ├── api/
    │   │   ├── compiler.ts
    │   │   ├── files.ts
    │   │   ├── auth.ts
    │   │   └── share.ts
    │   ├── contexts/
    │   │   └── AuthContext.tsx
    │   ├── components/
    │   │   └── TerminalPanel.tsx
    │   └── lib/
    │       └── novaLanguage.ts   # Monaco syntax highlighting for Nova
    ├── .env                      # VITE_API_URL
    └── vite.config.ts
```
