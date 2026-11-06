import torch, time
torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False
a = torch.randn(8192, 8192, device="cuda", dtype=torch.bfloat16)
b = torch.randn_like(a)
for _ in range(3):
    a @ b
torch.cuda.synchronize()
t = time.time()
for _ in range(10):
    c = a @ b
torch.cuda.synchronize()
dt = (time.time() - t) / 10
tflops = 2 * 8192**3 / dt / 1e12
print(f"BENCH-BF16-FP32ACCUM: {tflops:.1f} TFLOPS")
print(f"DEVICE: {torch.cuda.get_device_name(0)}  torch={torch.__version__}")
