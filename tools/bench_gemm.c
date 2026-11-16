// SPDX-License-Identifier: GPL-2.0-only
/*
 * Measure the number the tensor-core cap moves: a square half-precision GEMM
 * through cuBLAS, the best of a number of timed launches, per device.
 *
 * gcc -O2 bench_gemm.c -o bench_gemm -lcublas -lcudart
 * ./bench_gemm [device] [n] [iters] [bf16|fp16]
 *
 * Built against the CUDA toolkit in the loader path. Where it is not, add
 * -I/usr/local/cuda/include -L/usr/local/cuda/lib64
 * -Wl,-rpath,/usr/local/cuda/lib64.
 */
#include <cublas_v2.h>
#include <cuda_runtime.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(x)                                   \
	do {                                    \
		int e_ = (x);                   \
		if (e_) {                       \
			fprintf(stderr, "%s: %d\n", #x, e_); \
			exit(1);                \
		}                               \
	} while (0)

static int lower_pci_bus_id(char *out, size_t length, int dev)
{
	int i;

	if (cudaDeviceGetPCIBusId(out, (int)length, dev) != cudaSuccess)
		return 0;
	for (i = 0; out[i]; i++)
		out[i] = (char)tolower((unsigned char)out[i]);
	return 1;
}

int main(int argc, char **argv)
{
	int dev = argc > 1 ? atoi(argv[1]) : 0;
	int n = argc > 2 ? atoi(argv[2]) : 8192;
	int iters = argc > 3 ? atoi(argv[3]) : 30;
	const char *type = argc > 4 ? argv[4] : "bf16";
	cudaDataType_t dt = strcmp(type, "fp16") ? CUDA_R_16BF : CUDA_R_16F;
	struct cudaDeviceProp prop;
	cublasHandle_t h;
	unsigned short *a, *b;
	float *c;
	float alpha = 1.0f, beta = 0.0f;
	cudaEvent_t t0, t1;
	char bus[32] = "";
	float best = 1e30f, ms;
	double tflops;
	int i;

	if (n < 1024 || iters < 1) {
		fprintf(stderr, "usage: %s [device] [n] [iters] [bf16|fp16]\n", argv[0]);
		return 2;
	}
	memset(&prop, 0, sizeof prop);
	CK(cudaSetDevice(dev));
	CK(cudaGetDeviceProperties(&prop, dev));
	if (!lower_pci_bus_id(bus, sizeof bus, dev))
		snprintf(bus, sizeof bus, "unknown");
	CK(cudaMalloc((void **)&a, (size_t)n * n * 2));
	CK(cudaMalloc((void **)&b, (size_t)n * n * 2));
	CK(cudaMalloc((void **)&c, (size_t)n * n * 4));

	/* Uniform byte fill: 0x3f3f is a normal finite number in both 16-bit
	 * formats, and the tensor-core rate does not depend on the values.
	 */
	CK(cudaMemset(a, 0x3f, (size_t)n * n * 2));
	CK(cudaMemset(b, 0x3f, (size_t)n * n * 2));

	CK(cublasCreate(&h));
	CK(cudaEventCreate(&t0));
	CK(cudaEventCreate(&t1));

	/* Let cuBLAS pick and warm its kernel before anything is timed. */
	for (i = 0; i < 3; i++)
		CK(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, n, n, n, &alpha, a, dt, n,
				b, dt, n, &beta, c, CUDA_R_32F, n, CUBLAS_COMPUTE_32F,
				CUBLAS_GEMM_DEFAULT));
	CK(cudaDeviceSynchronize());

	for (i = 0; i < iters; i++) {
		CK(cudaEventRecord(t0, 0));
		CK(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, n, n, n, &alpha, a, dt, n,
				b, dt, n, &beta, c, CUDA_R_32F, n, CUBLAS_COMPUTE_32F,
				CUBLAS_GEMM_DEFAULT));
		CK(cudaEventRecord(t1, 0));
		CK(cudaEventSynchronize(t1));
		CK(cudaEventElapsedTime(&ms, t0, t1));
		if (ms < best)
			best = ms;
	}

	tflops = 2.0 * n * n * n / (best * 1e9);
	printf("dev%d %s %s %s n=%d best=%.3f ms %.2f TFLOPS\n", dev, bus,
	       prop.name, type, n, best, tflops);

	cublasDestroy(h);
	return 0;
}
