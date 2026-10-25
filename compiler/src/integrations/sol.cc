// SOL HPC cluster: SSH/SFTP (libssh2) and Slurm/training script generation. Split out of compiler.cc.
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include "lexer.h"
#include "compiler.h"
#include "integrations.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifdef __has_include
#  if __has_include(<libssh2.h>)
#    include <libssh2.h>
#    include <libssh2_sftp.h>
#    define NOVA_HAVE_LIBSSH2 1
#  endif
#endif
#endif

using namespace std;

// SOL connection settings (key path and username can come from the environment).
static string sol_key_path = "";
static string sol_host     = "localhost";
[[maybe_unused]] static int sol_port = 2223;  // used only by the libssh2 build
static string sol_username = getenv("SOL_USERNAME") ? getenv("SOL_USERNAME") : "";

// ── SOL cluster helpers ───────────────────────────────────────────────────────

string sol_trim(const string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void sol_write_file(const string& path, const string& content)
{
    ofstream f(path);
    f << content;
}

string sol_extract_slurm_id(const string& sbatchOut)
{
    // "Submitted batch job 12345" → "12345"
    string s = sol_trim(sbatchOut);
    size_t pos = s.rfind(' ');
    return (pos != string::npos) ? s.substr(pos + 1) : s;
}

#ifdef NOVA_HAVE_LIBSSH2

static string sol_get_key()
{
    if (!sol_key_path.empty()) return sol_key_path;
    const char* env = getenv("SOL_PRIVATE_KEY_PATH");
    return env ? string(env) : "";
}

static int sol_open_socket()
{
    struct sockaddr_in sin;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((uint16_t)sol_port);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(sock, (struct sockaddr*)&sin, sizeof(sin)) != 0) {
        close(sock); return -1;
    }
    return sock;
}

static LIBSSH2_SESSION* sol_open_session(int sock)
{
    static bool libssh2_ready = false;
    if (!libssh2_ready) { libssh2_init(0); libssh2_ready = true; }

    LIBSSH2_SESSION* s = libssh2_session_init();
    if (!s) return nullptr;
    if (libssh2_session_handshake(s, sock) != 0) {
        libssh2_session_free(s); return nullptr;
    }
    string key = sol_get_key();
    if (key.empty()) {
        fprintf(stderr, "Nova: SOL private key not configured. Set SOL_PRIVATE_KEY_PATH on the server.\n");
        libssh2_session_free(s); return nullptr;
    }
    if (libssh2_userauth_publickey_fromfile(
            s, sol_username.c_str(), nullptr, key.c_str(), nullptr) != 0) {
        fprintf(stderr, "Nova: SOL SSH auth failed — check key path: %s\n", key.c_str());
        libssh2_session_free(s); return nullptr;
    }
    return s;
}

static void sol_close_session(LIBSSH2_SESSION* s, int sock)
{
    libssh2_session_disconnect(s, "Normal shutdown");
    libssh2_session_free(s);
    close(sock);
}

string sshExecSOL(const string& command)
{
    int sock = sol_open_socket();
    if (sock < 0) {
        fprintf(stderr, "Nova: SOL tunnel not available (localhost:%d). "
                "Start the tunnel with: autossh -M 0 -f -N -R 2222:localhost:22 ubuntu@<ec2-ip>\n",
                sol_port);
        return "";
    }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return ""; }

    LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(session);
    if (!ch) { sol_close_session(session, sock); return ""; }

    libssh2_channel_exec(ch, command.c_str());

    string result;
    char buf[4096];
    int n;
    while ((n = libssh2_channel_read(ch, buf, sizeof(buf))) > 0)
        result.append(buf, n);

    libssh2_channel_send_eof(ch);
    libssh2_channel_free(ch);
    sol_close_session(session, sock);
    return result;
}

void sftpUploadSOL(const string& localPath, const string& remotePath)
{
    int sock = sol_open_socket();
    if (sock < 0) { fprintf(stderr, "Nova: SOL tunnel not available.\n"); return; }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return; }

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session);
    if (!sftp) { sol_close_session(session, sock); return; }

    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, remotePath.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (fh) {
        ifstream in(localPath, ios::binary);
        char buf[16384];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
            libssh2_sftp_write(fh, buf, (size_t)in.gcount());
        libssh2_sftp_close(fh);
    }
    libssh2_sftp_shutdown(sftp);
    sol_close_session(session, sock);
}

