#!/bin/sh
# Builds the command-line tool for Linux (used for automated testing).
set -e
cd "$(dirname "$0")"
# The compression libraries are looked for in ./deps first (see README), then ../deps.
if [ -d deps/7zip/C ]; then D=deps; else D=../deps; fi
SDK=${SDK:-$D/7zip/C}; ZL=${ZL:-$D/zlib}
mkdir -p build/linux
OBJ=""
for f in 7zCrc 7zCrcOpt Alloc CpuArch LzFind LzFindMt LzFindOpt Lzma2Enc LzmaEnc Lzma2Dec LzmaDec MtCoder MtDec Threads 7zStream 7zArcIn 7zDec 7zBuf Bcj2 Bra Bra86 BraIA64 Delta; do
  [ build/linux/$f.o -nt $SDK/$f.c ] || gcc -O2 -c -D_REENTRANT -w $SDK/$f.c -o build/linux/$f.o
  OBJ="$OBJ build/linux/$f.o"
done
for f in adler32 crc32 deflate inflate inffast inftrees trees zutil; do
  [ build/linux/z_$f.o -nt $ZL/$f.c ] || gcc -O2 -c -w $ZL/$f.c -o build/linux/z_$f.o
  OBJ="$OBJ build/linux/z_$f.o"
done
g++ -std=c++17 -O2 -Wall -Wextra -I$SDK -I$ZL src/engine.cpp src/extract.cpp src/arcfile.cpp src/batch.cpp src/verify.cpp src/platform.cpp src/cli.cpp $OBJ -lpthread -o build/linux/nibblezip-cli
echo built build/linux/nibblezip-cli
