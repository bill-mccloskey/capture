/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

const int kMaxScanLineWidth = 3840;
static uint16_t gWidth, gHeight;

const char kReuseScanline = 51;
const char kNewScanline = 122;

// One minute max.
const size_t kMaxFrames = 60 * 60;

uint64_t gIndex[kMaxFrames];
size_t gNumFrames;

void
Fail(const char* err)
{
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

const char*
RunLengthDecode(const char* input, char* output)
{
  const char* inp = input;
  char* outp = output;

  while (outp - output < gWidth) {
    char count = *inp++;
    char byte = *inp++;

    for (; count; count--) {
      *outp++ = byte;
    }
  }

  return inp;
}

size_t
ReadScanline(const char* input, size_t offset, char* output)
{
  if (input[offset] == kNewScanline) {
    offset++;
    const char* result = RunLengthDecode(input + offset, output);
    offset += result - (input + offset);
  } else if (input[offset] == kReuseScanline) {
    offset++;
    uint64_t innerOffset;
    memcpy(&innerOffset, input + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);

    ReadScanline(input, innerOffset, output);
  } else {
    assert(false);
  }

  return offset;
}

int
main(int argc, char** argv)
{
  int fd = open("video.pop", O_RDONLY, 0664);
  int outfd = open("video.raw2", O_WRONLY|O_CREAT|O_TRUNC, 0664);
  int indexfd = open("video.idx", O_RDONLY, 0664);

  read(indexfd, &gWidth, sizeof(uint16_t));
  read(indexfd, &gHeight, sizeof(uint16_t));

  printf("%d x %d\n", gWidth, gHeight);

  for (;;) {
    if (read(indexfd, &gIndex[gNumFrames], sizeof(uint64_t)) == 0) {
      break;
    }
    gNumFrames++;
  }

  printf("%d frames\n", int(gNumFrames));

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

  uint16_t data = gWidth;
  write(outfd, &data, sizeof(data));
  data = gHeight;
  write(outfd, &data, sizeof(data));

  size_t offset = 0;
  while (offset < length) {
    char scanLine[kMaxScanLineWidth];
    offset = ReadScanline(inputBuffer, offset, scanLine);
    write(outfd, scanLine, gWidth);
  }

  close(fd);
  close(outfd);

  return 0;
}
