/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include "DeckLinkAPI.h"
#include "DeckLinkAPIDispatch.cpp"

#include <atomic>
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

void
PrintDisplayModes(IDeckLink* deckLink)
{
  IDeckLinkDisplayMode*	displayMode;
  IDeckLinkInput* deckLinkInput;
  IDeckLinkDisplayModeIterator*	displayModeIterator;

  if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&deckLinkInput) != S_OK) {
    Fail("IDeckLinkInput QI failed");
  }

  if (deckLinkInput->GetDisplayModeIterator(&displayModeIterator) != S_OK) {
    Fail("GetDisplayModeIterator failed");
  }

  while (displayModeIterator->Next(&displayMode) == S_OK) {
    long width, height;
    BMDTimeValue frameRate;
    BMDTimeScale timeScale;
    width = displayMode->GetWidth();
    height = displayMode->GetHeight();
    displayMode->GetFrameRate(&frameRate, &timeScale);

    BMDDisplayModeSupport support;
    if (deckLinkInput->DoesSupportVideoMode(displayMode->GetDisplayMode(),
                                            bmdFormat8BitYUV,
                                            bmdVideoInputFlagDefault,
                                            &support, nullptr) != S_OK) {
      Fail("DoesSupportDisplayMode failed");
    }

    float fps = float(timeScale) / float(frameRate);
    if (support == bmdDisplayModeSupported && fps >= 58) {
      printf("Found display mode:\n");
      printf("  w=%ld h=%ld\n", width, height);
      printf("  rate=%f\n", fps);
    }

    RELEASE(displayMode);
  }

  RELEASE(deckLinkInput);
  RELEASE(displayModeIterator);
}

const int kWidth = 3840;
const int kHeight = 2160;

// We drop every second x and y pixels.
const size_t kFrameSize = kWidth * kHeight / 2 / 2;

// One minute max.
const size_t kMaxFrames = 60 * 60;

const size_t kFrameBufferSize = kMaxFrames * kFrameSize;

class CaptureCallback : public IDeckLinkInputCallback
{
public:
  CaptureCallback(char* frameBuffer)
   : mRefCount(1)
   , mFrameBuffer(frameBuffer)
   , mLastFrame(0)
   , mMaxInterArrivalTime(0)
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

  double MaxInterArrivalTime() const { return mMaxInterArrivalTime; }

private:
  std::atomic<int32_t> mRefCount;
  char* mFrameBuffer;
  double mLastFrame;
  double mMaxInterArrivalTime;
};

static std::atomic<uint32_t> gFrameCounter(0);

// Keeps in YUV, drops 3 of every 4 pixels in both dimensions.
void
ReduceFrameBy16(char* frameBytes, char* result)
{
  const size_t rowBytes = kWidth * 2;

  char* input = frameBytes;
  char* output = result;

  for (size_t h = 0; h < kHeight; h += 4) {
    for (size_t i = 0; i < rowBytes; i += 16) {
      char cb0 = *input++;
      char y0 = *input++;
      char cr0 = *input++;
      input++; // y1;

      input += 4;

      input++; // cb0
      char y01 = *input++;
      input++; // cr
      input++; // y1

      input += 4;

      *output++ = cb0;
      *output++ = y0;
      *output++ = cr0;
      *output++ = y01;
    }

    // Skip the next three rows.
    input += rowBytes;
    input += rowBytes;
    input += rowBytes;
  }
}

void
ReduceFrameBy8(char* frameBytes, char* result)
{
  const size_t rowBytes = kWidth * 2;

  char* input = frameBytes;
  char* output = result;

  for (size_t h = 0; h < kHeight; h += 2) {
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

HRESULT
CaptureCallback::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                        IDeckLinkAudioInputPacket* audioFrame)
{
  if (!videoFrame) {
    printf("No video in frame!\n");
    return S_OK;
  }

  //printf("Got frame\n");

  BMDTimeValue time, duration;
  if (videoFrame->GetStreamTime(&time, &duration, 60000) != S_OK) {
    Fail("GetStreamTime failed");
  }

  printf("Frame %lld\n", time / duration);

  if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
    printf("  [frame has no input source!]\n");
  } else /* if (std::atomic_load(&gFrameCounter) % 2 == 0) */ {
    void* frameBytes;
    videoFrame->GetBytes(&frameBytes);

    ReduceFrameBy8((char*)frameBytes, mFrameBuffer);
    mFrameBuffer += kFrameSize;
  }

  gFrameCounter++;
  return S_OK;
}

HRESULT
CaptureCallback::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events,
                                         IDeckLinkDisplayMode *mode,
                                         BMDDetectedVideoInputFormatFlags formatFlags)
{
  printf("VideoInputFormatChanged\n");
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

  printf("IF: %p\n", deckLink);

  PrintDisplayModes(deckLink);

  IDeckLinkInput* input;
  if (deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&input) != S_OK) {
    Fail("IDeckLinkInput QI failed");
  }

  if (input->EnableVideoInput(bmdMode4K2160p5994,
                              bmdFormat8BitYUV,
                              bmdVideoInputEnableFormatDetection) != S_OK) {
    Fail("EnableVideoInput failed");
  }

  CaptureCallback* callback = new CaptureCallback(frameBuffer);
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

  input->SetCallback(nullptr);
  RELEASE(callback);

  RELEASE(input);
  RELEASE(deckLink);

  printf("Writing to disk...\n");

  int fd = open("video.raw", O_WRONLY|O_CREAT|O_TRUNC, 0664);

  uint16_t data = kWidth / 2;
  write(fd, &data, sizeof(data));
  data = kHeight / 2;
  write(fd, &data, sizeof(data));

  for (int i = 0; i < kNumFrames; i++) {
    write(fd, frameBuffer + (i * kFrameSize), kFrameSize);
  }
  close(fd);

  printf("Done.\n");

  return 0;
}
