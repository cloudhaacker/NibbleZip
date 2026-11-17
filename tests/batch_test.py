#!/usr/bin/env python3
"""Tests for 'one archive per item / per file' mode, including pause and crash recovery mid-batch."""
import os, shutil, subprocess, sys, random
ROOT = os.environ.get("NZ_TEST_ROOT", "/tmp/nzb")
HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "../build/linux/nibblezip-cli")
PREFIX = os.environ.get("NZ_CLI_PREFIX", "").split()
if PREFIX: CLI = os.path.join(HERE, "../build/win/nibblezip-cli.exe")
SEVENZ = os.environ.get("NZ_7ZZ") or shutil.which("7zz") or shutil.which("7z") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../deps/node_modules/7zip-bin-full/linux/x64/7zz"))
STRIDE = int(os.environ.get("NZ_CRASH_STRIDE", "5"))
ENV = dict(os.environ, NIBBLEZIP_HOME=ROOT + "/home")

def run(args, crash_at=None):
    env = dict(ENV)
    if crash_at: env["NIBBLEZIP_CRASH_AT"] = str(crash_at)
    p = subprocess.run(PREFIX + [CLI] + args, env=env, capture_output=True, text=True, errors="replace", cwd=ROOT)
    return p.returncode, p.stderr

def make(dst):
    rnd = random.Random(4)
    os.makedirs(dst + "/roms/sub/deeper"); os.makedirs(dst + "/roms/emptydir")
    w = lambda p, b: open(dst + "/" + p, "wb").write(b)
    for i in range(14): w("roms/game %02d.nes" % i, rnd.randbytes(rnd.randrange(2000, 60000)) + b"\0" * 20000)
    w("roms/dup.a", b"first" * 300); w("roms/dup.b", b"second" * 300)
    w("roms/noext", b"plain" * 1000)
    w("roms/zero.bin", b"")
    w("roms/big.iso", rnd.randbytes(700_000) + b"text " * 60000)
    w("roms/sub/inner.gb", rnd.randbytes(30000)); w("roms/sub/deeper/ゲーム.sfc", rnd.randbytes(9000))
    w("solo.bin", rnd.randbytes(50000))

def files(d):
    out = {}
    for b, _, fs in os.walk(d):
        for f in fs: out[os.path.relpath(os.path.join(b, f), d)] = open(os.path.join(b, f), "rb").read()
    return out

def contents(arc):
    r = subprocess.run([SEVENZ, "t", arc], capture_output=True, text=True); assert r.returncode == 0, arc + r.stdout[-500:]
    rc, err = run(["verify", arc, "--quiet"]); assert rc == 0, arc + err
    x = arc + ".x"; shutil.rmtree(x, ignore_errors=True)
    r = subprocess.run([SEVENZ, "x", "-y", "-o" + x, arc], capture_output=True, text=True); assert r.returncode == 0
    f = files(x); shutil.rmtree(x); return f

def fresh(name):
    d = ROOT + "/" + name; shutil.rmtree(d, ignore_errors=True); shutil.copytree(ROOT + "/orig", d); return d

def check_per_file_inplace(d, fmt, orig, kept=False):
    ext = "." + fmt; now = files(d); seen = set()
    for rel, data in orig.items():
        base, name = os.path.split(rel); stem = os.path.splitext(name)[0] if "." in name[1:] else name
        cands = [os.path.join(base, stem + ext), os.path.join(base, name + ext)]
        arc = next((c for c in cands if c in now and c not in seen and name in contents(os.path.join(d, c))), None)
        assert arc, "no archive for " + rel + " in " + str(sorted(now))
        seen.add(arc)
        got = contents(os.path.join(d, arc)); assert got == {name: data}, "wrong content in " + arc
        assert (rel in now) == kept, ("original should be %s: " % ("kept" if kept else "gone")) + rel
    extra = set(now) - seen - (set(orig) if kept else set())
    assert not extra, "unexpected files: " + str(extra)

