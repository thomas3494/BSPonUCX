.PHONY: all clean

MPICC = mpicc
CC	  = gcc
FLAGS = -O3 -march=native -mtune=native -Wall -Wextra -ggdb #-fsanitize=undefined -fsanitize=address

all: mem_map_bench

mem_map_bench: mem_map_bench.c
	$(MPICC) $(FLAGS) $< -o $@ -lucp -luct -lucs -lucm -lm

clean:
	$(RM) mem_map_bench
