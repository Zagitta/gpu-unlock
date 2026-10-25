#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reproduce the AD107 evidence workload; GPU execution requires --run --pci.

Without --run this only constructs the pinned PTX and prints its hash.
Uses Python's standard library and the installed NVIDIA CUDA/NVML libraries;
no CUDA toolkit, firmware writes, clock changes, reset or module operations.
"""
import argparse
import ctypes as C
import hashlib
import json
import re
import statistics
import time

# The register-resident eight-chain generator follows
# JustAResearcher/gpu-unlock-ada@491925c1079435e260137b9f3526066e764de830,
# diagnostics/bench/tensor_modes_issue.cu (GPL-2.0-only).
# The following generator and measured kernel parameters reproduce the original
# evidence cells, including the additional FP16-accumulation control.
BENCH_MODES = {
    'fp16_fp16': {'instruction': 'mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16', 'k': 16, 'bits': 0x3c003c00, 'kind': 'half'},
    'fp16_fp32': {'instruction': 'mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32', 'k': 16, 'bits': 0x3c003c00, 'kind': 'float'},
    'bf16_fp32': {'instruction': 'mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32', 'k': 16, 'bits': 0x3f803f80, 'kind': 'float'},
    'tf32_fp32': {'instruction': 'mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32', 'k': 8, 'bits': 0x3f800000, 'kind': 'float'},
    'int8_int32': {'instruction': 'mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32', 'k': 32, 'bits': 0x01010101, 'kind': 'int'},
}

def build_mma_ptx(name, mode):
    kind = mode['kind']
    regs_per_chain = 2 if kind == 'half' else 4
    nregs = regs_per_chain * 8
    ctype = 'b32' if kind == 'half' else ('s32' if kind == 'int' else 'f32')
    lines = [f'.visible .entry {name}(.param .u32 iters, .param .u64 sink, .param .u32 aval, .param .u32 bval) {{',
             '.reg .pred %p;', '.reg .u32 %i, %n, %tx, %bx, %bd, %idx;',
             '.reg .u64 %out, %addr, %offset;', '.reg .b32 %a, %b;',
             f'.reg .{ctype} %c<{nregs}>;',
             '.reg .f32 %sum, %tmp;' if kind != 'int' else '.reg .s32 %sum;',
             'ld.param.u32 %n, [iters];', 'ld.param.u64 %out, [sink];',
             'ld.param.b32 %a, [aval];', 'ld.param.b32 %b, [bval];']
    if kind == 'half':
        lines.append('.reg .b16 %lo, %hi;')
    zero = '0f00000000' if kind == 'float' else '0'
    lines += [f'mov.{ctype} %c{r}, {zero};' for r in range(nregs)]
    lines += ['mov.u32 %i, 0;', 'LOOP:']
    for chain in range(8):
        acc = ', '.join(f'%c{r}' for r in range(chain * regs_per_chain, (chain + 1) * regs_per_chain))
        lines.append(f"{mode['instruction']} {{{acc}}}, {{%a, %a, %a, %a}}, {{%b, %b}}, {{{acc}}};")
    lines += ['add.u32 %i, %i, 1;', 'setp.lt.u32 %p, %i, %n;', '@%p bra LOOP;']
    sumtype = 's32' if kind == 'int' else 'f32'
    lines.append(f'mov.{sumtype} %sum, ' + ('0;' if kind == 'int' else '0f00000000;'))
    for r in range(nregs):
        if kind == 'half':
            lines += [f'mov.b32 {{%lo, %hi}}, %c{r};', 'cvt.f32.f16 %tmp, %lo;', 'add.f32 %sum, %sum, %tmp;', 'cvt.f32.f16 %tmp, %hi;', 'add.f32 %sum, %sum, %tmp;']
        else:
            lines.append(f'add.{sumtype} %sum, %sum, %c{r};')
    lines += ['mov.u32 %tx, %tid.x;', 'mov.u32 %bx, %ctaid.x;', 'mov.u32 %bd, %ntid.x;',
              'mad.lo.u32 %idx, %bx, %bd, %tx;', 'mul.wide.u32 %offset, %idx, 4;',
              'add.u64 %addr, %out, %offset;', f'st.global.{sumtype} [%addr], %sum;', 'ret;', '}']
    return '\n'.join(lines)

bench_ptx = '.version 8.0\n.target sm_89\n.address_size 64\n\n' + '\n\n'.join(build_mma_ptx(name, mode) for name, mode in BENCH_MODES.items())



def run_probe(pci):
    cuda_lib = C.CDLL('libcuda.so.1')

    def cuda_api(name, argtypes):
        fn = getattr(cuda_lib, name)
        fn.argtypes, fn.restype = argtypes, C.c_int
        return fn

    cuInit = cuda_api('cuInit', [C.c_uint])

    cuDeviceGetCount = cuda_api('cuDeviceGetCount', [C.POINTER(C.c_int)])

    cuDeviceGet = cuda_api('cuDeviceGet', [C.POINTER(C.c_int), C.c_int])

    cuDeviceGetAttribute = cuda_api('cuDeviceGetAttribute', [C.POINTER(C.c_int), C.c_int, C.c_int])

    cuDeviceGetPCIBusId = cuda_api('cuDeviceGetPCIBusId', [C.c_char_p, C.c_int, C.c_int])

    cuCtxCreate = cuda_api('cuCtxCreate_v2', [C.POINTER(C.c_void_p), C.c_uint, C.c_int])

    cuCtxDestroy = cuda_api('cuCtxDestroy_v2', [C.c_void_p])

    cuModuleLoadDataEx = cuda_api('cuModuleLoadDataEx', [C.POINTER(C.c_void_p), C.c_void_p, C.c_uint, C.POINTER(C.c_int), C.POINTER(C.c_void_p)])

    cuModuleUnload = cuda_api('cuModuleUnload', [C.c_void_p])

    cuModuleGetFunction = cuda_api('cuModuleGetFunction', [C.POINTER(C.c_void_p), C.c_void_p, C.c_char_p])

    cuFuncGetAttribute = cuda_api('cuFuncGetAttribute', [C.POINTER(C.c_int), C.c_int, C.c_void_p])

    cuMemAlloc = cuda_api('cuMemAlloc_v2', [C.POINTER(C.c_uint64), C.c_size_t])

    cuMemFree = cuda_api('cuMemFree_v2', [C.c_uint64])

    cuMemcpyDtoH = cuda_api('cuMemcpyDtoH_v2', [C.c_void_p, C.c_uint64, C.c_size_t])

    cuEventCreate = cuda_api('cuEventCreate', [C.POINTER(C.c_void_p), C.c_uint])

    cuEventRecord = cuda_api('cuEventRecord', [C.c_void_p, C.c_void_p])

    cuEventSynchronize = cuda_api('cuEventSynchronize', [C.c_void_p])

    cuEventElapsedTime = cuda_api('cuEventElapsedTime', [C.POINTER(C.c_float), C.c_void_p, C.c_void_p])

    cuEventDestroy = cuda_api('cuEventDestroy_v2', [C.c_void_p])

    cuCtxSynchronize = cuda_api('cuCtxSynchronize', [])

    cuLaunchKernel = cuda_api('cuLaunchKernel', [C.c_void_p, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_uint, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p])

    cuGetErrorString = cuda_api('cuGetErrorString', [C.c_int, C.POINTER(C.c_char_p)])

    def cuda_check(code, step):
        if code:
            message = C.c_char_p()
            cuGetErrorString(code, C.byref(message))
            raise RuntimeError(f'{step}: CUDA {code}: {message.value!r}')

    cuda_check(cuInit(0), 'cuInit')

    cuda_count = C.c_int()

    cuda_check(cuDeviceGetCount(C.byref(cuda_count)), 'cuDeviceGetCount')

    assert cuda_count.value == 1, f'Unexpected device count: {cuda_count.value}'

    cuda_device = C.c_int()

    cuda_check(cuDeviceGet(C.byref(cuda_device), 0), 'cuDeviceGet')

    pci_buffer = C.create_string_buffer(32)

    cuda_check(cuDeviceGetPCIBusId(pci_buffer, len(pci_buffer), cuda_device.value), 'cuDeviceGetPCIBusId')

    assert pci_buffer.value.decode().lower() == pci.lower(), pci_buffer.value

    cuda_attributes = {}

    for attr_name, attr_code in [('sm_count', 16), ('compute_major', 75), ('compute_minor', 76), ('max_threads_per_block', 1)]:
        attr_value = C.c_int()
        cuda_check(cuDeviceGetAttribute(C.byref(attr_value), attr_code, cuda_device.value), attr_name)
        cuda_attributes[attr_name] = attr_value.value

    assert (cuda_attributes['compute_major'], cuda_attributes['compute_minor']) == (8, 9), cuda_attributes

    if cuda_attributes['sm_count'] != 24:
        raise RuntimeError('This fixed evidence workload requires the measured 24-SM configuration')
    nvml_lib = C.CDLL('libnvidia-ml.so.1')

    def nvml_api(name, argtypes):
        fn = getattr(nvml_lib, name)
        fn.argtypes, fn.restype = argtypes, C.c_int
        return fn

    nvmlInit = nvml_api('nvmlInit_v2', [])

    nvmlShutdown = nvml_api('nvmlShutdown', [])

    nvmlGetDevice = nvml_api('nvmlDeviceGetHandleByPciBusId_v2', [C.c_char_p, C.POINTER(C.c_void_p)])

    nvmlGetClock = nvml_api('nvmlDeviceGetClockInfo', [C.c_void_p, C.c_uint, C.POINTER(C.c_uint)])

    nvmlGetTemp = nvml_api('nvmlDeviceGetTemperature', [C.c_void_p, C.c_uint, C.POINTER(C.c_uint)])

    nvmlGetPower = nvml_api('nvmlDeviceGetPowerUsage', [C.c_void_p, C.POINTER(C.c_uint)])

    def measure_tensor_rates():
        assert 0 < cuda_attributes['sm_count'] <= 128
        blocks, threads, iters, reps = cuda_attributes['sm_count'] * 32, 256, 1024, 5
        cells = blocks * threads
        ctx, module, start_event, stop_event, nvml_device = (C.c_void_p() for _ in range(5))
        device_mem = C.c_uint64()
        nvml_ready = False
        report = {'pci': pci.lower(), 'sm_count': cuda_attributes['sm_count'], 'compute_capability': '8.9', 'ptx_sha256': hashlib.sha256(bench_ptx.encode()).hexdigest(), 'blocks': blocks, 'threads': threads, 'iterations': iters, 'independent_chains': 8, 'repetitions': reps, 'clocks_locked': False, 'hardware_configuration_writes': False, 'modes': {}}
        t0 = time.monotonic()
        def telemetry():
            result = {}
            for key, fn, args in [('sm_mhz', nvmlGetClock, (nvml_device, 1)), ('temperature_c', nvmlGetTemp, (nvml_device, 0)), ('power_mw', nvmlGetPower, (nvml_device,))]:
                value = C.c_uint()
                status = fn(*args, C.byref(value))
                result[key] = value.value if status == 0 else {'nvml_status': status}
            return result
        try:
            status = nvmlInit()
            if status:
                raise RuntimeError(f'nvmlInit failed: {status}')
            nvml_ready = True
            status = nvmlGetDevice(pci.encode('ascii'), C.byref(nvml_device))
            if status:
                raise RuntimeError(f'nvmlDeviceGetHandle failed: {status}')
            report['before'] = telemetry()
            cuda_check(cuCtxCreate(C.byref(ctx), 0, cuda_device.value), 'cuCtxCreate')
            ptx_buffer = C.create_string_buffer(bench_ptx.encode())
            error_log = C.create_string_buffer(8192)
            jit_options = (C.c_int * 2)(5, 6)
            jit_values = (C.c_void_p * 2)(C.cast(error_log, C.c_void_p), C.c_void_p(len(error_log)))
            load_status = cuModuleLoadDataEx(C.byref(module), C.cast(ptx_buffer, C.c_void_p), 2, jit_options, jit_values)
            if load_status:
                raise RuntimeError(f'PTX JIT failed ({load_status}): {error_log.value.decode(errors="replace")}')
            cuda_check(cuMemAlloc(C.byref(device_mem), cells * 4), 'cuMemAlloc')
            cuda_check(cuEventCreate(C.byref(start_event), 0), 'cuEventCreate start')
            cuda_check(cuEventCreate(C.byref(stop_event), 0), 'cuEventCreate stop')
            functions = {}
            for name, mode in BENCH_MODES.items():
                fn = C.c_void_p()
                cuda_check(cuModuleGetFunction(C.byref(fn), module, name.encode()), 'cuModuleGetFunction ' + name)
                attrs = {}
                for attr_name, attr_code in [('registers_per_thread', 4), ('local_bytes_per_thread', 3)]:
                    value = C.c_int()
                    cuda_check(cuFuncGetAttribute(C.byref(value), attr_code, fn), attr_name)
                    attrs[attr_name] = value.value
                assert attrs['local_bytes_per_thread'] == 0, (name, attrs)
                functions[name] = fn
                report['modes'][name] = {**attrs, 'instruction': mode['instruction'], 'samples': [], 'numeric_checks': []}
            def launch(name, repetitions):
                mode = BENCH_MODES[name]
                repeat_arg = C.c_uint32(repetitions)
                aval = C.c_uint32(mode['bits'])
                bval = C.c_uint32(mode['bits'])
                params = (C.c_void_p * 4)(C.cast(C.byref(repeat_arg), C.c_void_p), C.cast(C.byref(device_mem), C.c_void_p), C.cast(C.byref(aval), C.c_void_p), C.cast(C.byref(bval), C.c_void_p))
                cuda_check(cuLaunchKernel(functions[name], blocks, 1, 1, threads, 1, 1, 0, None, params, None), 'launch ' + name)
            def verify_output(name, repetitions):
                host_type = C.c_int32 if BENCH_MODES[name]['kind'] == 'int' else C.c_float
                host = (host_type * cells)()
                cuda_check(cuMemcpyDtoH(C.cast(host, C.c_void_p), device_mem.value, cells * 4), 'read output ' + name)
                expected = 4 * 8 * repetitions * BENCH_MODES[name]['k']
                mismatches = sum(1 for value in host if value != expected)
                assert mismatches == 0, (name, repetitions, expected, mismatches, host[0], host[-1])
                report['modes'][name]['numeric_checks'].append({'iterations': repetitions, 'expected': expected, 'cells_checked': cells, 'mismatches': mismatches})
            # Small numeric preflight before the measured load; verify every output.
            for name in BENCH_MODES:
                launch(name, 16)
                cuda_check(cuCtxSynchronize(), 'numeric preflight ' + name)
                verify_output(name, 16)
            # Warm-up each path; kernels are short and all parameters remain stock.
            for _ in range(3):
                for name in BENCH_MODES:
                    launch(name, iters)
            cuda_check(cuCtxSynchronize(), 'warmup')
            names = list(BENCH_MODES)
            for repetition in range(reps):
                for name in names[repetition:] + names[:repetition]:
                    before = telemetry()
                    cuda_check(cuEventRecord(start_event, None), 'record start')
                    launch(name, iters)
                    cuda_check(cuEventRecord(stop_event, None), 'record stop')
                    cuda_check(cuEventSynchronize(stop_event), 'synchronize stop')
                    elapsed = C.c_float()
                    cuda_check(cuEventElapsedTime(C.byref(elapsed), start_event, stop_event), 'elapsed time')
                    assert elapsed.value > 0
                    after = telemetry()
                    ops = (blocks * threads // 32) * iters * 8 * (16 * 8 * BENCH_MODES[name]['k'] * 2)
                    report['modes'][name]['samples'].append({'ms': elapsed.value, 'tera_ops_per_s': ops / (elapsed.value * 1e9), 'before': before, 'after': after})
                    verify_output(name, iters)
            report['after'] = telemetry()
            for result in report['modes'].values():
                result['median_tera_ops_per_s'] = statistics.median(sample['tera_ops_per_s'] for sample in result['samples'])
                result['min_tera_ops_per_s'] = min(sample['tera_ops_per_s'] for sample in result['samples'])
                result['max_tera_ops_per_s'] = max(sample['tera_ops_per_s'] for sample in result['samples'])
        finally:
            cleanup_errors = []
            for label, handle, cleanup in [('stop event', stop_event, cuEventDestroy), ('start event', start_event, cuEventDestroy), ('VRAM', device_mem, cuMemFree), ('module', module, cuModuleUnload), ('context', ctx, cuCtxDestroy)]:
                if handle.value:
                    status = cleanup(handle.value)
                    if status:
                        cleanup_errors.append((label, status))
            if nvml_ready:
                status = nvmlShutdown()
                if status:
                    cleanup_errors.append(('NVML', status))
            report['cleanup_errors'] = cleanup_errors
            if cleanup_errors:
                print({'cleanup_errors': cleanup_errors})
        report['wall_seconds'] = time.monotonic() - t0
        return report

    return measure_tensor_rates()


PTX_SHA256 = "c937fdef3ed6719e46b05869c6983149d7c8ec8ad1bbcf8ef7ca33a13a097051"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="run the bounded compute workload on the selected GPU")
    parser.add_argument("--pci", help="full PCI address of the single CUDA-visible 24-SM SM89 GPU")
    args = parser.parse_args()
    digest = hashlib.sha256(bench_ptx.encode()).hexdigest()
    if digest != PTX_SHA256:
        raise SystemExit("Generated PTX does not match the published evidence hash")
    if not args.run:
        print(json.dumps({"ptx_sha256": digest, "ptx_bytes": len(bench_ptx.encode()),
                          "gpu_access": False, "blocks": 768, "threads": 256,
                          "iterations": 1024, "independent_chains": 8, "repetitions": 5}))
        return
    if not args.pci or not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", args.pci):
        parser.error("--run requires --pci with a full domain:bus:device.function address")
    if not __debug__:
        raise SystemExit("Do not disable the architecture and numerical assertions with python -O")
    report = run_probe(args.pci)
    print(json.dumps(report, indent=2))
    if report["cleanup_errors"]:
        raise SystemExit("CUDA/NVML cleanup failed; inspect cleanup_errors")


if __name__ == "__main__":
    main()
