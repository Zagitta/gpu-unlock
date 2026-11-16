# Square GEMM rate benchmark

`bench_gemm.c` measures. It runs one square half-precision GEMM through cuBLAS on
every device you name, times each launch with CUDA events, and prints the best of
them with the device's PCI bus id, so a line can be matched to a card. It needs the
CUDA toolkit to build and a working driver to run; a driver alone is not enough.

```sh
gcc -O2 tools/bench_gemm.c -o bench_gemm -lcublas -lcudart
./bench_gemm 0 8192 30 bf16
./bench_gemm 0 8192 30 fp16
```

It is the A/B instrument for the unlock, so compare a card only with itself: lock
the clocks (`nvidia-smi -lgc 1500,1500`, released afterwards with `-rgc`), run it,
power the card off, unlock it with `gpu_unlock.targets=` on the kernel command line,
and run it again. A number from one card, clock, power limit or temperature says
nothing about another.
