/* -*- Mode: C++; indent-tabs-mode: nil; c-basic-offset: 2; tab-width: 8 -*- */

#include "DeckLinkAPI.h"
#include "DeckLinkAPIDispatch.cpp"

#include <atomic>
#include <assert.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "EncodeLib.h"

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
static std::atomic<uint32_t> gSkipFrameCounter(0);
static bool gHasFirstFrame;

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
ProcessFrame(size_t width, size_t height, char* frameBytes, char* result, int decoration)
{
  const size_t rowBytes = width * 4;

  char* input = frameBytes;
  char* output = result;

  for (size_t h = 0; h < height; h++) {
    bool invert = decoration;
#if 0
    if (decoration == 1 && h < height/2) {
      invert = true;
    }
    if (decoration == 2 && h >= height/2) {
      invert = true;
    }
#endif
    for (size_t i = 0; i < rowBytes; i += 4) {
      char first = *input++; // alpha
      char second = *input++;
      char third = *input++;
      char fourth = *input++;

      // Not sure why, but this channel seems to contain the luminosity.
      int32_t luminosity = third;

      *output++ = invert ? 256 - luminosity : luminosity;
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
  printf("Frame");
  if (!mWidth) return S_OK;
  assert(mWidth != 0);
  assert(mHeight != 0);

  if (!videoFrame) {
    printf("No video in frame!\n");
    return S_OK;
  }

  if (!audioFrame) {
    printf("No audio in frame\n");
    return S_OK;
  }

  gSkipFrameCounter++;
  if (gSkipFrameCounter <= 60) {
    return S_OK;
  }

  BMDTimeValue time, duration;
  if (videoFrame->GetStreamTime(&time, &duration, 60000) != S_OK) {
    Fail("GetStreamTime failed");
  }

  long audioFrameCount = audioFrame->GetSampleFrameCount();
  //printf("audio sample frame count = %ld\n", audioFrameCount);

  BMDTimeValue audioTime;
  if (audioFrame->GetPacketTime(&audioTime, 60000) != S_OK) {
    Fail("GetPacketTime failed");
  }

  void* frameBytes;
  videoFrame->GetBytes(&frameBytes);
  printf(" %lld", time / duration);

  printf(" (audio %lld)", audioTime / duration);

  void* audioBuffer;
  audioFrame->GetBytes(&audioBuffer);
  int16_t* audioBytes = static_cast<int16_t*>(audioBuffer);

#if 0
  int type = 0;
  for (size_t i = 0; i < audioFrameCount * 2; i += 2) {
    if (audioBytes[i] > 100) {
      int dips = 0;
      for (size_t j = 0; j < 20 && i + j < audioFrameCount * 2; j += 2) {
        printf(" %d", audioBytes[i + j]);
        if (audioBytes[i + j] < 0) {
          dips++;
        }
      }

      if (dips >= 3) {
        type = 1;
      } else {
        type = 2;
      }
      i = i + 30;
    }
  }
#endif

  int type = 0;
  for (size_t i = 0; i < audioFrameCount * 2; i += 2) {
    if (audioBytes[i] > 100) {
      type = 1;
    }
  }

  if (type) {
    gHasFirstFrame = true;

    printf(" type = %d", type);
  }

#if 0
  double sum = 0;
  for (size_t i = 0; i < audioFrameCount * 2; i += 2) {
    sum += audioBytes[i];
  }
  double avg = sum / audioFrameCount;

  double sumSq = 0;
  for (size_t i = 0; i < audioFrameCount * 2; i += 2) {
    double dev = audioBytes[i] - avg;
    sumSq += dev * dev;
  }

  double stddev = sqrt(sumSq / audioFrameCount);

  if (stddev > 100) {
    printf(" [mean %f, stddev = %f]", avg, stddev);
  }
#endif

  printf("\n");

  if (!gHasFirstFrame) {
    return S_OK;
  }

  //printf("first sample: %d %d %d %d %d %d\n", audioBytes[0], audioBytes[1],
  //       audioBytes[2], audioBytes[3],
  //       audioBytes[4], audioBytes[5]);

  if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
    printf("  [frame has no input source!]\n");
  } else /* if (std::atomic_load(&gFrameCounter) % 2 == 0) */ {
    void* frameBytes;
    videoFrame->GetBytes(&frameBytes);

    mFrameBuffer += ProcessFrame(mWidth, mHeight, (char*)frameBytes, mFrameBuffer, type);
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

  if (mInput->EnableAudioInput(bmdAudioSampleRate48kHz,
                               bmdAudioSampleType16bitInteger,
                               2) != S_OK) {
    Fail("EnableAudioInput failed");
  }

  mInput->FlushStreams();
  mInput->StartStreams();
  return S_OK;
}

void
WriteRaw(const char* fname, int width, int height, char* frameBuffer, int numFrames)
{
  int fd = open(fname, O_WRONLY|O_CREAT|O_TRUNC, 0664);

#if 0
  uint16_t data = width;
  write(fd, &data, sizeof(data));
  data = height;
  write(fd, &data, sizeof(data));
#endif

  size_t frameSize = width * height;
  for (int i = 0; i < numFrames; i++) {
    write(fd, frameBuffer + (i * frameSize), frameSize);
  }
  close(fd);
}

int
main(int argc, char* argv[])
{
  printf("Hello world!\n");

  int numSecs = 10;
  if (argc > 1) {
    numSecs = atoi(argv[1]);
  }

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

  if (input->EnableAudioInput(bmdAudioSampleRate48kHz,
                              bmdAudioSampleType16bitInteger,
                              2) != S_OK) {
    Fail("EnableAudioInput failed");
  }

  CaptureCallback* callback = new CaptureCallback(input, frameBuffer);
  input->SetCallback(callback);

  input->StartStreams();

  int numFrames = numSecs * 60;

  while (gFrameCounter < numFrames) {
    sched_yield();
  }

  input->StopStreams();

  double stop = Now();

  if (input->DisableVideoInput() != S_OK) {
    Fail("DisableVideoInput failed");
  }

  if (input->DisableAudioInput() != S_OK) {
    Fail("DisableAudioInput failed");
  }

  printf("Finished recording.\n");

  size_t width, height;
  callback->GetSize(&width, &height);

  input->SetCallback(nullptr);
  RELEASE(callback);

  RELEASE(input);
  RELEASE(deckLink);

  printf("Writing to disk...\n");

  //WriteRaw("video.raw", width, height, frameBuffer, numFrames);
  WriteCompressed("video.pop", "video.idx", width, height, frameBuffer, numFrames);

  printf("Done.\n");

  return 0;
}
