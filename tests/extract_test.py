#!/usr/bin/env python3
"""Tests for extraction, including space-reclaiming extraction with a crash injected at every checkpoint.

Archives are made by NibbleZip itself and by reference tools (7-Zip, Python's zipfile) in many layouts:
multi-block and single-block LZMA2, LZMA, stored, x86-filtered executables, Deflate, ZIP64, split parts.
Every result is compared with what 7-Zip extracts from the same archive.
"""
import hashlib, os, random, shutil, subprocess, sys, warnings, zipfile
warnings.filterwarnings("ignore", message="Duplicate name")

ROOT = os.environ.get("NZ_TEST_ROOT", "/tmp/nzx")
HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "../build/linux/nibblezip-cli")
PREFIX = os.environ.get("NZ_CLI_PREFIX", "").split()
if PREFIX: CLI = os.path.join(HERE, "../build/win/nibblezip-cli.exe")
SEVENZ = os.environ.get("NZ_7ZZ") or shutil.which("7zz") or shutil.which("7z") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../deps/node_modules/7zip-bin-full/linux/x64/7zz"))
STRIDE = int(os.environ.get("NZ_CRASH_STRIDE", "3"))
ENV = dict(os.environ, NIBBLEZIP_HOME=ROOT + "/home")
IV = "300000"   # commit interval used in the tests, so that even small archives have many restart points

def run(args, crash_at=None):
    env = dict(ENV)
    if crash_at: env["NIBBLEZIP_CRASH_AT"] = str(crash_at)
    p = subprocess.run(PREFIX + [CLI] + args, env=env, capture_output=True, text=True, errors="replace", cwd=ROOT)
    return p.returncode, p.stderr

def sz(*a):
    r = subprocess.run([SEVENZ, *a], capture_output=True, text=True)
    assert r.returncode == 0, (a, r.stdout[-1500:], r.stderr[-500:])

