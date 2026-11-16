#!/bin/bash
# The Triton matmul as a SOL batch job: from this directory on SOL, `sbatch run_triton.sh`. It benchmarks Triton
# against cuBLAS (FP32 and TF32) and writes one kernel's IR at each stage to ir/. The output lands in
# triton-<job id>.out here. It needs a conda environment with PyTorch (which brings Triton), made once:
#   conda create -n nova-triton python=3.11 -y && conda activate nova-triton && pip install torch
# CONDA_ENV names another environment.
#SBATCH --job-name=nova-triton
#SBATCH --partition=public
#SBATCH --qos=public
#SBATCH --gres=gpu:a100:1
#SBATCH --time=0-00:20:00
#SBATCH --output=triton-%j.out

set -e
cd "${SLURM_SUBMIT_DIR:-.}"

# A batch job does not read ~/.bashrc, so conda may need loading first
command -v conda >/dev/null || module load mamba/latest 2>/dev/null || true
command -v conda >/dev/null || source ~/.bashrc
eval "$(conda shell.bash hook)"
conda activate "${CONDA_ENV:-nova-triton}"

nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
python matmul.py
python matmul.py --dump-ir ir
