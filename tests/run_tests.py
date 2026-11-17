#!/usr/bin/env python3
"""End-to-end tests: round-trips against the reference 7-Zip / unzip tools, and crash injection at every checkpoint."""
import os, random, shutil, subprocess, sys, zipfile, filecmp

ROOT = os.environ.get("NZ_TEST_ROOT", "/tmp/nzt")
CLI = os.path.abspath(os.path.join(os.path.dirname(__file__), "../build/linux/nibblezip-cli"))
SEVENZ = os.environ.get("NZ_7ZZ") or shutil.which("7zz") or shutil.which("7z") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../deps/node_modules/7zip-bin-full/linux/x64/7zz"))
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
    if archive.endswith(".001"):
        # the parts joined end to end must be exactly one ordinary archive
        base = archive[:-4]; joined = base + ".joined." + fmt; n = 1
        with open(joined, "wb") as o:
            while os.path.exists("%s.%03d" % (base, n)):
                o.write(open("%s.%03d" % (base, n), "rb").read()); n += 1
        r = subprocess.run([SEVENZ, "t", joined], capture_output=True, text=True); assert r.returncode == 0, "joined parts invalid: " + r.stdout[-800:]
        if fmt == "zip":
            r = subprocess.run(["unzip", "-tq", joined], capture_output=True, text=True); assert r.returncode == 0, r.stdout + r.stderr
        os.remove(joined)
    elif fmt == "zip":
        r = subprocess.run(["unzip", "-tq", archive], capture_output=True, text=True)
        assert r.returncode == 0, "unzip -t failed: " + r.stdout + r.stderr
        with zipfile.ZipFile(archive) as z:
            assert z.testzip() is None
    if expect_subset is None and fmt == "7z":
        # mtime preserved
        assert abs(os.path.getmtime(out + "/game/readme.txt") - 1_600_000_000) < 2
    shutil.rmtree(out)
    rc, err = run(["verify", archive, "--quiet"])
    assert rc == 0, "built-in verify failed: " + err
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
    # 7a. split into parts
    PART = 300000
    for fmt in ("7z", "zip"):
        d = fresh("split_" + fmt); arc = d + "/out." + fmt
        rc, err = run(pack_args(d, fmt, step, ["--split", str(PART)])); assert rc == 0, err
        assert src_gone(d) and not os.path.exists(arc) and not os.path.exists(arc + ".nzj")
        parts = sorted(f for f in os.listdir(d) if f.startswith("out." + fmt + "."))
        sizes = [os.path.getsize(d + "/" + f) for f in parts]
        assert len(parts) > 5 and all(x == PART for x in sizes[:-1]) and 0 < sizes[-1] <= PART, (parts, sizes)
        verify(arc + ".001", fmt, ROOT + "/orig")
        # a missing or damaged part must be noticed by the checker
        shutil.copy(d + "/" + parts[3], d + "/saved"); data = bytearray(open(d + "/" + parts[3], "rb").read()); data[100] ^= 0xFF
        open(d + "/" + parts[3], "wb").write(data); rc, err = run(["verify", arc + ".001", "--quiet"]); assert rc != 0, "damage in a part not detected"
        os.remove(d + "/" + parts[3]); rc, err = run(["verify", arc + ".001", "--quiet"]); assert rc != 0, "missing part not detected"
        print(fmt, "split: %d parts OK, damage and missing part detected" % len(parts), flush=True)
        n = 1; cnt = 0
        while True:
            d = fresh("splitcrash_" + fmt); arc = d + "/out." + fmt
            rc, err = run(pack_args(d, fmt, step, ["--split", str(PART)]), crash_at=n)
            if rc == 0: break
            assert rc == 77, (rc, err)
            if n % 3 == 1 and os.path.exists(arc + ".nzj"): os.remove(arc + ".nzj")
            rc, err = run(["resume", arc, "--quiet", "--step", str(step)]); assert rc == 0, "split resume failed at %d: %s" % (n, err)
            assert src_gone(d) and not os.path.exists(arc + ".nzj"), err
            verify(arc + ".001", fmt, ROOT + "/orig")
            n += max(STRIDE, 2); cnt += 1
        print(fmt, "split crash/resume OK (%d scenarios)" % cnt, flush=True)
        d = fresh("splitpause_" + fmt); arc = d + "/out." + fmt
        rc, err = run(pack_args(d, fmt, step, ["--split", str(PART), "--pause-after", "900000"])); assert rc == 3, (rc, err)
        rc, err = run(["resume", arc, "--quiet", "--step", str(step)]); assert rc == 0, err
        verify(arc + ".001", fmt, ROOT + "/orig"); assert src_gone(d)
        d = fresh("splitearly_" + fmt); arc = d + "/out." + fmt
        rc, err = run(pack_args(d, fmt, step, ["--split", str(PART), "--finish-after", "900000"])); assert rc == 0 and "Closed early" in err, (rc, err)
        verify(arc + ".001", fmt, ROOT + "/orig", expect_subset=True)
        print(fmt, "split pause/resume and finish-early OK", flush=True)
    # 7u. undo: put a half-processed big file back
    def pause_at(d, fmt, after, extra=()):
        rc, err = run(pack_args(d, fmt, step, ["--pause-after", str(after), *extra])); assert rc == 3, (rc, err)
    for fmt in ("7z", "zip"):
        for after, expect_removed in ((900000, True), (2600000, False)):
            for extra in ((), ("--split", "300000")):
                d = fresh("undo_%s_%d_%d" % (fmt, after, len(extra))); arc = d + "/out." + fmt; first = arc + (".001" if extra else "")
                pause_at(d, fmt, after, extra)
                rc, err = run(["undo", arc, "--quiet"]); assert rc == 0, err
                assert not os.path.exists(arc + ".nzj"), "journal left after undo"
                left = tree(d + "/src"); want = tree(ROOT + "/orig")
                if expect_removed:
                    assert not os.path.exists(first), "empty archive should have been removed"
                    assert left == want, "originals not fully restored"
                else:
                    got = verify(first, fmt, ROOT + "/orig", expect_subset=True)
                    for k, v in want.items():
                        if v is None or v == b"": continue
                        assert (k in got) != (k in left), "file in both or neither: " + k
                        assert got.get(k, left.get(k)) == v, "content differs after undo: " + k
        # crash at every checkpoint of the undo itself, then finish the undo, or resume forwards instead
        n = 1; cnt = 0
        while True:
            d = fresh("undocrash_" + fmt); arc = d + "/out." + fmt
            pause_at(d, fmt, 2600000)
            rc, err = run(["undo", arc, "--quiet"], crash_at=n)
            if rc == 0: break
            assert rc == 77, (rc, err)
            if n % 2:
                rc, err = run(["undo", arc, "--quiet"])
                if rc != 0: assert "nothing to put back" in err, err; rc, err = run(["resume", arc, "--finalize", "--quiet"]); assert rc == 0, err
                got = verify(arc, fmt, ROOT + "/orig", expect_subset=True); left = tree(d + "/src"); want = tree(ROOT + "/orig")
                for k, v in want.items():
                    if v is None or v == b"": continue
                    assert got.get(k, left.get(k)) == v and ((k in got) != (k in left)), "after crashed undo: " + k
            else:
                rc, err = run(["resume", arc, "--quiet", "--step", str(step)]); assert rc == 0, err
                assert src_gone(d); verify(arc, fmt, ROOT + "/orig")
            n += 1; cnt += 1
        print(fmt, "undo OK (plain and split, archive kept and removed, %d crash scenarios)" % cnt, flush=True)
    # 7e. size estimate is in the right region
    for fmt in ("7z", "zip"):
        d = fresh("est_" + fmt)
        p = subprocess.run(PREFIX + [CLI, "estimate", "-f", fmt, d + "/src/game", d + "/src/single file.bin"], env=ENV, capture_output=True, text=True, cwd=ROOT)
        tot_b, est = map(int, p.stdout.split())
        rc, err = run(pack_args(d, fmt, step, ["--keep"])); assert rc == 0
        real = os.path.getsize(d + "/out." + fmt)
        assert 0.6 * real < est < 1.6 * real, (tot_b, est, real)
        print(fmt, "estimate %d vs real %d (%.0f%%)" % (est, real, 100.0 * est / real), flush=True)
    # 7b. the built-in checker must catch damage, and must accept archives made by real 7-Zip
    for fmt in ("7z", "zip"):
        d = fresh("corrupt_" + fmt)
        rc, err = run(pack_args(d, fmt, step, ["--keep"])); assert rc == 0, err
        arc = d + "/out." + fmt; size = os.path.getsize(arc); caught = 0
        for frac in (0.1, 0.35, 0.6, 0.8):
            data = bytearray(open(arc, "rb").read()); pos = int(size * frac); data[pos] ^= 0x5A
            open(arc + ".bad." + fmt, "wb").write(data)
            rc, err = run(["verify", arc + ".bad." + fmt, "--quiet"])
            caught += rc != 0
        assert caught == 4, "checker missed corruption (%d/4)" % caught
        ref = d + "/ref." + fmt
        r = subprocess.run([SEVENZ, "a", "-mx1", ref, d + "/src/game"], capture_output=True, text=True); assert r.returncode == 0
        rc, err = run(["verify", ref, "--quiet"]); assert rc == 0, "checker rejected a 7-Zip-made archive: " + err
        print(fmt, "checker catches corruption and accepts 7-Zip-made archives OK", flush=True)
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