void sftpDownloadSOL(const string& remotePath, const string& localPath)
{
    int sock = sol_open_socket();
    if (sock < 0) { fprintf(stderr, "Nova: SOL tunnel not available.\n"); return; }
    LIBSSH2_SESSION* session = sol_open_session(sock);
    if (!session) { close(sock); return; }

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session);
    if (!sftp) { sol_close_session(session, sock); return; }

    LIBSSH2_SFTP_HANDLE* fh = libssh2_sftp_open(sftp, remotePath.c_str(),
        LIBSSH2_FXF_READ, 0);
    if (fh) {
        ofstream out(localPath, ios::binary);
        char buf[16384]; int n;
        while ((n = (int)libssh2_sftp_read(fh, buf, sizeof(buf))) > 0)
            out.write(buf, n);
        libssh2_sftp_close(fh);
    }
    libssh2_sftp_shutdown(sftp);
    sol_close_session(session, sock);
}

#else  // no libssh2 or Windows

string sshExecSOL(const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2. "
            "Install with: sudo apt install libssh2-1-dev\n");
    return "";
}
void sftpUploadSOL(const string&, const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2.\n");
}
void sftpDownloadSOL(const string&, const string&)
{
    fprintf(stderr, "Nova: SOL cluster requires libssh2.\n");
}

#endif  // NOVA_HAVE_LIBSSH2

static string sol_replace_all(string tmpl, const string& from, const string& to)
{
    size_t pos = 0;
    while ((pos = tmpl.find(from, pos)) != string::npos) {
        tmpl.replace(pos, from.size(), to);
        pos += to.size();
    }
    return tmpl;
}

