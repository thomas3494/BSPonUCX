.PHONY: all clean

MPICC = mpicc
CC	  = gcc
FLAGS = -O3 -march=native -mtune=native -Wall -Wextra -ggdb -fopenmp#-fsanitize=undefined -fsanitize=address

all: mem_map_bench mem_map_stream stream

mem_map_bench: mem_map_bench.c
	$(MPICC) $(FLAGS) $< -o $@ -lucp -luct -lucs -lucm -lm

mem_map_stream: mem_map_stream.c
	$(MPICC) -DMEM_MAP=1 $(FLAGS) $< -o $@ -lucp -luct -lucs -lucm -lm

stream: mem_map_stream.c
	$(MPICC) $(FLAGS) $< -o $@ -lucp -luct -lucs -lucm -lm

clean:
	$(RM) mem_map_bench mem_map_stream stream
