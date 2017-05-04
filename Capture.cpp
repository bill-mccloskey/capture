/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include "DeckLinkAPI.h"
#include "DeckLinkAPIDispatch.cpp"

#include <atomic>
#include <assert.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define RELEASE(p) do { (p)->Release(); (p) = nullptr; } while (0)

void
Fail(const char* err)
{
  fprintf(stderr, "error: %s\n", err);
  exit(1);
}

double
Now()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return double(tv.tv_usec) / 1000000.0 + double(tv.tv_sec);
}

// We drop every second x and y pixels.
const size_t kMaxFrameSize = 3840 * 2160 / 2 / 2;

// One minute max.
const size_t kMaxFrames = 60 * 60;

const size_t kFrameBufferSize = kMaxFrames * kMaxFrameSize;

class CaptureCallback : public IDeckLinkInputCallback
{
public:
  CaptureCallback(IDeckLinkInput* input, char* frameBuffer)
   : mRefCount(1)
   , mInput(input)
   , mFrameBuffer(frameBuffer)
   , mWidth(0)
   , mHeight(0)
  {}

  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {
    return E_NOINTERFACE;
  }

  virtual ULONG STDMETHODCALLTYPE AddRef() {
    int32_t old = std::atomic_fetch_add(&mRefCount, 1);
    return old;
  }

  virtual ULONG STDMETHODCALLTYPE  Release() {
    int32_t old = std::atomic_fetch_sub(&mRefCount, 1);
    if (old == 1) {
      delete this;
    }
    return old - 1;
  }

  virtual HRESULT STDMETHODCALLTYPE
  VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                          IDeckLinkDisplayMode*,
                          BMDDetectedVideoInputFormatFlags);

  virtual HRESULT STDMETHODCALLTYPE
  VideoInputFrameArrived(IDeckLinkVideoInputFrame*,
                         IDeckLinkAudioInputPacket*);

  void GetSize(size_t* width, size_t* height) {
    *width = mWidth;
    *height = mHeight;
  }

private:
  std::atomic<int32_t> mRefCount;
  IDeckLinkInput* mInput;
  char* mFrameBuffer;
  size_t mWidth, mHeight;
};

static std::atomic<uint32_t> gFrameCounter(0);

// Data is expected to be in UYVY format, with 32 bits for every two pixels.
void
ReduceFrameBy8(size_t width, size_t height, char* frameBytes, char* result)
{
  const size_t rowBytes = width * 2;

  char* input = frameBytes;
  char* output = result;

  for (size_t h = 0; h < height; h += 2) {
    for (size_t i = 0; i < rowBytes; i += 4) {
      input++; // cb0
      char y0 = *input++;
      input++; // cr0
      input++; // y1

      *output++ = y0;
    }

    // Skip the next row.
    input += rowBytes;
  }
}

size_t
ProcessFrame(size_t width, size_t height, char* frameBytes, char* result)
{
  const size_t rowBytes = width * 4;

  char* input = frameBytes;
  char* output = result;

  for (size_t h = 0; h < height; h++) {
    for (size_t i = 0; i < rowBytes; i += 4) {
      input++; // alpha
      char red = *input++;
      char green = *input++;
      char blue = *input++;

      // Actual multipliers should be:
      // red: 0.299
      // green: 0.587
      // blue: 0.114
      int32_t luminosity = (red >> 2) + (green >> 1) + (blue >> 3);
      *output++ = luminosity;
    }

    // Skip the next row.
    //input += rowBytes;
  }

  return width * height;
}

