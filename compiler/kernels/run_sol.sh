#!/bin/bash
# The matmul benchmark as a SOL batch job: from this directory on SOL, `sbatch run_sol.sh` (any arguments go to
# ./bench, e.g. `sbatch run_sol.sh 2048 2048 2048 8`). The output lands in matmul-<job id>.out here.
#SBATCH --job-name=nova-matmul
#SBATCH --partition=general
#SBATCH --qos=public
#SBATCH --gres=gpu:a100:1
#SBATCH --time=0-00:15:00
#SBATCH --output=matmul-%j.out

set -e
cd "${SLURM_SUBMIT_DIR:-.}"

# A CUDA toolkit: CUDA_MODULE names one (see `module avail cuda`), or a default one is tried
module load "${CUDA_MODULE:-cuda-12.6.1-gcc-12.1.0}" 2>/dev/null || module load cuda 2>/dev/null || true
command -v nvcc >/dev/null || { echo "no nvcc: set CUDA_MODULE to one of \`module avail cuda\`"; exit 1; }

nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
make ARCH="${ARCH:-sm_80}"
./bench "$@"