string generateTrainScript(const string& model, const string& task,
                                   const string& dsName, const string& dsSplit,
                                   int epochs, float lr, int batchSize,
                                   const string& outputDir, const string& hfTok)
{
    string t = R"NOVA(#!/usr/bin/env python3
# Generated by NovaComp sol.finetune()
import os, json
from datasets import load_dataset
from transformers import (AutoTokenizer, AutoModelForSequenceClassification,
                          TrainingArguments, Trainer)

MODEL_NAME   = "{{MODEL_NAME}}"
TASK         = "{{TASK}}"
DATASET_NAME = "{{DATASET_NAME}}"
DATASET_SPLIT= "{{DATASET_SPLIT}}"
EPOCHS       = {{EPOCHS}}
LR           = {{LR}}
BATCH_SIZE   = {{BATCH_SIZE}}
OUTPUT_DIR   = "{{OUTPUT_DIR}}"
HF_TOKEN     = "{{HF_TOKEN}}"

print(f"Loading model: {MODEL_NAME}")
tokenizer = AutoTokenizer.from_pretrained(MODEL_NAME, token=HF_TOKEN)
model = AutoModelForSequenceClassification.from_pretrained(
    MODEL_NAME, token=HF_TOKEN, num_labels=2)

print(f"Loading dataset: {DATASET_NAME}/{DATASET_SPLIT}")
dataset = load_dataset(DATASET_NAME, split=DATASET_SPLIT, token=HF_TOKEN)

def tokenize(examples):
    return tokenizer(examples["text"], truncation=True, padding=True, max_length=512)

tokenized = dataset.map(tokenize, batched=True)

training_args = TrainingArguments(
    output_dir=OUTPUT_DIR,
    num_train_epochs=EPOCHS,
    per_device_train_batch_size=BATCH_SIZE,
    learning_rate=LR,
    save_strategy="epoch",
    logging_dir=f"{OUTPUT_DIR}/logs",
    report_to="none"
)

trainer = Trainer(model=model, args=training_args, train_dataset=tokenized)
print("Starting training...")
trainer.train()
print("Saving model...")
trainer.save_model(OUTPUT_DIR)
tokenizer.save_pretrained(OUTPUT_DIR)

# Ensure pytorch_model.bin exists — login node may have older transformers
# that cannot load safetensors format.
import os as _os
_st = _os.path.join(OUTPUT_DIR, 'model.safetensors')
_pt = _os.path.join(OUTPUT_DIR, 'pytorch_model.bin')
if _os.path.exists(_st) and not _os.path.exists(_pt):
    try:
        from safetensors.torch import load_file as _lf
        import torch as _torch
        _torch.save(_lf(_st), _pt)
        print("Converted model.safetensors -> pytorch_model.bin for compatibility")
    except Exception as _e:
        print(f"[Warning] safetensors->pytorch conversion failed: {_e}")


# Upload trained model to user's file storage via pre-signed S3 URL
_upload_url = "{{UPLOAD_URL}}"
_s3_key     = "{{S3_KEY}}"
if _upload_url:
    print("Packaging model for user file storage...")
    import subprocess as _sp
    _tar = OUTPUT_DIR.rstrip('/') + '.tar.gz'
    _sp.run(['tar', '-czf', _tar, '-C', _os.path.dirname(OUTPUT_DIR),
             _os.path.basename(OUTPUT_DIR)], check=True)
    _r = _sp.run(
        ['curl', '-s', '-o', '/dev/null', '-w', '%{http_code}',
         '-X', 'PUT', '-H', 'Content-Type: application/gzip',
         '-T', _tar, _upload_url],
        capture_output=True, text=True)
    _code = _r.stdout.strip()
    if _code == '200':
        print(f"NOVA_MODEL_UPLOADED:{_s3_key}")
        print("Model uploaded to your file storage successfully.")
    else:
        print(f"[Warning] Model upload to file storage failed (HTTP {_code})")
    _sp.run(['rm', '-f', _tar])

summary = {"model": MODEL_NAME, "task": TASK, "epochs": EPOCHS, "status": "completed"}
with open(f"{OUTPUT_DIR}/summary.json", "w") as f:
    json.dump(summary, f)
print(f"Training complete! Model saved to {OUTPUT_DIR}")
)NOVA";

    char lrBuf[32];
    snprintf(lrBuf, sizeof(lrBuf), "%.8f", (double)lr);

    const char* uploadUrlEnv = getenv("NOVA_MODEL_UPLOAD_URL");
    const char* s3KeyEnv     = getenv("NOVA_MODEL_S3_KEY");

    t = sol_replace_all(t, "{{MODEL_NAME}}",    model);
    t = sol_replace_all(t, "{{TASK}}",          task);
    t = sol_replace_all(t, "{{DATASET_NAME}}",  dsName);
    t = sol_replace_all(t, "{{DATASET_SPLIT}}", dsSplit);
    t = sol_replace_all(t, "{{EPOCHS}}",        to_string(epochs));
    t = sol_replace_all(t, "{{LR}}",            string(lrBuf));
    t = sol_replace_all(t, "{{BATCH_SIZE}}",    to_string(batchSize));
    t = sol_replace_all(t, "{{OUTPUT_DIR}}",    outputDir);
    t = sol_replace_all(t, "{{HF_TOKEN}}",      hfTok);
    t = sol_replace_all(t, "{{UPLOAD_URL}}",    uploadUrlEnv ? string(uploadUrlEnv) : "");
    t = sol_replace_all(t, "{{S3_KEY}}",        s3KeyEnv     ? string(s3KeyEnv)     : "");
    return t;
}

string generateSlurmScript(const string& jobId, const string& jobDir)
{
    string t = R"(#!/bin/bash
#SBATCH --job-name=nova_{{JOB_ID}}
#SBATCH --output={{JOB_DIR}}/slurm_output.txt
#SBATCH --error={{JOB_DIR}}/slurm_error.txt
#SBATCH --time=02:00:00
#SBATCH --partition=general
#SBATCH --gres=gpu:1
#SBATCH --mem=16G
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4

cd {{JOB_DIR}}
module load cuda/12.9
module load python/3.10
# Only install if not already present (avoids ~60s overhead on every job)
python -c "import torch, transformers, datasets, accelerate, safetensors" 2>/dev/null || \
    pip install --quiet torch transformers datasets accelerate safetensors --user
python train.py
echo "NOVA_JOB_DONE:$?"
)";
    t = sol_replace_all(t, "{{JOB_ID}}",  jobId);
    t = sol_replace_all(t, "{{JOB_DIR}}", jobDir);
    return t;
}
