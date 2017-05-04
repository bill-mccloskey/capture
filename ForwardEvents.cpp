#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
#include <mutex>

double
Now()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return double(tv.tv_usec) / 1000000.0 + double(tv.tv_sec);
}

struct Forwarder
{
  Forwarder()
    : mKeyTable()
    , mCmdBufRead(0)
    , mCmdBufWrite(0)
    , mRelX(0)
    , mRelY(0)
    , mRelWheel(0)
    , mPlayAudio(false)
    , mFinished(false)
  {}

  void Init(const char* keyName, const char* mouseName);
  void Finish();

  void KeyboardThread();
  void MouseThread();
  void OutputThread();
  void AudioThread();

private:
  void MakeKeyTable();

  void SetupOutput(int fd);
  void OutputCommand(char cmd);
  void OutputRel(int amt, char negCmd, char posCmd);

  void BufferCommand(char cmd);

  char mKeyTable[128];

  std::mutex mMutex;

  char mCmdBuf[32];
  int mCmdBufRead, mCmdBufWrite;

  int mRelX, mRelY, mRelWheel;

  int mKeyboardFile;
  int mMouseFile;
  int mOutputFile;

  int mPlayAudio;
  double mPlayStart;

  std::atomic<bool> mFinished;
};

void
Forwarder::MakeKeyTable()
{
  mKeyTable[1] = 110;

  for (int i = 2; i <= 13; i++) {
    mKeyTable[i] = i;
  }

  for (int i = 14; i <= 27; i++) {
    mKeyTable[i] = i + 1;
  }

  mKeyTable[28] = 43;
  mKeyTable[29] = 58;

  for (int i = 30; i <= 40; i++) {
    mKeyTable[i] = i + 1;
  }

  mKeyTable[41] = 1;
  mKeyTable[42] = 44;
  mKeyTable[43] = 29;

  for (int i = 42; i <= 53; i++) {
    mKeyTable[i] = i + 2;
  }

  mKeyTable[54] = 57;
  mKeyTable[55] = 0; // ???
  mKeyTable[56] = 60;
  mKeyTable[57] = 61;
  mKeyTable[58] = 30;

  mKeyTable[59] = 112;
  mKeyTable[60] = 113;
  mKeyTable[61] = 114;
  mKeyTable[62] = 115;

  mKeyTable[97] = 64;
  mKeyTable[100] = 62;
  mKeyTable[102] = 80;
  mKeyTable[103] = 83;
  mKeyTable[104] = 85;
  mKeyTable[105] = 79;
  mKeyTable[106] = 89;
  mKeyTable[107] = 81;
  mKeyTable[108] = 84;
  mKeyTable[109] = 86;
}

void
Forwarder::SetupOutput(int fd)
{
  struct termios tty;
  memset(&tty, 0, sizeof tty);

  if (tcgetattr(fd, &tty) != 0) {
    perror("tcgetattr");
    exit(1);
  }

  // Baud rate
  cfsetospeed(&tty, (speed_t)B9600);
  cfsetispeed(&tty, (speed_t)B9600);

  // 8N1
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;

  tty.c_cflag &= ~CRTSCTS; // no flow control
  //tty.c_cc[VMIN] = 1; // read doesn't block
  //tty.c_cc[VTIME] = 5; // 0.5 seconds read timeout
  tty.c_cflag |= CREAD | CLOCAL;     // turn on READ & ignore ctrl lines

  /* Make raw */
  cfmakeraw(&tty);

  /* Flush Port, then applies attributes */
  tcflush(fd, TCIFLUSH);
  if (tcsetattr(fd, TCSANOW, &tty) != 0) {
    perror("tcsetattr");
    exit(1);
  }
}

void
Forwarder::OutputCommand(char ch)
{
  if (write(mOutputFile, &ch, 1) != 1) {
    perror("write");
    exit(1);
  }

  char result;
  assert(read(mOutputFile, &result, 1) == 1);
  assert(result == ~ch);
}

void
Forwarder::BufferCommand(char cmd)
{
  std::lock_guard<std::mutex> guard(mMutex);
  mCmdBuf[mCmdBufWrite++] = cmd;
}

void
Forwarder::KeyboardThread()
{
  while (!mFinished) {
    struct input_event ev;

    if (read(mKeyboardFile, &ev, sizeof(struct input_event)) != sizeof(struct input_event)) {
      perror("read");
      exit(1);
    }

    if (ev.type == EV_KEY) {
      if (ev.code == KEY_INSERT) {
        printf("Finishing...\n");
        mFinished = true;
        return;
      }

      bool make = ev.value;
      char usb_code = mKeyTable[ev.code];
      if (!usb_code) {
        printf("unexpected unmapped code: %d\n", ev.code);
        continue;
      }

      if (!make) {
        usb_code += 128;
      }

      BufferCommand(usb_code);
    }
  }
}

void
Forwarder::MouseThread()
{
  while (!mFinished) {
    struct input_event ev;

    if (read(mMouseFile, &ev, sizeof(struct input_event)) != sizeof(struct input_event)) {
      perror("read");
      exit(1);
    }

    if (ev.type == EV_KEY) {
      if (ev.code == BTN_LEFT) {
        if (ev.value) {
          BufferCommand(0x49);
        } else {
          BufferCommand(0xc9);
        }
      } else if (ev.code == BTN_MIDDLE) {
        if (ev.value) {
          BufferCommand(0x4d);
        } else {
          BufferCommand(0xcd);
        }
      } else if (ev.code == BTN_RIGHT) {
        if (ev.value) {
          BufferCommand(0x4a);
        } else {
          BufferCommand(0xca);
        }
      }
    } else if (ev.type == EV_REL) { // movement
      if (ev.code == REL_X) {
        std::lock_guard<std::mutex> guard(mMutex);
        mRelX += ev.value;
      } else if (ev.code == REL_Y) {
        std::lock_guard<std::mutex> guard(mMutex);
        mRelY += ev.value;
      } else if (ev.code == REL_WHEEL) {
        std::lock_guard<std::mutex> guard(mMutex);
        mRelWheel += ev.value;
      }
    }
  }
}

