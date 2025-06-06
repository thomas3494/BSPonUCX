#!/bin/sh

#SBATCH --partition=rome
#SBATCH --nodes=2
#SBATCH --tasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=0
#SBATCH --time=00:10:00
#SBATCH --output=mem_map_bench.out

module load 2024
module load OpenMPI/5.0.3-GCC-13.3.0
module load UCX/1.16.0-GCCcore-13.3.0

make clean
make
srun --cpu-bind=none ./mem_map_bench 5 80
