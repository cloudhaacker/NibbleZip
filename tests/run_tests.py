#!/usr/bin/env python3
"""End-to-end tests: round-trips against the reference 7-Zip / unzip tools, and crash injection at every checkpoint."""
import os, random, shutil, subprocess, sys, zipfile, filecmp

ROOT = os.environ.get("NZ_TEST_ROOT", "/tmp/nzt")
CLI = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/linux/nibblezip-cli"))
SEVENZ = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../deps/node_modules/7zip-bin-full/linux/x64/7zz"))
ENV = dict(os.environ, NIBBLEZIP_HOME=ROOT + "/home")

def make_dataset(dst, seed=1):
    rnd = random.Random(seed)
    os.makedirs(dst + "/game/data/deep/deeper")
    os.makedirs(dst + "/game/empty dir")
    os.makedirs(dst + "/game/empty parent/child")
    w = lambda p, b: open(dst + "/" + p, "wb").write(b)
    w("game/readme.txt", b"hello world\n" * 200)
    w("game/zero.bin", b"")
    w("game/data/zero2", b"")
    w("game/data/text.log", ("line %d of the log\n" * 1).encode() * 30000)
    w("game/data/random.bin", rnd.randbytes(300_000))
    w("game/data/big_random.pak", rnd.randbytes(1_500_000))
    w("game/data/big_text.iso", b"".join(b"%08d some repeating payload\n" % rnd.randrange(5000) for _ in range(90000)))
    w("game/data/exact_step.bin", rnd.randbytes(262144))
    w("game/data/deep/deeper/ゲーム ünïcöde \U0001F600.dat", rnd.randbytes(5000))
    for i in range(40):
        w("game/data/deep/small_%02d.cfg" % i, (b"key=%d\n" % i) * rnd.randrange(1, 400))
    w("single file.bin", rnd.randbytes(700_000) + b"A" * 400_000)
    os.utime(dst + "/game/readme.txt", (1_600_000_000, 1_600_000_000))

PREFIX = os.environ.get("NZ_CLI_PREFIX", "").split()      # e.g. "wine64" to test the Windows build
if PREFIX: CLI = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/win/nibblezip-cli.exe"))
STRIDE = int(os.environ.get("NZ_CRASH_STRIDE", "1"))

def run(args, crash_at=None, **kw):
    env = dict(ENV)
    if crash_at: env["NIBBLEZIP_CRASH_AT"] = str(crash_at)
    env.update(kw.get("env", {}))
    p = subprocess.run(PREFIX + [CLI] + args, env=env, capture_output=True, text=True, errors="replace", cwd=ROOT)
    return p.returncode, p.stderr

def tree(d):
    out = {}
    for base, dirs, files in os.walk(d):
        rel = os.path.relpath(base, d)
        out[rel + "/"] = None
        for f in files:
            p = os.path.join(base, f)
            out[os.path.normpath(os.path.join(rel, f))] = open(p, "rb").read()
    return out