HRESULT
CaptureCallback::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                        IDeckLinkAudioInputPacket* audioFrame)
{
  printf("frame!\n");
  if (!mWidth) return S_OK;
  assert(mWidth != 0);
  assert(mHeight != 0);

  if (!videoFrame) {
    printf("No video in frame!\n");
    return S_OK;
  }

  BMDTimeValue time, duration;
  if (videoFrame->GetStreamTime(&time, &duration, 60000) != S_OK) {
    Fail("GetStreamTime failed");
  }

  void* frameBytes;
  videoFrame->GetBytes(&frameBytes);
  printf("Frame %lld %p\n", time / duration, frameBytes);

  if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
    printf("  [frame has no input source!]\n");
  } else /* if (std::atomic_load(&gFrameCounter) % 2 == 0) */ {
    void* frameBytes;
    videoFrame->GetBytes(&frameBytes);

    mFrameBuffer += ProcessFrame(mWidth, mHeight, (char*)frameBytes, mFrameBuffer);
    //ReduceFrameBy8(mWidth, mHeight, (char*)frameBytes, mFrameBuffer);
    //mFrameBuffer += mWidth * mHeight / 2 / 2;
  }

  gFrameCounter++;
  return S_OK;
}

HRESULT
CaptureCallback::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events,
                                         IDeckLinkDisplayMode *mode,
                                         BMDDetectedVideoInputFormatFlags formatFlags)
{
  BMDTimeValue t, scale;
  mode->GetFrameRate(&t, &scale);
  printf("format changed\n");
  printf("%ld x %ld %f fpd\n", mode->GetWidth(), mode->GetHeight(),
         float(scale) / float(t));

  assert(mWidth == 0);
  assert(mHeight == 0);

  mWidth = mode->GetWidth();
  mHeight = mode->GetHeight();

  mInput->PauseStreams();

  BMDDisplayModeSupport support;
  mInput->DoesSupportVideoMode(mode->GetDisplayMode(),
                               bmdFormat8BitARGB,
                               bmdVideoInputFlagDefault,
                               &support, nullptr);
  printf("support: %d\n", support == bmdDisplayModeSupported);

  if (mInput->EnableVideoInput(mode->GetDisplayMode(),
                               bmdFormat8BitARGB,
                               bmdVideoInputEnableFormatDetection) != S_OK) {
    Fail("EnableVideoInput failed from VideoInputFormatChanged");
  }

  mInput->FlushStreams();
  mInput->StartStreams();
  return S_OK;
}

int
main()
{
  printf("Hello world!\n");

  char* frameBuffer = (char*)mmap(nullptr, kFrameBufferSize,
                                  PROT_READ | PROT_WRITE,
                                  MAP_ANON | MAP_PRIVATE,
                                  0, 0);
  if (frameBuffer == MAP_FAILED) {
    Fail("mmap failed");
  }

  IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
  IDeckLink* deckLink;
  if (deckLinkIterator->Next(&deckLink) != S_OK) {
    Fail("device iteration failed");
  }
  RELEASE(deckLinkIterator);

  IDeckLinkInput* input;
  if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
    Fail("IDeckLinkInput QI failed");
  }

  if (input->EnableVideoInput(bmdModeHD1080p5994,
                              bmdFormat8BitARGB,
                              bmdVideoInputEnableFormatDetection) != S_OK) {
    Fail("EnableVideoInput failed");
  }

  CaptureCallback* callback = new CaptureCallback(input, frameBuffer);
  input->SetCallback(callback);

  input->StartStreams();

  const int kNumSeconds = 2;
  const size_t kNumFrames = kNumSeconds * 60;

  while (gFrameCounter < kNumFrames) {
    sched_yield();
  }

  input->StopStreams();

  double stop = Now();

  if (input->DisableVideoInput() != S_OK) {
    Fail("DisableVideoInput failed");
  }

  printf("Finished recording.\n");

  size_t width, height;
  callback->GetSize(&width, &height);

  input->SetCallback(nullptr);
  RELEASE(callback);

  RELEASE(input);
  RELEASE(deckLink);

  printf("Writing to disk...\n");

  int fd = open("video.raw", O_WRONLY|O_CREAT|O_TRUNC, 0664);

  uint16_t data = width;
  write(fd, &data, sizeof(data));
  data = height;
  write(fd, &data, sizeof(data));

  size_t frameSize = width * height;
  for (int i = 0; i < kNumFrames; i++) {
    write(fd, frameBuffer + (i * frameSize), frameSize);
  }
  close(fd);

  printf("Done.\n");

  return 0;
}
