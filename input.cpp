#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>

void
Write(char ch, int fd)
{
    if (write(fd, &ch, 1) != 1) {
        perror("write");
        exit(1);
    }

    char result;
    assert(read(fd, &result, 1) == 1);
    assert(result == ~ch);
}

void
Setup(int fd)
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

int main()
{
    int usb_fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY);
    if (usb_fd == -1) {
        perror("open");
        exit(1);
    }

    Setup(usb_fd);

#if 0
    Write(36, usb_fd);
    Write(164, usb_fd);

    Write(24, usb_fd);
    Write(152, usb_fd);
#endif

    //Write(0x6f, usb_fd);
    //Write(0x6f, usb_fd);
    //Write(0x6f, usb_fd);

    for (int i = 0; i < 100; i++) {
        Write(0x58, usb_fd);
    }

    Write(0x6d, usb_fd);

    close(usb_fd);

    return 0;
}