def verify(archive, fmt, orig, expect_subset=None):
    r = subprocess.run([SEVENZ, "t", archive], capture_output=True, text=True)
    assert r.returncode == 0, "7zz t failed:\n" + r.stdout + r.stderr
    out = archive + ".x"
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([SEVENZ, "x", "-y", "-o" + out, archive], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    got, want = tree(out), tree(orig)
    if expect_subset is None:
        assert got.keys() == want.keys(), "tree mismatch: missing %s extra %s" % (sorted(want.keys() - got.keys()), sorted(got.keys() - want.keys()))
    for k, v in got.items():
        assert k in want and want[k] == v, "content mismatch " + k
    if fmt == "zip":
        r = subprocess.run(["unzip", "-tq", archive], capture_output=True, text=True)
        assert r.returncode == 0, "unzip -t failed: " + r.stdout + r.stderr
        with zipfile.ZipFile(archive) as z:
            assert z.testzip() is None
    if expect_subset is None and fmt == "7z":
        # mtime preserved
        assert abs(os.path.getmtime(out + "/game/readme.txt") - 1_600_000_000) < 2
    shutil.rmtree(out)
    return got

def fresh(name):
    d = ROOT + "/" + name
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copytree(ROOT + "/orig", d + "/src", symlinks=True)
    for base, dirs, files in os.walk(d + "/src"):
        pass
    shutil.copystat(ROOT + "/orig/game/readme.txt", d + "/src/game/readme.txt")
    os.utime(d + "/src/game/readme.txt", (1_600_000_000, 1_600_000_000))
    return d

def src_gone(d):
    return not os.path.exists(d + "/src/game") and not os.path.exists(d + "/src/single file.bin")

def pack_args(d, fmt, step, extra=()):
    return ["pack", "-f", fmt, "-l", "1", "--quiet", "--step", str(step), "-o", d + "/out." + fmt, d + "/src/game", d + "/src/single file.bin", *extra]

def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    os.makedirs(ROOT + "/home")
    make_dataset(ROOT + "/orig")
    step = 262144
    total = 0
    for fmt in ("7z", "zip"):
        shutil.rmtree(ROOT + "/home/journals", ignore_errors=True)
        # 1. plain eat run
        d = fresh("plain_" + fmt)
        rc, err = run(pack_args(d, fmt, step))
        assert rc == 0, err
        assert src_gone(d), "sources not removed: " + err
        assert not os.path.exists(d + "/out.%s.nzj" % fmt)
        assert os.listdir(ROOT + "/home/journals") == [], "mirror journal left behind"
        verify(d + "/out." + fmt, fmt, ROOT + "/orig")
        print(fmt, "plain eat run OK,", os.path.getsize(d + "/out." + fmt), "bytes", flush=True)

        # 2. keep mode leaves sources alone
        d = fresh("keep_" + fmt)
        rc, err = run(pack_args(d, fmt, step, ["--keep"]))
        assert rc == 0, err
        assert tree(d + "/src").keys() == tree(ROOT + "/orig").keys()
        verify(d + "/out." + fmt, fmt, ROOT + "/orig")
        print(fmt, "keep mode OK")

        # 3. production step sizes (multi-threaded path), no --step
        d = fresh("prod_" + fmt)
        rc, err = run(["pack", "-f", fmt, "-l", "3", "--quiet", "-o", d + "/out." + fmt, d + "/src/game", d + "/src/single file.bin"])
        assert rc == 0, err
        assert src_gone(d)
        verify(d + "/out." + fmt, fmt, ROOT + "/orig")
        print(fmt, "production settings OK")

        # 4. crash at every checkpoint, then resume
        n = 1
        while True:
            d = fresh("crash_" + fmt)
            rc, err = run(pack_args(d, fmt, step), crash_at=n)
            if rc == 0:
                break
            assert rc == 77, "unexpected rc %d at checkpoint %d: %s" % (rc, n, err)
            where = err.strip().splitlines()[-1]
            variant = n % 4
            if variant == 1:      # primary journal lost: must recover from the mirror copy
                os.remove(d + "/out.%s.nzj" % fmt)
            elif variant == 2:    # crash again during the resume, then resume once more
                rc2, err2 = run(["resume", d + "/out." + fmt, "--quiet", "--step", str(step)], crash_at=3)
                assert rc2 in (0, 77), err2
            elif variant == 3:    # mirror journal lost
                for f in os.listdir(ROOT + "/home/journals"): os.remove(ROOT + "/home/journals/" + f)
            rc, err = run(["resume", d + "/out." + fmt, "--quiet", "--step", str(step)])
            if rc != 0 and "No unfinished job" in err and variant == 2:
                pass
            else:
                assert rc == 0, "resume failed after %s: %s" % (where, err)
            assert src_gone(d), "sources left after resume (%s): %s" % (where, err)
            assert not os.path.exists(d + "/out.%s.nzj" % fmt)
            verify(d + "/out." + fmt, fmt, ROOT + "/orig")
            n += STRIDE; total += 1
        print(fmt, "crash/resume OK,", total, "scenarios so far", flush=True)

        # 5. pause mid-way, resume
        d = fresh("pause_" + fmt)
        rc, err = run(pack_args(d, fmt, step, ["--pause-after", "900000"]))
        assert rc == 3, (rc, err)
        assert os.path.exists(d + "/out.%s.nzj" % fmt)
        rc, err = run(["pending"]); 
        rc, err = run(["resume", d + "/out." + fmt, "--quiet", "--step", str(step)])
        assert rc == 0, err
        assert src_gone(d)
        verify(d + "/out." + fmt, fmt, ROOT + "/orig")
        print(fmt, "pause/resume OK")

        # 6. finish early: valid archive of a subset, the rest of the sources untouched
        d = fresh("early_" + fmt)
        rc, err = run(pack_args(d, fmt, step, ["--finish-after", "900000"]))
        assert rc == 0 and "Closed early" in err, (rc, err)
        got = verify(d + "/out." + fmt, fmt, ROOT + "/orig", expect_subset=True)
        left = tree(d + "/src"); want = tree(ROOT + "/orig")
        for k, v in want.items():
            if v is None: continue
            assert (k in got) != (k in left) or v == b"", "file neither/both in archive and source: " + k
            assert (got.get(k, left.get(k)) == v)
        assert any(v for v in left.values()), "nothing left?"
        print(fmt, "finish-early OK (%d files archived, %d left)" % (sum(1 for v in got.values() if v is not None), sum(1 for v in left.values() if v is not None)))

        # 7. pause then finalize-only at a file boundary
        d = fresh("fin_" + fmt)
        rc, err = run(pack_args(d, fmt, step, ["--pause-after", "400000"]))
        assert rc == 3, (rc, err)
        rc, err = run(["resume", d + "/out." + fmt, "--finalize", "--quiet"])
        if rc == 0:
            verify(d + "/out." + fmt, fmt, ROOT + "/orig", expect_subset=True)
            print(fmt, "finalize-only OK")
        else:
            assert "half-processed" in err, err
            print(fmt, "finalize-only correctly refused mid-file")
    # 8. refusal cases
    d = fresh("refuse")
    open(d + "/out.7z", "wb").write(b"x")
    rc, err = run(pack_args(d, "7z", step))
    assert rc == 1 and "already exists" in err, err
    assert open(d + "/out.7z", "rb").read() == b"x" and not src_gone(d)
    print("refuses to overwrite an existing archive OK")
    print("ALL TESTS PASSED (%d crash scenarios)" % total)

if __name__ == "__main__":
    main()