def make_data(dst):
    rnd = random.Random(11)
    os.makedirs(dst + "/pack/sub/deeper"); os.makedirs(dst + "/pack/empty dir")
    w = lambda p, b: open(dst + "/" + p, "wb").write(b)
    words = [bytes(rnd.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(rnd.randint(2, 9))) for _ in range(4000)]
    text = lambda n: b" ".join(rnd.choice(words) for _ in range(n // 6))[:n]
    w("pack/readme.txt", text(4000))
    w("pack/zero.bin", b"")
    w("pack/big_text.dat", text(2_600_000))
    w("pack/big_random.pak", rnd.randbytes(1_400_000))
    w("pack/mixed.bin", rnd.randbytes(500_000) + text(700_000) + rnd.randbytes(300_000))
    # x86-looking code, so that 7-Zip applies its executable filter
    code = bytearray(text(600_000))
    for i in range(0, len(code) - 5, 37): code[i] = 0xE8; code[i + 1:i + 5] = (i * 7 % 50000).to_bytes(4, "little")
    w("pack/tool.exe", bytes(code)); w("pack/sub/helper.dll", bytes(code[:200_000]))
    w("pack/sub/deeper/\u30b2\u30fc\u30e0 \u00fcn\u00efc\u00f6de.sav", rnd.randbytes(7000))
    for i in range(30): w("pack/sub/small_%02d.cfg" % i, (b"key=%d\n" % i) * rnd.randrange(1, 500))
    os.utime(dst + "/pack/readme.txt", (1_600_000_000, 1_600_000_000))

def tree(d):
    out = {}
    for base, dirs, files in os.walk(d):
        out[os.path.relpath(base, d) + "/"] = None
        for f in files: out[os.path.normpath(os.path.join(os.path.relpath(base, d), f))] = open(os.path.join(base, f), "rb").read()
    return out

def parts_of(arc):
    if arc.endswith(".001"):
        base = arc[:-4]; n = 1; out = []
        while os.path.exists("%s.%03d" % (base, n)): out.append("%s.%03d" % (base, n)); n += 1
        return out
    return [arc]

def digest(arc):
    h = hashlib.sha1()
    for p in parts_of(arc): h.update(open(p, "rb").read())
    return h.hexdigest()

def stage(arc, name):
    """Copies an archive (all its parts) into a fresh working folder and returns the new path."""
    d = ROOT + "/work/" + name; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    for p in parts_of(arc): shutil.copy2(p, d)
    return d + "/" + os.path.basename(arc)

def base_of(arc): return arc[:-4] if arc.endswith(".001") else arc
def dest_of(arc): return os.path.splitext(base_of(arc))[0]
def leftovers(arc):
    return sorted(os.listdir(os.path.dirname(arc)))

def build_archives(A, data):
    arcs = {}
    def nz(name, fmt, extra=()):
        d = ROOT + "/mk"; shutil.rmtree(d, ignore_errors=True); shutil.copytree(data, d)
        out = "%s/%s.%s" % (A, name, fmt)
        rc, err = run(["pack", "-f", fmt, "--keep", "--quiet", "--step", "262144", *extra, "-o", out, d + "/pack"]); assert rc == 0, err
        return out
    arcs["nz-7z"] = nz("nz", "7z"); arcs["nz-zip"] = nz("nz", "zip")
    arcs["nz-7z-split"] = nz("nzs", "7z", ["--split", "300000"]) + ".001"
    arcs["nz-zip-split"] = nz("nzs", "zip", ["--split", "300000"]) + ".001"
    def z7(name, *sw, ext="7z"):
        out = "%s/%s.%s" % (A, name, ext); subprocess.run([SEVENZ, "a", *sw, out, "pack"], cwd=data, capture_output=True, check=True); return out
    arcs["7z-default"] = z7("default", "-mx5")
    arcs["7z-blocks"] = z7("blocks", "-m0=lzma2:d=64k:c=256k", "-mmt2")
    arcs["7z-solid-1thread"] = z7("solid1", "-m0=lzma2:d=1m", "-mmt1", "-ms=on")
    arcs["7z-lzma1"] = z7("lzma1", "-m0=lzma:d=1m")
    arcs["7z-copy"] = z7("copy", "-m0=copy")
    arcs["7z-nonsolid"] = z7("nonsolid", "-ms=off", "-mx3")
    arcs["zip-7zip"] = z7("sevenzip", "-tzip", "-mx5", ext="zip")
    out = A + "/python.zip"
    with zipfile.ZipFile(out, "w") as z:
        for base, dirs, files in os.walk(data + "/pack"):
            rel = os.path.relpath(base, data)
            z.writestr(rel.replace(os.sep, "/") + "/", b"")
            for i, f in enumerate(sorted(files)):
                z.write(os.path.join(base, f), rel.replace(os.sep, "/") + "/" + f, compress_type=zipfile.ZIP_STORED if i % 3 == 0 else zipfile.ZIP_DEFLATED)
    arcs["zip-python-mixed"] = out
    out = A + "/zip64.zip"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in ("big_text.dat", "mixed.bin", "readme.txt"):
            with z.open(zipfile.ZipInfo("pack/" + f), "w", force_zip64=True) as o: o.write(open(data + "/pack/" + f, "rb").read())
    arcs["zip-zip64"] = out
    return arcs

def expect_tree(dest, ref, what):
    got, want = tree(dest), tree(ref)
    assert got.keys() == want.keys(), "%s: tree differs. missing %s extra %s" % (what, sorted(want.keys() - got.keys())[:5], sorted(got.keys() - want.keys())[:5])
    for k, v in got.items(): assert v == want[k], "%s: content differs in %s" % (what, k)

def main():
    shutil.rmtree(ROOT, ignore_errors=True); os.makedirs(ROOT + "/home"); os.makedirs(ROOT + "/arcs")
    make_data(ROOT + "/data")
    arcs = build_archives(ROOT + "/arcs", ROOT + "/data")
    total_crash = 0
    for name, arc in arcs.items():
        ref = ROOT + "/ref/" + name; shutil.rmtree(ref, ignore_errors=True)
        sz("x", "-y", "-o" + ref, arc)
        # 1. plain extraction keeps the archive untouched
        a = stage(arc, name + "-keep"); before = digest(a)
        rc, err = run(["extract", a, "--keep", "--quiet", "--interval", IV, "--dest", ROOT + "/work/" + name + "-keep/out"]); assert rc == 0, (name, err)
        expect_tree(ROOT + "/work/" + name + "-keep/out", ref, name + " keep")
        assert digest(a) == before and not [f for f in leftovers(a) if f.endswith(".nzx")], name
        # 2. space-reclaiming extraction removes the archive at the end
        a = stage(arc, name + "-eat")
        rc, err = run(["extract", a, "--quiet", "--interval", IV]); assert rc == 0, (name, err)
        expect_tree(dest_of(a), ref, name + " eat")
        assert leftovers(a) == [os.path.basename(dest_of(a))], (name, leftovers(a))
        if "7z" in name.split("-")[0] or name.startswith("nz-7z"):
            assert abs(os.path.getmtime(dest_of(a) + "/pack/readme.txt") - 1_600_000_000) < 2, name + ": timestamp not restored"
        assert not os.listdir(ROOT + "/home/journals"), "mirror journal left behind"
        # 3. crash at sampled checkpoints, then run the same command again
        n = 1; cnt = 0; hollowed = False
        while True:
            a = stage(arc, name + "-crash")
            rc, err = run(["extract", a, "--quiet", "--interval", IV], crash_at=n)
            if rc == 0: break
            assert rc == 77, (name, n, rc, err)
            size_now = sum(os.stat(p).st_blocks * 512 for p in parts_of(a) if os.path.exists(p)) if not a.endswith(".001") else \
                       sum(os.stat(os.path.join(os.path.dirname(a), f)).st_blocks * 512 for f in leftovers(a) if f[-3:].isdigit())
            if size_now < 0.7 * sum(os.path.getsize(p) for p in parts_of(arc)): hollowed = True
            if n % 2 and os.path.exists(base_of(a) + ".nzx"): os.remove(base_of(a) + ".nzx")   # primary journal lost
            rc2 = 77
            if n % 5 == 0:   # crash again during the resume
                rc2, err2 = run(["extract", a, "--quiet", "--interval", IV], crash_at=2); assert rc2 in (0, 77), err2
            if rc2 != 0:
                rc, err = run(["extract", a, "--quiet", "--interval", IV])
                assert rc == 0, (name, n, err)
            expect_tree(dest_of(a), ref, "%s after crash %d" % (name, n))
            assert leftovers(a) == [os.path.basename(dest_of(a))], (name, n, leftovers(a))
            n += STRIDE; cnt += 1
        total_crash += cnt
        # 4. pause and continue
        a = stage(arc, name + "-pause")
        rc, err = run(["extract", a, "--quiet", "--interval", IV, "--pause-after", "9000000"])
        if rc == 3:
            rc, err = run(["extract", a, "--quiet", "--interval", IV])
        assert rc == 0, (name, err)
        expect_tree(dest_of(a), ref, name + " pause")
        print("%-18s OK  (%d crash scenarios%s)" % (name, cnt, ", archive seen shrinking" if hollowed else ""), flush=True)
        shutil.rmtree(ROOT + "/work", ignore_errors=True)

    # an archive holding one folder with the archive's own name does not get nested twice
    for src, ext in (("7z-default", "7z"), ("zip-7zip", "zip"), ("nz-7z", "7z")):
        d = ROOT + "/work/same-" + src; shutil.rmtree(d, ignore_errors=True); os.makedirs(d); shutil.copy2(arcs[src], d + "/pack." + ext)
        rc, err = run(["extract", d + "/pack." + ext, "--quiet", "--interval", IV]); assert rc == 0, err
        expect_tree(d + "/pack", ROOT + "/ref/" + src + "/pack", "same-name " + src)
        assert sorted(os.listdir(d)) == ["pack"], os.listdir(d)
    print("archive named after its single top folder is not nested twice OK", flush=True)
    # unfinished jobs claim what they work on: nothing else may touch it until they are finished
    a = stage(arcs["nz-7z"], "claims"); wd = os.path.dirname(a); dest = dest_of(a)
    rc, err = run(["extract", a, "--quiet", "--interval", IV, "--no-precheck", "--pause-after", "2000000"]); assert rc == 3, (rc, err)
    snap = tree(dest)
    for args in (["pack", "-o", wd + "/x1.7z", dest],                                # the folder being extracted into
                 ["pack", "-o", wd + "/x2.7z", dest + "/pack/big_text.dat"],         # a file inside it
                 ["pack", "-o", ROOT + "/work/x3.7z", wd],                           # a folder that contains the job
                 ["pack", "--each", "file", "-o", "-", dest]):                       # one archive per file inside it
        rc, err = run(args + ["--quiet"])
        assert "unfinished extraction" in err or "unfinished NibbleZip job" in err, (args, rc, err)
    other = stage(arcs["zip-7zip"], "claims-other")
    rc, err = run(["extract", other, "--quiet", "--dest", dest + "/inner"]); assert rc == 1 and "unfinished extraction" in err, err
    assert tree(dest) == snap, "the partly extracted folder was changed"
    # and the other way round: a stopped compression job claims its inputs
    src = ROOT + "/work/claims-comp"; shutil.rmtree(src, ignore_errors=True); shutil.copytree(ROOT + "/data/pack", src + "/stuff")
    shutil.copy2(arcs["zip-7zip"], src + "/stuff/inside.zip")
    rc, err = run(["pack", "--quiet", "--step", "262144", "--pause-after", "1500000", "-o", src + "/stuff.7z", src + "/stuff"]); assert rc == 3, (rc, err)
    rc, err = run(["extract", src + "/stuff/inside.zip", "--quiet"]); assert rc == 1 and "unfinished compression job" in err, err
    rc, err = run(["extract", other, "--quiet", "--dest", src + "/stuff/out"]); assert rc == 1 and "unfinished compression job" in err, err
    # both jobs can still be finished, with the right results
    rc, err = run(["extract", a, "--quiet", "--interval", IV]); assert rc == 0, err
    expect_tree(dest, ROOT + "/ref/nz-7z", "claims extraction")
    rc, err = run(["resume", src + "/stuff.7z", "--quiet", "--step", "262144"]); assert rc == 0, err
    assert not os.path.exists(src + "/stuff")
    rc, err = run(["verify", src + "/stuff.7z", "--quiet"]); assert rc == 0, err
    print("unfinished jobs keep other jobs away from their files OK", flush=True)
    shutil.rmtree(ROOT + "/work", ignore_errors=True)

    # archives it must refuse, without touching them
    data = ROOT + "/data"; A = ROOT + "/arcs"; bad = {}
    def mk(name, *sw, ext="7z"):
        out = "%s/%s.%s" % (A, name, ext); subprocess.run([SEVENZ, "a", *sw, out, "pack/readme.txt", "pack/big_text.dat"], cwd=data, capture_output=True, check=True); bad[name] = out
    mk("ppmd", "-m0=ppmd"); mk("bzip2", "-m0=bzip2"); mk("encrypted", "-psecret"); mk("enc-names", "-psecret", "-mhe=on")
    mk("zip-encrypted", "-tzip", "-psecret", ext="zip"); mk("zip-deflate64", "-tzip", "-mm=Deflate64", ext="zip")
    for src in ("7z-default", "nz-7z", "zip-7zip", "nz-zip"):
        b = bytearray(open(arcs[src], "rb").read()); b[len(b) // 2] ^= 0x55
        bad["damaged-" + src] = A + "/damaged-" + os.path.basename(arcs[src]); open(bad["damaged-" + src], "wb").write(b)
    open(A + "/notarchive.zip", "wb").write(b"this is not an archive at all" * 10); bad["not-an-archive"] = A + "/notarchive.zip"
    for name, arc in bad.items():
        a = stage(arc, "bad-" + name); before = digest(a)
        rc, err = run(["extract", a, "--quiet", "--interval", IV])
        assert rc == 1, (name, rc, err)
        assert digest(a) == before, name + ": archive was modified"
        assert leftovers(a) == [os.path.basename(a)], (name, leftovers(a), err)
        assert not os.listdir(ROOT + "/home/journals")
    print("refuses %d unsupported or damaged archives without touching them OK" % len(bad), flush=True)

    # hostile names can never escape the destination folder
    evil = A + "/evil.zip"
    with zipfile.ZipFile(evil, "w", zipfile.ZIP_DEFLATED) as z:
        for n in ("../escape.txt", "/abs.txt", "C:\\win.txt", "ok/../../up.txt", "..\\..\\back.txt", "dup.txt", "dup.txt", "DUP.txt", "CON.txt", "trail. ", "a/b:c?.txt", "good/fine.txt"):
            z.writestr(zipfile.ZipInfo(n), ("content of " + n).encode())
    a = stage(evil, "evil"); wd = os.path.dirname(a)
    rc, err = run(["extract", a, "--quiet"]); assert rc == 0, err
    outside = [f for f in os.listdir(wd) if f != "evil"] + [f for f in os.listdir(ROOT + "/work") if not os.path.isdir(ROOT + "/work/" + f)]
    assert outside == [], "files escaped: " + str(outside)
    got = sorted(k for k, v in tree(wd + "/evil").items() if v is not None)
    assert len(got) == 12, got
    assert not os.path.exists("/abs.txt") and not os.path.exists(ROOT + "/escape.txt")
    print("hostile file names stay inside the destination OK:", got, flush=True)
    print("EXTRACT TESTS PASSED (%d crash scenarios)" % total_crash)

main()
