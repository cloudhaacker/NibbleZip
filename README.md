# NibbleZip

Compresses files into a 7z or ZIP archive and frees up the originals' disk space *as it goes*,
so you never need room for both the files and the archive at once.

## Using it

1. Run `NibbleZip.exe` (no install). Windows may show a blue "Windows protected your PC" box
   because the program is not code-signed: choose **More info > Run anyway**.
2. Drag files or folders into the window, or use **Add files / Add folder**.
3. Pick 7z (smallest) or ZIP (opens everywhere) and a compression level, then press **Start**.
4. Optional: **Add to right-click menu** puts "Compress and reclaim space (NibbleZip)" on the
   right-click menu of files and folders (under "Show more options" on Windows 11).

Untick "Delete each original..." to use it as an ordinary archiver. That is the best way to try
it for the first time.

## How it stays safe

* **Check before starting.** It works out how much free space one step needs and refuses to start
  if that is not available.
* **Verify, then delete.** Each chunk is written to the archive, forced to disk, read back from
  disk, decompressed, and compared byte-for-byte with the source. Only then is the source released.
* **Journal.** Every committed step is recorded in a small `.nzj` file next to the archive, with a
  second copy under `%LOCALAPPDATA%\NibbleZip\journals`. After a crash, power cut or Stop, start
  NibbleZip again and it offers to resume exactly where it left off.
* **Always a valid archive between files.** The archive index is rewritten after every file or batch.
* Files that are in use, change while being read, or are links/junctions are skipped and left alone.

## How space is reclaimed

* Small files are compressed together in batches; the whole batch is deleted once verified.
* A file bigger than one step (64 MB - 1 GB depending on level and RAM) is compressed a step at a
  time, and the part already stored is released from inside the file (NTFS sparse "hole punching").
  A half-processed big file is therefore only complete again once the job is resumed and finished.

## Limits

* Deleted originals do not go to the Recycle Bin.
* Releasing space inside a big file needs NTFS. On exFAT/FAT32 drives a big file is only freed when
  it is finished, so it needs room for its compressed copy.
* The archive must be on the same drive as the files for the space saving to matter.
* RAR archives cannot be created: the RAR compressor is proprietary to WinRAR.
* 7z archives are written in solid blocks of one step rather than one giant solid block.

## Building

Needs Python with `pip install ziglang`, the 7-Zip sources (`github.com/ip7z/7zip`, only the
public-domain `C/` folder is used) and zlib (`github.com/madler/zlib`) checked out in `../deps`.

    ./build-windows.sh     # NibbleZip.exe and nibblezip-cli.exe
    ./build-linux.sh       # command-line build used by the tests
    python3 tests/run_tests.py   # round-trips against 7-Zip/unzip, crash injected at every checkpoint
    python3 tests/big_test.py    # >4 GB files, peak disk use

Compression code: LZMA SDK by Igor Pavlov (public domain) and zlib by Jean-loup Gailly and Mark Adler (zlib licence).