void
Forwarder::OutputRel(int amt, char negCmd, char posCmd)
{
  if (abs(amt) >= 5) {
    OutputCommand(0x6f);
  }

  if (amt < 0) {
    OutputCommand(negCmd);
  } else if (amt > 0) {
    OutputCommand(posCmd);
  }

  if (abs(amt) >= 5) {
    OutputCommand(0x6d);
  }
}

void
Forwarder::OutputThread()
{
  while (!mFinished) {
    int command, relX, relY, relWheel;

    {
      std::lock_guard<std::mutex> guard(mMutex);
      if (mCmdBufRead == mCmdBufWrite) {
        mCmdBufRead = mCmdBufWrite = 0;
      }

      if (mCmdBufRead < mCmdBufWrite) {
        command = mCmdBuf[mCmdBufRead++];
      } else {
        command = 0;

        relX = mRelX;
        relY = mRelY;
        relWheel = mRelWheel;
        mRelX = mRelY = mRelWheel = 0;
      }
    }

    if (command) {
      printf("command %d\n", command);
      OutputCommand(command);

      std::lock_guard<std::mutex> guard(mMutex);
      mPlayAudio = true;
      mPlayStart = Now();
    } else if (relX || relY || relWheel) {
      printf("output: x=%d, y=%d, z=%d\n", relX, relY, relWheel);
      OutputRel(relX, 0x42, 0x43);
      OutputRel(relY, 0x44, 0x45);
      OutputRel(relWheel, 0x58, 0x57);
    }
  }
}

void
Forwarder::AudioThread()
{
  snd_pcm_t *handle;

  int err = snd_pcm_open(&handle, "default", SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    printf("Playback open error: %s\n", snd_strerror(err));
    exit(1);
  }

  err = snd_pcm_set_params(handle,
                           SND_PCM_FORMAT_U8,
                           SND_PCM_ACCESS_RW_INTERLEAVED,
                           1,
                           44000,
                           1,
                           4000);
  if (err < 0) {
    printf("Playback open error: %s\n", snd_strerror(err));
    exit(1);
  }

  const size_t bufferSize = 11;
  unsigned char onBuffer[bufferSize], offBuffer[bufferSize];

  for (int i = 0; i < bufferSize; i++) {
    onBuffer[i] = i % 2 == 0 ? 255 : 0;
    offBuffer[i] = 0;
  }

  while (!mFinished) {
    bool play;
    double start;
    {
      std::lock_guard<std::mutex> guard(mMutex);
      play = mPlayAudio;
      start = mPlayStart;
      mPlayAudio = false;
    }

    if (play) {
      double t = Now () - start;
      printf("audio delay: %g\n", t);
    }

    unsigned char* buffer = play ? onBuffer : offBuffer;

    snd_pcm_sframes_t frames = snd_pcm_writei(handle, buffer, sizeof(onBuffer));
    if (frames < 0) {
      frames = snd_pcm_recover(handle, frames, 0);
    }
    if (frames < 0) {
      printf("snd_pcm_writei failed: %s\n", snd_strerror(frames));
      exit(1);
    }
    if (frames > 0 && frames < (long)sizeof(onBuffer)) {
      printf("Short write (expected %li, wrote %li)\n", (long)sizeof(onBuffer), frames);
      exit(1);
    }
  }

  snd_pcm_close(handle);
}

void
Forwarder::Init(const char* keyName, const char* mouseName)
{
  MakeKeyTable();

  mKeyboardFile = open(keyName, O_RDONLY | O_NOCTTY);
  if (mKeyboardFile == -1) {
    perror("open");
    exit(1);
  }

  char name[128];
  ioctl(mKeyboardFile, EVIOCGNAME(sizeof(name)), name);
  printf("Reading from keyboard %s\n", name);

  mMouseFile = open(mouseName, O_RDONLY | O_NOCTTY);
  if (mMouseFile == -1) {
    perror("open");
    exit(1);
  }

  ioctl(mMouseFile, EVIOCGNAME(sizeof(name)), name);
  printf("Reading from mouse %s\n", name);

  mOutputFile = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY);
  if (mOutputFile == -1) {
    perror("open");
    exit(1);
  }

  SetupOutput(mOutputFile);
}

void
Forwarder::Finish()
{
  // Clear the buffer.
  OutputCommand(0x38);
}

int
main(int argc, char* argv[])
{
  Forwarder fwd;

  fwd.Init(argv[1], argv[2]);

  std::thread keyboard_in(&Forwarder::KeyboardThread, std::ref(fwd));
  std::thread mouse_in(&Forwarder::MouseThread, std::ref(fwd));
  std::thread audio_out(&Forwarder::AudioThread, std::ref(fwd));

  fwd.OutputThread();

  keyboard_in.join();
  mouse_in.join();
  audio_out.join();

  fwd.Finish();

  printf("Done capturing\n");

  for (;;) {
    // Try to read everything from stdin.
    char buffer[1024];
    read(STDIN_FILENO, buffer, sizeof(buffer));
  }

  return 0;
}
