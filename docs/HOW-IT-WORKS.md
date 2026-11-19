# How NibbleZip works

This is the under-the-hood tour: what the program is trying to do, the rules it follows so
that it never loses data, and how the pieces fit together. The user guide is in the
[README](../README.md).

## Who wrote this

All of the code, the tests and this document were written by an AI (Claude, made by
Anthropic) at the request of the project's author, who supplied the idea and the
requirements and tests each version on real Windows. He is not a programmer and has not
reviewed the code line by line.

That is the reason this document spends so much space on verification and testing. The
design assumes the code may be wrong and makes it prove itself at run time: nothing is
released until it has been read back from disk and compared. The test suite then tries to
break that by killing the program everywhere it can. Independent review is welcome.

## The idea

Compressing normally needs room for two copies at once: the files and the archive. If the
drive is nearly full, you cannot compress the very things that are filling it.

NibbleZip compresses a little, proves that little is safely inside the archive, and only
then releases the originals it came from. Repeat until done. The extra space needed at any
moment is one step (64 MB to 1 GB, depending on settings), not the size of the whole job.

[HatchZip](https://hatchzip.dev) does the same thing in the other direction, extracting an
archive while shrinking it, and was the inspiration for this project. NibbleZip shares no
code with it.

## Goals

1. **Never destroy the only copy of anything.** At every instant, every byte of the user's
   data is either still in its original file or verifiably inside the archive, and the
   program can prove which.
2. **Survive anything.** A crash, a power cut, a full disk or the user pressing Stop must
   leave a state that can be continued, with no manual repair.
3. **Produce ordinary archives.** The output is plain 7z or ZIP that 7-Zip, WinRAR and
   others open. There is no private container format.
4. **Stay simple to use.** One window, sensible defaults, a confirmation that says plainly
   what will be deleted.

Not goals: being the fastest compressor, or supporting every archive format. RAR cannot be
created at all, because its compressor is proprietary.

## The core loop

All work is done in **steps**. A step is one bounded chunk of input, and every step goes
through the same sequence:

1. Read the source data into memory. Files are opened so that other programs cannot write
   to them while they are being read.
2. Compress it in memory.
3. Check that the destination drive has room for the result.
4. Append the compressed bytes to the archive and force them to disk.
5. Read those bytes back from the archive file and compare their checksum with what was
   written.
6. Decompress what was read back and compare it byte for byte with the source data that is
   still in memory.
7. Append a record of the step to the journal and force that to disk.
8. Rewrite the archive's index so the archive is valid as it stands, and force that to disk.
9. Only now release the originals.

Nothing is deleted until the data has made a full round trip through the disk and the
decompressor. If any check fails, the job stops and nothing from that step is released.

### Batches and big files

A step holds up to one step-size of input, and inputs come in two shapes.

**Small files** are gathered into a batch until the next file would overflow the step. The
batch is compressed together, which is why a folder of similar files compresses much better
as one 7z than as separate archives. After the step is committed, each file is deleted,
but only if its size and timestamp are still what they were when it was read.

**A file larger than one step** is processed one step at a time. After each step is
committed, the part of the file that is now in the archive is released from *inside* the
file. On NTFS this is done by marking the file sparse and deallocating the range
(`FSCTL_SET_SPARSE` and `FSCTL_SET_ZERO_DATA`), so free space rises while the file is
still being read. On file systems without sparse files, the space comes back only when the
file is finished.

A half-processed big file is the one state where data lives in two places at once: the
front of it only in the archive, the rest only in the original. The journal exists largely
to make that state safe, and the Undo feature exists to reverse it.

### Step sizes

| Level   | LZMA2 dictionary | Block  | Typical step              |
|---------|------------------|--------|---------------------------|
| Fast    | 4 MB             | 16 MB  | block x threads           |
| Normal  | 16 MB            | 64 MB  | block x threads           |
| Maximum | 32 MB            | 128 MB | block x threads           |
| Ultra   | 64 MB            | 192 MB | block x threads, max 1 GB |

The number of block threads is chosen from the CPU count and half of physical memory. If
free space is too low for a full step, the step is shrunk until it fits, and the job is
refused only if even the smallest step will not fit.

## Keeping a growing archive valid

Both formats keep their index at the end of the file. NibbleZip rewrites that index after
every completed file or batch, so the archive on disk is a complete, valid archive of
everything committed so far. The next step overwrites the old index with new data and
writes a new index after it.

### 7z

Each step becomes (part of) a 7z *folder* with a single LZMA2 coder. A batch is one folder
holding several files. A big file is one folder built up over many steps.

The trick for big files: LZMA2 is a sequence of self-describing chunks, and a run that
starts with a dictionary reset depends on nothing before it. Each step is encoded as an
independent run with its end marker removed, and the runs are simply appended. The end
marker is written once, after the last step. The result is an ordinary LZMA2 stream, and
it is the same layout 7-Zip itself produces when it compresses with several threads.

Because each step is independent, a step can be verified, journaled, resumed after, or
undone without any state from the steps before it.

The header (file names, sizes, CRCs, timestamps, attributes) is written after the data,
and the 32-byte signature header at the start is updated to point at it.

### ZIP

Each file is a normal ZIP entry using Deflate. A big file is one entry whose Deflate stream
is produced step by step, with a full flush between steps so each step starts on a byte
boundary with no history. The local header is written with ZIP64 placeholders and patched
with the real CRC and sizes when the file is finished. The central directory is rewritten
after every file or batch. ZIP64 records are used when sizes, offsets or entry counts need
them. Names are stored as UTF-8.

### Data that is already compressed

Before compressing a step, a handful of 32 KB samples are run through a fast Deflate. If
they shrink by less than 3%, the step is stored instead of compressed: as uncompressed
LZMA2 chunks in 7z, or as a "stored" entry in ZIP. Video, music and existing archives go
through at disk speed instead of being ground through LZMA for no gain. Stored steps are
still read back and compared before anything is released.

## The journal

The journal is an append-only file of records, each with a length and a CRC, kept in two
places: next to the archive (`name.7z.nzj`) and under `%LOCALAPPDATA%\NibbleZip\journals`.
On load, whichever copy has the longer valid run of records is used. A torn final record
is ignored, which is correct because nothing is released until its record is on disk.

Records:

- **Header**: format, level, options, the archive path and the list of inputs.
- **Batch**: where the archive data now ends, and for every file in the batch its path,
  size, timestamp, attributes and CRC.
- **Part**: one committed step of a big file: the new end of data, how far into the file
  it reached, and the running CRC.
- **Empty items**: empty files and folders, recorded just before the final index.
- **Un-part**: one step of a big file has been put back into its original (Undo).

Replaying the records rebuilds the complete archive index and the exact position of the
job. That is why losing the index at the end of the archive is harmless: the journal can
always write it again.

The journal is created before the archive, so there is never an archive without one.

## Why a crash cannot lose data

Order is everything. For each step:

| Crash happens...                        | State on disk                                   | On resume                                    |
|-----------------------------------------|-------------------------------------------------|----------------------------------------------|
| while writing or verifying step data    | originals intact, journal does not mention step | archive is cut back to the journal's end of data; step is redone |
| after the journal record, before index  | data in archive and journaled, originals intact | index rewritten from journal; originals then released |
| after the index, during deletion        | some originals gone, all are in the archive     | remaining originals of that step are released |
| mid-way through a big file              | front of file in archive, rest in original      | continues from the journaled offset           |
| during final clean-up                   | archive complete                                | clean-up is finished, journal removed         |

An original is only ever released after the journal says its data is in the archive, and
the journal only says so after that data has been read back and decompressed correctly.

## Split archives

Splitting is a layer beneath everything else. The engine writes to one logical archive by
offset, and that layer maps offsets onto `name.7z.001`, `.002` and so on. The parts are a
raw split of the archive, exactly as 7-Zip writes them, so joining them end to end gives
the ordinary archive. Resume, verify and Undo all work unchanged, and parts that are no
longer needed are deleted when the archive is cut back.

## Separate archives

"One archive per item" and "one per file" plan a list of ordinary jobs and run them in
turn, each with its own journal. Two rules keep this safe across interruptions:

- An original that already belongs to an unfinished job always continues in *that*
  archive. It is never started again under a different name, because it may already be
  partly consumed.
- An item whose archive already exists and has no journal is finished. It is skipped and
  its original is left alone.

So pressing Start again with the same selection simply carries on.

## Undo

If a job is stopped part-way through a big file, the user can put that file back instead
of resuming. Undo:

1. decompresses every committed step of that file and checks the result against the
   running CRC in the journal, changing nothing if it does not match;
2. then, last step first, decompresses a step, writes it back into the original, forces it
   to disk, reads it back and compares, journals it, and cuts the archive back by that
   step.

Working backwards means the archive shrinks as the file refills, so Undo needs no extra
room. It is journaled like everything else, so an interrupted Undo can be continued, or
the job can be resumed forwards instead.

## Extraction that shrinks the archive

Extraction runs the same idea in reverse: turn a stretch of the archive into files, prove
those files are safely on disk, and only then release that stretch of the archive.

### Front to back, releasing space inside the file

An archive's index sits at its end, and decompression only runs forwards. So the extractor
reads the archive from the front and releases what it has consumed from *inside* the
archive file, using the same sparse-file mechanism as compression. The index at the end
and the signature at the start stay intact, which is what lets a later run open the
half-consumed archive again. For a split archive, parts that have been fully consumed are
simply deleted.

### Safe restart points

Space can only be released up to a point from which decompression can restart with no
memory of what came before. Those points are:

- **7z, LZMA2:** any chunk that begins with a dictionary reset. The extractor walks the
  LZMA2 chunk headers itself, so it sees every such boundary. Multi-threaded 7-Zip and
  NibbleZip both write one at every block.
- **ZIP, Deflate:** any block boundary. The journal stores the bit offset and the last
  32 KB of output, and the decompressor is restarted with `inflatePrime` and
  `inflateSetDictionary`. This is the technique from zlib's `zran` example, used here to
  make a single huge ZIP entry resumable.
- **Stored data:** anywhere.
- **The end of any file or folder.**

A 7z folder with no reset inside it (single-threaded LZMA2, the older LZMA method, or one
passed through 7-Zip's x86 filter) is treated as one unit: it is extracted in full and its
space is released when it is done.

### The commit

At a restart point, once enough output has accumulated:

1. every file written since the last commit is forced to disk, read back, and its checksum
   compared with the checksum of what the decompressor produced;
2. the position (and, for Deflate, the 32 KB window) is appended to the extraction journal,
   `name.7z.nzx`, which is kept in two copies like the compression journal;
3. the consumed part of the archive is released.

After each commit the decompressor is deliberately thrown away and restarted from the
journaled state, exactly as a resumed run would. Every commit in every run therefore
exercises the resume path, not just the runs that crash.

### Check first

Before any of this, in space-saving mode, the whole archive is run through the same
decompression pipeline with nothing written: every file's size and CRC must match the
index. A damaged or unsupported archive is refused at this stage, untouched.

### Names

Names from an archive are never trusted. Drive letters, absolute paths, `..`, reserved
device names and characters Windows cannot store are rewritten, so no entry can be written
outside the destination folder. Duplicate names get a numbered suffix.

## Jobs never interfere with each other

A stopped job is not a finished one. A compression job may be half-way through a file whose
front is already released; an extraction may be half-way through writing a file whose
source is already released from the archive. If another job took in either of those, it
could archive or delete the only copy of something.

So every unfinished job keeps a claim on what it works on:

- a compression job: its inputs (files and folders), its archive and parts, and its journal;
- an extraction job: its archive and parts, and the folder it is extracting into.

Before any new job starts, every path it would read or write is compared with those claims.
Overlap in either direction counts: the same path, a path inside a claimed folder, or a
folder that contains a claimed path. Any overlap refuses the new job before it changes
anything, with a message naming the job that holds the claim. The claims are read from the
journals, so they hold across restarts.

## The checker

"Check an archive" opens a finished 7z or ZIP from disk (including split ones), streams
every file through the decompressor and compares sizes and CRCs with the index. It reads
7z through the LZMA SDK's header parser and its own streaming loop, so a 100 GB archive
needs only a few megabytes of memory. It handles the methods NibbleZip writes (LZMA2,
stored, Deflate) and says so plainly when an archive uses something else.

## How it is tested

The engine is portable C++ with a thin platform layer, so the same code runs on Linux for
testing and on Windows for real.

- **Reference tools.** Every archive made in the tests is tested and extracted by real
  7-Zip (and Info-ZIP `unzip` for ZIP), and the extracted tree is compared byte for byte
  with the original, including empty files, empty folders, Unicode names and timestamps.
- **Crash injection.** The engine has named checkpoints between every pair of steps in
  the sequences above. The test suite runs a job, kills the process at checkpoint 1,
  resumes, and verifies; then again at checkpoint 2; and so on until the job completes
  without being killed. Variations delete the primary journal, delete the mirror, or crash
  a second time during the resume.
- **Numbers at the time of writing:** 357 crash scenarios for plain jobs, 179 with split
  archives, 33 through Undo, and several hundred through the separate-archive modes.
- **Extraction** is tested against archives made by NibbleZip, by 7-Zip in eight layouts
  (default with the executable filter, small blocks, single-threaded solid, LZMA, stored,
  non-solid, ZIP) and by Python's zipfile (mixed methods, ZIP64). Each is extracted with a
  crash at every checkpoint and the result compared with what 7-Zip extracts. Archives that
  must be refused (encrypted, unsupported methods, damaged, not an archive) are checked to
  be left byte-for-byte untouched, and a ZIP full of hostile names must stay inside its
  destination folder.
- **Claims** are tested in both directions: with an extraction stopped half-way, compressing
  its folder, a file inside it, a folder containing it, or extracting something else into it
  must all be refused and leave it untouched; the same for a stopped compression job; then
  both jobs must still finish with correct results.
- **Large files.** A separate test compresses files over 4 GB, checks ZIP64, and measures
  peak disk use to confirm that space really is released as the job runs.
- **Damage.** Flipped bytes and missing parts must be caught by the checker.
- **The Windows build** is cross-compiled and runs the same suite under Wine, and the real
  window is driven through start, stop, resume, undo and the other flows by a small helper.

What that does not cover: Wine has no sparse-file support, so releasing space inside a
file on NTFS is verified on real Windows by hand, not by the automated suite.

## Limits

- Deleted originals do not go to the Recycle Bin.
- A 7z made here is solid within a step, not across the whole archive. On very similar
  data that costs a little compared with one giant solid block.
- Releasing space inside a big file needs NTFS.
- The archive has to be on the same drive as the files for the space saving to matter.
- No encryption, no RAR.
- Extraction cannot release space inside a 7z folder that has no dictionary resets.

## Source map

| File                 | What it holds                                                    |
|----------------------|------------------------------------------------------------------|
| `src/engine.cpp`     | scanning, step sizing, the core loop, 7z and ZIP writers, journal, resume, Undo, estimate |
| `src/arcfile.cpp`    | one archive as a single file or as split parts                   |
| `src/batch.cpp`      | planning and running separate archives                           |
| `src/extract.cpp`    | extraction, restart points, the extraction journal               |
| `src/common.h`       | pieces shared by compressor and extractor (journal, checkpoints) |
| `src/verify.cpp`     | the archive checker                                              |
| `src/platform.cpp`   | Win32 and POSIX file operations, including hole punching         |
| `src/gui.cpp`        | the Windows interface                                            |
| `src/cli.cpp`        | command-line front end, used by the tests                        |
| `tests/`             | round-trip, crash-injection, large-file and separate-archive tests |

Compression itself comes from the LZMA SDK (public domain) and zlib.
