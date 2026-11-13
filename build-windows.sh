#!/bin/sh
# Cross-compiles NibbleZip.exe (GUI) and nibblezip-cli.exe for 64-bit Windows using zig (pip install ziglang).
set -e
cd "$(dirname "$0")"
SDK=${SDK:-../deps/7zip/C}; ZL=${ZL:-../deps/zlib}
ZIG="python3 -m ziglang"; T="-target x86_64-windows-gnu"
mkdir -p build/win
OBJ=""
for f in 7zCrc 7zCrcOpt Alloc CpuArch LzFind LzFindMt LzFindOpt Lzma2Enc LzmaEnc Lzma2Dec LzmaDec MtCoder MtDec Threads 7zStream; do
  [ build/win/$f.o -nt $SDK/$f.c ] || $ZIG cc $T -O2 -DNDEBUG -w -c $SDK/$f.c -o build/win/$f.o
  OBJ="$OBJ build/win/$f.o"
done
for f in adler32 crc32 deflate inflate inffast inftrees trees zutil; do
  [ build/win/z_$f.o -nt $ZL/$f.c ] || $ZIG cc $T -O2 -DNDEBUG -w -c $ZL/$f.c -o build/win/z_$f.o
  OBJ="$OBJ build/win/z_$f.o"
done
CXX="$ZIG c++ $T -std=c++17 -O2 -DNDEBUG -Wall -Wextra -municode -I$SDK -I$ZL"
$CXX src/engine.cpp src/platform.cpp src/gui.cpp res/nibblezip.rc $OBJ -Wl,--subsystem,windows -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -o build/win/NibbleZip.exe
$CXX src/engine.cpp src/platform.cpp src/cli.cpp res/cli.rc $OBJ -ladvapi32 -o build/win/nibblezip-cli.exe
ls -la build/win/*.exe
