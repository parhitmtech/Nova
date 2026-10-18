/*
 * Copyright (C) Rida Bazzi, 2017
 *
 * Do not share this file with anyone
 */
// Interpreter: HuggingFace inference and SOL cluster operations. Split out of execute_program (interpreter.cc).
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <cstring>
#include <string>
#include <stack>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cmath>
#include <random>
#include <unordered_map>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "lexer.h"
#include "compiler.h"
#include "bytecode.h"
#include "autograd.h"
#include "optimizer.h"
#include "regalloc.h"
#include "codegen.h"
#include "integrations.h"
#include "runtime.h"
#include "tensor_ops.h"
#include "interp_internal.h"

using namespace std;

// Remote directory on SOL for training jobs (relative paths resolve from the SOL home directory).
static string sol_work_dir = getenv("SOL_WORK_DIR") ? getenv("SOL_WORK_DIR") : "novacomp_jobs";

InstructionNode* exec_hf_infer(InstructionNode* pc)
{
    do {
            auto& hf = pc->hf_inst;
            string model = strMem[mem[hf.model_slot]];

            if (hf.op == HF_OP_SET_TOKEN) {
                hf_api_token = model;
                pc = pc->next;
                break;
            }

            if (hf.op == HF_OP_DATASET) {
                // model_slot holds dataset name, input_slot holds split string
                string dsName  = model;
                string dsSplit = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "train";
                string handle  = dsName + ":" + dsSplit;
                int strIdx = (int)strMem.size();
                strMem.push_back(handle);
                if (hf.result_slot >= 0) mem[hf.result_slot] = strIdx;
                pc = pc->next;
                break;
            }

            // Trim any whitespace/newlines the parser may have left in the model name
            while (!model.empty() && (model.back() == ' ' || model.back() == '\n' ||
                   model.back() == '\r' || model.back() == '\t')) model.pop_back();
            while (!model.empty() && (model.front() == ' ' || model.front() == '\n' ||
                   model.front() == '\r' || model.front() == '\t')) model = model.substr(1);

            string tok = hf_api_token;
            if (tok.empty()) {
                const char* env = getenv("HF_API_TOKEN");
                if (env) tok = env;
            }
            // Trim token too
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\n' ||
                   tok.back() == '\r' || tok.back() == '\t')) tok.pop_back();

            string input = (hf.input_slot >= 0) ? strMem[mem[hf.input_slot]] : "";

            string payload;
            if (hf.op == HF_OP_GENERATE) {
                payload = "{\"inputs\":\"" + hf_escape_json(input) +
                          "\",\"parameters\":{\"max_new_tokens\":" +
                          to_string(hf.max_tokens) + "}}";
            } else {
                payload = "{\"inputs\":\"" + hf_escape_json(input) + "\"}";
            }

            string jobTag      = to_string((long long)(intptr_t)pc);
            string payloadFile = "/tmp/hf_payload_" + jobTag + ".json";
            string tmpOut      = "/tmp/hf_resp_"    + jobTag + ".json";
            string pyFile      = "/tmp/hf_req_"     + jobTag + ".py";

            // Write the JSON payload to a file — avoids all shell-quoting issues.
            { ofstream pf(payloadFile); pf << payload; }

            // Use Python urllib with:
            //   ProxyHandler({})  — disables all env-based proxies
            //   NoRedirect        — catches HTTP redirects before they reach HTML pages.
            //                       HF redirects requests with invalid/missing tokens
            //                       to huggingface.co (HTML) instead of returning JSON.
            string pyScript =
                "import urllib.request, urllib.error, json, sys\n"
                "\n"
                "class NoRedirect(urllib.request.HTTPRedirectHandler):\n"
                "    def redirect_request(self, req, fp, code, msg, headers, newurl):\n"
                "        raise Exception('HF API redirected ' + str(code) + ' to: ' + newurl[:80]\n"
                "                        + ' — check your HF token is valid')\n"
                "\n"
                "with open('" + payloadFile + "', 'rb') as f: data = f.read()\n"
                "url = 'https://api-inference.huggingface.co/models/" + model + "'\n"
                "req = urllib.request.Request(url, data=data, method='POST')\n"
                "req.add_header('Authorization', 'Bearer " + tok + "')\n"
                "req.add_header('Content-Type', 'application/json')\n"
                "opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect)\n"
                "try:\n"
                "    with opener.open(req, timeout=30) as r:\n"
                "        sys.stdout.write(r.read().decode('utf-8'))\n"
                "except urllib.error.HTTPError as e:\n"
                "    try: sys.stdout.write(e.read().decode('utf-8'))\n"
                "    except: sys.stdout.write(json.dumps({'error': 'HTTP ' + str(e.code)}))\n"
                "except Exception as e:\n"
                "    sys.stdout.write(json.dumps({'error': str(e)}))\n";

            { ofstream pf(pyFile); pf << pyScript; }

            string pyCmd = "python3 " + pyFile + " > " + tmpOut + " 2>/dev/null";
            fprintf(stderr, "HF_CMD: python3 %s\n", pyFile.c_str());
            if (system(pyCmd.c_str()) == -1)
                fprintf(stderr, "hf: could not start python3\n");

            string response;
            {
                ifstream rf(tmpOut);
                response.assign(istreambuf_iterator<char>(rf),
                                istreambuf_iterator<char>());
            }
            fprintf(stderr, "HF_RESP: %s\n", response.substr(0, 200).c_str());
            remove(payloadFile.c_str());
            remove(pyFile.c_str());
            remove(tmpOut.c_str());

            if (hf.result_slot >= 0) {
                string text = hf_extract_text(response);
                int strIdx = (int)strMem.size();
                strMem.push_back(text);
                mem[hf.result_slot] = strIdx;
            }

            pc = pc->next;
            break;
    } while (0);
    return pc;
}

