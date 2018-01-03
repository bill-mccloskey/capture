/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "EncodeLib.h"

static void
Fail(const char* err)
{
  fprintf(stderr, "error: %s\n", err);
  exit(1);
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

  char* ptr = inputBuffer;

  int width = *((uint16_t*)ptr);
  ptr += sizeof(uint16_t);
  int height = *((uint16_t*)ptr);
  ptr += sizeof(uint16_t);

  length -= sizeof(uint16_t) * 2;

  printf("%d x %d\n", width, height);

  int numFrames = length / (size_t(width) * size_t(height));

  WriteCompressed("video.pop", "video.idx", width, height, inputBuffer, numFrames);

  close(fd);
  return 0;
}
