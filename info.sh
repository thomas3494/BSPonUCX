#!/bin/sh

#SBATCH --partition=rome
#SBATCH --nodes=2
#SBATCH --tasks-per-node=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=0
#SBATCH --time=00:10:00
#SBATCH --output=info.out

module load 2024
module load OpenMPI/5.0.3-GCC-13.3.0
module load UCX/1.16.0-GCCcore-13.3.0

ucx_info -d > ucx_info.txt
ompi_info -V > ompi_info.txt

cat /etc/issue > OS_version.txt
uname -a >> OS_version.txt
rpm -q libibverbs > ib_devinfo.txt
ibv_devinfo -vv >> ib_devinfo.txt