def main():
    shutil.rmtree(ROOT, ignore_errors=True); os.makedirs(ROOT + "/home"); make(ROOT + "/orig")
    step = "262144"
    for fmt in ("zip", "7z"):
        # per file, in place
        d = fresh("pf_" + fmt); orig = files(d + "/roms")
        rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, d + "/roms"]); assert rc == 0, err
        check_per_file_inplace(d + "/roms", fmt, orig)
        assert os.path.isdir(d + "/roms/emptydir") and os.path.exists(d + "/solo.bin")
        # running it again does nothing and harms nothing
        before = files(d); rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", d + "/roms"]); assert rc == 1 and "Nothing to do" in err, err
        assert files(d) == before
        print(fmt, "per-file in place OK", flush=True)
        # existing archive with the same name: the item is skipped and its original kept
        d = fresh("ex_" + fmt); open(d + "/roms/noext." + fmt, "wb").write(b"not really an archive")
        rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, d + "/roms"]); assert rc == 0, err
        assert os.path.exists(d + "/roms/noext") and open(d + "/roms/noext." + fmt, "rb").read() == b"not really an archive"
        print(fmt, "existing archive left alone OK", flush=True)
        # per item into an output folder
        d = fresh("pi_" + fmt); os.makedirs(d + "/out"); orig = files(d + "/roms")
        rc, err = run(["pack", "--each", "item", "--outdir", d + "/out", "-f", fmt, "--quiet", "--step", step, d + "/roms", d + "/solo.bin"]); assert rc == 0, err
        assert sorted(os.listdir(d + "/out")) == ["roms." + fmt, "solo." + fmt], os.listdir(d + "/out")
        got = contents(d + "/out/roms." + fmt); assert got == {"roms/" + k: v for k, v in orig.items()}
        assert not os.path.exists(d + "/roms") and not os.path.exists(d + "/solo.bin")
        print(fmt, "per-item into output folder OK", flush=True)
        # per file into a mirrored output folder, originals kept
        d = fresh("pm_" + fmt); os.makedirs(d + "/out"); orig = files(d + "/roms")
        rc, err = run(["pack", "--each", "file", "--outdir", d + "/out", "--keep", "-f", fmt, "--quiet", "--step", step, d + "/roms"]); assert rc == 0, err
        assert files(d + "/roms") == orig
        check_per_file_inplace(d + "/out/roms", fmt, orig)
        print(fmt, "per-file mirrored, originals kept OK", flush=True)
        # pause in the middle, then run the same command again
        d = fresh("pa_" + fmt); orig = files(d + "/roms")
        rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, "--pause-after", "400000", d + "/roms"]); assert rc == 3, (rc, err)
        rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, d + "/roms"]); assert rc == 0, err
        check_per_file_inplace(d + "/roms", fmt, orig)
        print(fmt, "pause mid-batch and continue OK", flush=True)
        # crash at sampled checkpoints across the whole batch, then run the same command again
        n = 1; count = 0
        while True:
            d = fresh("cr_" + fmt); orig = files(d + "/roms")
            rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, d + "/roms"], crash_at=n)
            if rc == 0: break
            assert rc == 77, (rc, err)
            rc, err = run(["pack", "--each", "file", "-f", fmt, "--quiet", "--step", step, d + "/roms"]); assert rc == 0, "after crash %d: %s" % (n, err)
            check_per_file_inplace(d + "/roms", fmt, orig)
            assert not [f for f in files(d) if f.endswith(".nzj")], "journal left behind"
            n += STRIDE; count += 1
        print(fmt, "crash mid-batch and continue OK (%d scenarios)" % count, flush=True)
    log = open(ROOT + "/home/deleted-files.log").read()
    assert "game 00.nes" in log and "deleted" in log and "stored in:" in log, "deletion log missing entries"
    print("deletion log OK (%d entries)" % log.count("stored in:"))
    print("BATCH TESTS PASSED")

main()
