#!/bin/bash
# The Triton matmul as a SOL batch job: from this directory on SOL, `sbatch run_triton.sh`. It benchmarks Triton
# against cuBLAS (FP32 and TF32) and writes one kernel's IR at each stage to ir/. The output lands in
# triton-<job id>.out here. It needs a Python with PyTorch (which brings Triton): PYTHON names one; otherwise a
# Python 3.10 installed under ~/python3.10 is used when there is one, and else the conda environment CONDA_ENV
# (default nova-triton), made once with
#   conda create -n nova-triton python=3.11 -y && conda activate nova-triton && python -m pip install torch
#SBATCH --job-name=nova-triton
#SBATCH --partition=public
#SBATCH --qos=public
#SBATCH --gres=gpu:a100:1
#SBATCH --time=0-00:20:00
#SBATCH --output=triton-%j.out

set -e
cd "${SLURM_SUBMIT_DIR:-.}"

if [ -z "$PYTHON" ] && [ -x "$HOME/python3.10/bin/python3.10" ]; then
    PYTHON="$HOME/python3.10/bin/python3.10"
fi
if [ -z "$PYTHON" ]; then
    # A batch job does not read ~/.bashrc, so conda may need loading first
    command -v conda >/dev/null || module load mamba/latest 2>/dev/null || true
    command -v conda >/dev/null || source ~/.bashrc
    eval "$(conda shell.bash hook)"
    conda activate "${CONDA_ENV:-nova-triton}"
    PYTHON=python
fi

nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
"$PYTHON" matmul.py
"$PYTHON" matmul.py --dump-ir ir
