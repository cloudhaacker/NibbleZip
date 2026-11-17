#!/usr/bin/env python3
"""Large-file test: >4 GB sources, real hole punching, peak disk usage, multi-threaded blocks."""
import os, random, shutil, subprocess, sys, threading, time, hashlib
ROOT = "/tmp/nzbig"
CLI = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/linux/nibblezip-cli"))
SEVENZ = os.environ.get("NZ_7ZZ") or shutil.which("7zz") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../deps/node_modules/7zip-bin-full/linux/x64/7zz"))
ENV = dict(os.environ, NIBBLEZIP_HOME=ROOT + "/home", NIBBLEZIP_CPUS="4")

def used(d):
    return int(subprocess.run(["du", "-sk", d], capture_output=True, text=True).stdout.split()[0]) * 1024

def make(path, size, seed):
    rnd = random.Random(seed); block = rnd.randbytes(1 << 20); h = hashlib.sha1()
    with open(path, "wb") as f:
        n = 0; i = 0
        while n < size:
            b = (block[i % 4096:] + block[:i % 4096]) if i % 3 else rnd.randbytes(1 << 20)
            b = b[:size - n]; f.write(b); h.update(b); n += len(b); i += 1
    return h.hexdigest()

def sha(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b: break
            h.update(b)
    return h.hexdigest()

def one(fmt, level, size, crash=None):
    d = ROOT + "/" + fmt
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d + "/src/sub")
    hashes = {"big.bin": make(d + "/src/big.bin", size, 7), "sub/after.bin": make(d + "/src/sub/after.bin", 3_000_000, 8)}
    open(d + "/src/sub/tiny.txt", "w").write("tiny"); hashes["sub/tiny.txt"] = sha(d + "/src/sub/tiny.txt")
    start = used(d); peak = [start]; stop = [False]
    def watch():
        while not stop[0]:
            peak[0] = max(peak[0], used(d)); time.sleep(0.5)
    t = threading.Thread(target=watch); t.start(); t0 = time.time()
    env = dict(ENV)
    if crash: env["NIBBLEZIP_CRASH_AT"] = str(crash)
    r = subprocess.run([CLI, "pack", "-f", fmt, "-l", str(level), "--quiet", "-o", d + "/out." + fmt, d + "/src"], env=env, capture_output=True, text=True)
    if crash:
        assert r.returncode == 77, r.stderr
        r = subprocess.run([CLI, "resume", d + "/out." + fmt, "--quiet"], env=ENV, capture_output=True, text=True)
    stop[0] = True; t.join()
    assert r.returncode == 0, r.stderr
    assert not os.path.exists(d + "/src"), "source left: " + r.stderr
    arc = os.path.getsize(d + "/out." + fmt)
    print("%s: %.2f GB -> %.2f GB in %.0fs; peak disk use %.2f GB (start %.2f GB)" % (fmt, size / 2**30, arc / 2**30, time.time() - t0, peak[0] / 2**30, start / 2**30), flush=True)
    assert peak[0] < start + 1.3 * 2**30, "peak usage too high: hole punching not effective"
    r = subprocess.run([SEVENZ, "t", d + "/out." + fmt], capture_output=True, text=True); assert r.returncode == 0, r.stdout[-2000:]
    r = subprocess.run([SEVENZ, "x", "-y", "-o" + d + "/x", d + "/out." + fmt], capture_output=True, text=True); assert r.returncode == 0, r.stdout[-2000:]
    for k, v in hashes.items(): assert sha(d + "/x/src/" + k) == v, "hash mismatch " + k
    shutil.rmtree(d + "/x")
    if fmt == "zip":
        r = subprocess.run(["unzip", "-tq", d + "/out.zip"], capture_output=True, text=True); assert r.returncode == 0, r.stdout + r.stderr
    shutil.rmtree(d)
    print(fmt, "verified with 7-Zip" + (" and unzip" if fmt == "zip" else ""), flush=True)

shutil.rmtree(ROOT, ignore_errors=True); os.makedirs(ROOT + "/home")
size = int(float(sys.argv[1]) * 2**30) if len(sys.argv) > 1 else int(4.6 * 2**30)
one("zip", 0, size, crash=40)
one("7z", 0, size, crash=40)
print("BIG TEST PASSED")