InstructionNode* exec_sol_call(InstructionNode* pc)
{
    do {
            auto& si = pc->sol_inst;

            switch (si.op)
            {
                case SOL_OP_SET_KEY:
                {
                    // sol.set_key() is disabled — SSH key is configured server-side
                    // via SOL_PRIVATE_KEY_PATH to prevent users from accessing SOL directly.
                    break;
                }

                case SOL_OP_FINETUNE:
                {
                    string model   = strMem[mem[si.model_slot]];
                    string task    = strMem[mem[si.task_slot]];
                    string dataset = (si.dataset_slot >= 0) ? strMem[mem[si.dataset_slot]] : "imdb:train";

                    // Parse "name:split" handle produced by hf.dataset()
                    string dsName = dataset, dsSplit = "train";
                    size_t sep = dataset.find(':');
                    if (sep != string::npos) {
                        dsName  = dataset.substr(0, sep);
                        dsSplit = dataset.substr(sep + 1);
                    }

                    string jobId  = to_string((long long)time(nullptr)) + "_" +
                                    to_string((unsigned)rand() % 10000);
                    string jobDir = sol_work_dir + "/" + jobId;

                    string trainPy = generateTrainScript(model, task, dsName, dsSplit,
                                                         si.epochs, si.lr, si.batch_size,
                                                         jobDir, hf_api_token);
                    string slurmSh = generateSlurmScript(jobId, jobDir);

                    string localPy    = "/tmp/train_" + jobId + ".py";
                    string localSlurm = "/tmp/job_"   + jobId + ".slurm";
                    sol_write_file(localPy,    trainPy);
                    sol_write_file(localSlurm, slurmSh);

                    sshExecSOL("mkdir -p " + jobDir);
                    sftpUploadSOL(localPy,    jobDir + "/train.py");
                    sftpUploadSOL(localSlurm, jobDir + "/job.slurm");

                    string sbatchOut  = sshExecSOL("sbatch " + jobDir + "/job.slurm");
                    string slurmJobId = sol_extract_slurm_id(sbatchOut);

                    if (slurmJobId.empty()) {
                        // sbatch failed or returned unexpected output — surface it to the user
                        printf("Nova: SOL sbatch failed. Output: %s\n",
                               sbatchOut.empty() ? "(empty — tunnel may be down)" : sbatchOut.c_str());
                        // Use sentinel so the wait loop exits immediately
                        slurmJobId = "0";
                    }

                    printf("Nova: SOL finetune submitted — SLURM job ID %s\n", slurmJobId.c_str());

                    // Handle: "jobId:slurmJobId:jobDir:model"
                    string handle = jobId + ":" + slurmJobId + ":" + jobDir + ":" + model;
                    int strIdx = (int)strMem.size();
                    strMem.push_back(handle);
                    mem[si.job_slot] = strIdx;

                    remove(localPy.c_str());
                    remove(localSlurm.c_str());
                    break;
                }

                case SOL_OP_WAIT:
                {
                    string handle = strMem[mem[si.job_slot]];
                    // Parse handle: "jobId:slurmJobId:jobDir:model"
                    size_t c1 = handle.find(':');
                    size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                    size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                    if (c1 == string::npos) { pc = pc->next; break; }
                    string slurmJobId = handle.substr(c1 + 1, c2 - c1 - 1);

                    if (slurmJobId == "0") {
                        printf("Nova: Skipping wait — job was not submitted.\n");
                        pc = pc->next;
                        break;
                    }

                    string jobDir_wait = (c3 != string::npos)
                        ? handle.substr(c2 + 1, c3 - c2 - 1)
                        : handle.substr(c2 + 1);

                    bool seenRunning = false;
                    int  runPolls    = 0;
                    string lastSnippet;

                    while (true) {
                        string status = sol_trim(sshExecSOL(
                            "squeue -j " + slurmJobId + " -h -o \"%T\" 2>/dev/null || echo DONE"));

                        if (status.empty() || status == "DONE" || status == "COMPLETED") {
                            printf("Nova: Training complete!\n");
                            break;
                        } else if (status == "FAILED" || status == "CANCELLED") {
                            fprintf(stderr, "Nova: Job %s %s on SOL cluster.\n",
                                    slurmJobId.c_str(), status.c_str());
                            break;
                        } else if (status == "PENDING" || status == "CONFIGURING") {
                            printf("Nova: Training job in queue, waiting for GPU server...\n");
                        } else if (status == "RUNNING") {
                            if (!seenRunning) {
                                printf("Nova: GPU server acquired! Training started.\n");
                                seenRunning = true;
                                runPolls = 0;
                            }
                            // Stream a live snippet of the training log every poll (60s)
                            if (runPolls % 1 == 0) {
                                string snippet = sol_trim(sshExecSOL(
                                    "tail -2 " + jobDir_wait + "/slurm_output.txt 2>/dev/null"));
                                if (!snippet.empty() && snippet != lastSnippet) {
                                    printf("%s\n", snippet.c_str());
                                    lastSnippet = snippet;
                                }
                            }
                            runPolls++;
                        }
    #ifndef _WIN32
                        sleep(60);
    #endif
                    }

                    // Show final training summary (metrics + upload confirmation)
                    string slurmLog = sol_trim(sshExecSOL(
                        "grep -v 'it/s\\]' " + jobDir_wait +
                        "/slurm_output.txt 2>/dev/null | tail -15"));
                    if (!slurmLog.empty())
                        printf("Nova: [Training summary]\n%s\n", slurmLog.c_str());

                    // Show any errors from SLURM stderr
                    string slurmErr = sol_trim(sshExecSOL(
                        "tail -10 " + jobDir_wait + "/slurm_error.txt 2>/dev/null"));

                    break;
                }

                case SOL_OP_PREDICT:
                {
                    string handle = strMem[mem[si.job_slot]];
                    size_t c2 = handle.find(':', handle.find(':') + 1);
                    size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                    string jobDir = (c3 != string::npos)
                        ? handle.substr(c2 + 1, c3 - c2 - 1)
                        : handle.substr(c2 + 1);
                    string input  = strMem[mem[si.input_slot]];

                    // Escape input for embedding in Python string literal
                    string escaped;
                    for (char c : input) {
                        if (c == '"') escaped += "\\\"";
                        else if (c == '\\') escaped += "\\\\";
                        else escaped += c;
                    }

                    string inferPy =
                        "import sys, os, json\n"
                        "job_dir = '" + jobDir + "'\n"
                        "if not os.path.isdir(job_dir):\n"
                        "    print(f'[SOL predict] Job directory not found: {job_dir}')\n"
                        "    sys.exit(0)\n"
                        "if 'config.json' not in os.listdir(job_dir):\n"
                        "    print('[SOL predict] config.json missing — training likely failed')\n"
                        "    sys.exit(0)\n"
                        "try:\n"
                        "    import torch\n"
                        "    from transformers import AutoTokenizer, AutoModelForSequenceClassification\n"
                        "    tokenizer = AutoTokenizer.from_pretrained(job_dir, local_files_only=True)\n"
                        "    model = AutoModelForSequenceClassification.from_pretrained(\n"
                        "        job_dir, local_files_only=True, ignore_mismatched_sizes=True)\n"
                        "    model.eval()\n"
                        "    inputs = tokenizer(\"" + escaped + "\", return_tensors='pt',\n"
                        "                       truncation=True, padding=True, max_length=512)\n"
                        "    inputs.pop('token_type_ids', None)  # not all models use this\n"
                        "    with torch.no_grad():\n"
                        "        logits = model(**inputs).logits\n"
                        "    probs = torch.softmax(logits, dim=-1)[0]\n"
                        "    pred  = int(torch.argmax(probs).item())\n"
                        "    label = model.config.id2label.get(pred, f'LABEL_{pred}')\n"
                        "    score = float(probs[pred].item())\n"
                        "    print(f'{label} ({score:.3f})')\n"
                        "except Exception as e:\n"
                        "    print(f'[SOL predict error: {e}]')\n";

                    string localInfer  = "/tmp/infer_" + to_string((long long)(intptr_t)pc) + ".py";
                    string remoteInfer = jobDir + "/infer.py";
                    sol_write_file(localInfer, inferPy);
                    sftpUploadSOL(localInfer, remoteInfer);
                    remove(localInfer.c_str());

                    // Use bash -l (login shell) so module system init scripts are sourced.
                    // Direct SSH exec doesn't run /etc/profile.d/modules.sh, so
                    // `module load` would silently fail in a plain exec session.
                    string output = sol_trim(sshExecSOL(
                        "bash -l -c '"
                        "source /etc/profile.d/modules.sh 2>/dev/null || true; "
                        "module load python/3.10 2>/dev/null || true; "
                        "python3 " + remoteInfer + " 2>&1'"));
                    if (output.empty())
                        output = "[SOL predict: no output — model may not have saved correctly]";
                    int strIdx = (int)strMem.size();
                    strMem.push_back(output);
                    if (si.result_slot >= 0) mem[si.result_slot] = strIdx;
                    break;
                }

                case SOL_OP_SAVE:
                {
                    string handle = strMem[mem[si.job_slot]];
                    size_t c1 = handle.find(':');
                    size_t c2 = (c1 != string::npos) ? handle.find(':', c1 + 1) : string::npos;
                    size_t c3 = (c2 != string::npos) ? handle.find(':', c2 + 1) : string::npos;
                    string jobId  = handle.substr(0, c1);
                    string jobDir = (c3 != string::npos) ? handle.substr(c2 + 1, c3 - c2 - 1) : handle.substr(c2 + 1);
                    string model  = (c3 != string::npos) ? handle.substr(c3 + 1) : "";
                    string savePath = strMem[mem[si.path_slot]];

                    // Check if the SLURM job uploaded the model to S3
                    string uploadedS3Key;
                    string slurmFullLog = sol_trim(sshExecSOL(
                        "cat " + jobDir + "/slurm_output.txt 2>/dev/null"));
                    const string UPLOAD_MARKER = "NOVA_MODEL_UPLOADED:";
                    size_t mpos = slurmFullLog.find(UPLOAD_MARKER);
                    if (mpos != string::npos) {
                        size_t end = slurmFullLog.find('\n', mpos + UPLOAD_MARKER.size());
                        uploadedS3Key = sol_trim(slurmFullLog.substr(
                            mpos + UPLOAD_MARKER.size(),
                            end == string::npos ? string::npos
                                                : end - mpos - UPLOAD_MARKER.size()));
                    }

                    if (!uploadedS3Key.empty()) {
                        // Model was uploaded to S3 by the SLURM compute node
                        string fname = uploadedS3Key;
                        size_t sl = fname.rfind('/');
                        if (sl != string::npos) fname = fname.substr(sl + 1);
                        printf("Nova: Model saved to your file storage as '%s'\n", fname.c_str());
                        // Marker for server to register in DynamoDB (no upload needed)
                        printf("NOVA_S3_FILE:%s\n", uploadedS3Key.c_str());
                    } else {
                        // Fallback: write a small JSON reference — model stays on SOL scratch
                        string meta =
                            "{\n"
                            "  \"type\": \"hf_model_ref\",\n"
                            "  \"model\": \"" + model + "\",\n"
                            "  \"job_id\": \"" + jobId + "\",\n"
                            "  \"sol_path\": \"" + jobDir + "\"\n"
                            "}\n";

                        ofstream nbc(savePath);
                        nbc << meta;
                        nbc.close();

                        printf("Nova: Model reference saved to %s\n", savePath.c_str());
                        printf("Nova: Full model weights remain on SOL at %s\n", jobDir.c_str());
                        printf("NOVA_SAVE_FILE:%s\n", savePath.c_str());
                    }
                    break;
                }
            }

            pc = pc->next;
            break;
    } while (0);
    return pc;
}
