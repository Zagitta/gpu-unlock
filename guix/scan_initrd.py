import sys, gzip, subprocess

def load(path):
    data = open(path, "rb").read()
    if data[:2] == b"\x1f\x8b":
        return gzip.decompress(data)
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)
    except ImportError:
        out = subprocess.run(["zstdcat", path], capture_output=True)
        if out.returncode == 0:
            return out.stdout
        raise SystemExit(f"cannot decompress {path}: magic={data[:4]!r}")

for path in sys.argv[1:]:
    data = load(path)
    print(f"== {path.split('/')[-1]} ({len(data)//1048576} MiB)")
    for needle in (b"gpu-unlock", b"gsp_ga10x.bin", b"falcon_loader.bin",
                   b"nvidia/580.178.04", b"gpu_unlock"):
        print(f"   {needle.decode():24s} x{data.count(needle)}")
