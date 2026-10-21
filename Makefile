# ==============================================================================
# The Matrix Rooftop Bullet-Time 3D Terminal Simulation
# ==============================================================================

CC ?= gcc
CPU_CFLAGS ?= -O3 -march=native -ffast-math -fopenmp -Wall -Wextra
GPU_CFLAGS ?= -O3 -march=native -ffast-math -Wall -Wextra
CPU_LDFLAGS ?= -lm
GPU_LDFLAGS ?= -lglfw -lGL -lm

TARGETS = main ascii gpu-main gpu-ascii

.PHONY: all clean run-main run-ascii run-gpu-main run-gpu-ascii

all: $(TARGETS)

main: main.c
	$(CC) $(CPU_CFLAGS) $< -o $@ $(CPU_LDFLAGS)

ascii: ascii.c
	$(CC) $(CPU_CFLAGS) $< -o $@ $(CPU_LDFLAGS)

gpu-main: gpu-main.c
	$(CC) $(GPU_CFLAGS) $< -o $@ $(GPU_LDFLAGS)

gpu-ascii: gpu-ascii.c
	$(CC) $(GPU_CFLAGS) $< -o $@ $(GPU_LDFLAGS)

run-main: main
	./main

run-ascii: ascii
	./ascii

run-gpu-main: gpu-main
	./gpu-main

run-gpu-ascii: gpu-ascii
	./gpu-ascii

clean:
	rm -f $(TARGETS)
