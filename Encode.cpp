/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <map>
#include <unordered_set>

const int kMaxScanLineWidth = 3840;
static uint16_t gWidth, gHeight;

const char kReuseScanline = 51;
const char kNewScanline = 122;

int gNow;

struct CachedScanLine
{
  const char *bytes;
  uint64_t fileOffset;
  int lastUsed;

  CachedScanLine() : bytes(nullptr), fileOffset(0), lastUsed(0) {}

  CachedScanLine(const char* bytes, size_t fileOffset)
   : bytes(bytes)
   , fileOffset(fileOffset)
   , lastUsed(gNow)
  {}

  CachedScanLine(CachedScanLine&& other)
   : bytes(other.bytes)
   , fileOffset(other.fileOffset)
   , lastUsed(other.lastUsed)
  {
    other.bytes = nullptr;
  }

  CachedScanLine& operator=(CachedScanLine&& other) {
    bytes = other.bytes;
    fileOffset = other.fileOffset;
    lastUsed = other.lastUsed;
    other.bytes = nullptr;
    return *this;
  }

  CachedScanLine(const CachedScanLine&) = delete;
  CachedScanLine& operator=(const CachedScanLine&) = delete;
};

struct ScanLineHasher
{
  size_t operator()(const CachedScanLine& scanLine) {
    size_t hash = 0;
    for (size_t x = 0; x < gWidth; x++) {
      hash = (hash << 8) ^ scanLine.bytes[x] ^ ((hash >> 24) & 0xff);
    }
    return hash;
  }
};

struct ScanLineEq
{
  size_t operator()(const CachedScanLine& scanLine1, const CachedScanLine& scanLine2) {
    return memcmp(scanLine1.bytes, scanLine2.bytes, gWidth) == 0;
  }
};

const size_t kScanLineCacheSize = 1080 * 4;

std::unordered_set<CachedScanLine, ScanLineHasher, ScanLineEq> gScanLineCache;
std::map<int, CachedScanLine> gScanLineLRU;
size_t gNumCachedScanLines;

void
Fail(const char* err)
{
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

size_t
RunLengthEncode(const char* input, char* output)
{
  char* outp = output;

  for (size_t x = 0; x < gWidth; ) {
    size_t x2 = x + 1;
    while (x2 < gWidth && x2 - x < 255 && input[x2] == input[x]) {
      x2++;
    }

    *outp++ = x2 - x;
    *outp++ = input[x];
    x = x2;
  }

  return outp - output;
}

void
EvictScanLine()
{
  for (auto it = gScanLineLRU.begin(); it != gScanLineLRU.end(); it++) {
    if (gNumCachedScanLines <= kScanLineCacheSize) {
      return;
    }

    gNumCachedScanLines--;
    delete[] it->second.bytes;
    gScanLineLRU.erase(it->first);
    gScanLineCache.erase(it->second);
  }
}

void
WriteScanLine(int fd, uint64_t* outOffset, const char* scanLine)
{
  gNow++;

  auto result = gScanLineCache.emplace(scanLine, *outOffset);
  if (!result.second) {
    // We got a cache hit!
    CachedScanLine* cached = &const_cast<CachedScanLine&>(*result.first);

    gScanLineLRU.erase(cached->lastUsed);
    cached->lastUsed = gNow;

    CachedScanLine copy(cached->bytes, cached->fileOffset);
    gScanLineLRU.insert(std::make_pair(cached->lastUsed, std::move(copy)));

    char ctl = kReuseScanline;
    write(fd, &ctl, 1);
    write(fd, &cached->fileOffset, sizeof(uint64_t));
    *outOffset += 1 + sizeof(uint64_t);
    return;
  }

  gNumCachedScanLines++;
  EvictScanLine();

  char* bytes = new char[gWidth];
  memcpy(bytes, scanLine, gWidth);

  CachedScanLine copy(bytes, 0);
  gScanLineLRU.insert(std::make_pair(gNow, std::move(copy)));

  CachedScanLine* cached = &const_cast<CachedScanLine&>(*result.first);
  cached->bytes = bytes;

  char buffer[kMaxScanLineWidth];
  size_t size = RunLengthEncode(scanLine, buffer);

  char ctl = kNewScanline;
  write(fd, &ctl, 1);
  write(fd, buffer, size);
  *outOffset += 1 + size;
}

int
ReadSize(int fd)
{
  uint16_t data;
  read(fd, &data, sizeof(uint16_t));
  return data;
}

int
main(int argc, char** argv)
{
  int fd = open("video.raw", O_RDONLY, 0664);
  int outfd = open("video.pop", O_WRONLY|O_CREAT|O_TRUNC, 0664);
  int indexfd = open("video.idx", O_WRONLY|O_CREAT|O_TRUNC, 0664);

  struct stat stbuf;
  fstat(fd, &stbuf);

  size_t length = stbuf.st_size;
  printf("File size: %zu bytes\n", length);

  char* inputBuffer = (char*)mmap(nullptr, length,
                                  PROT_READ,
                                  MAP_FILE | MAP_PRIVATE,
                                  fd, 0);
  if (inputBuffer == MAP_FAILED) {
    Fail("mmap failed");
  }

  uint64_t outOffset = 0;

  char* ptr = inputBuffer;

  gWidth = *((uint16_t*)ptr);
  ptr += sizeof(uint16_t);
  gHeight = *((uint16_t*)ptr);
  ptr += sizeof(uint16_t);

  length -= sizeof(uint16_t) * 2;

  printf("%d x %d\n", gWidth, gHeight);

  write(indexfd, &gWidth, sizeof(uint16_t));
  write(indexfd, &gHeight, sizeof(uint16_t));

  size_t processed = 0;
  while (processed < length) {
    // Assume everyone uses little endian.
    write(indexfd, &outOffset, sizeof(uint64_t));

    for (size_t h = 0; h < gHeight; h++) {
      WriteScanLine(outfd, &outOffset, ptr);
      ptr += gWidth;
    }

    processed += size_t(gWidth) * size_t(gHeight);
  }

  close(fd);
  close(outfd);
  close(indexfd);
  return 0;
}
