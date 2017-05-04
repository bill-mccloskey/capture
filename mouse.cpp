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
Write3(char ch, int fd)
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

int
main(int argc, char* argv[])
{
    int in_fd = open(argv[1], O_RDONLY | O_NOCTTY);
    if (in_fd == -1) {
        perror("open");
        exit(1);
    }

    char name[128];
    ioctl(in_fd, EVIOCGNAME(sizeof(name)), name);
    printf("Reading from %s\n", name);

    int usb_fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY);
    if (usb_fd == -1) {
        perror("open");
        exit(1);
    }

    Setup(usb_fd);

    for (;;) {
        struct input_event ev;

        if (read(in_fd, &ev, sizeof(struct input_event)) != sizeof(struct input_event)) {
            perror("read");
            exit(1);
        }

        if (ev.type == EV_KEY) {
            if (ev.code == BTN_LEFT) {
                if (ev.value) {
                    Write(0x49, usb_fd);
                } else {
                    Write(0xc9, usb_fd);
                }
            } else if (ev.code == BTN_MIDDLE) {
                if (ev.value) {
                    Write(0x4d, usb_fd);
                } else {
                    Write(0xcd, usb_fd);
                }
            } else if (ev.code == BTN_RIGHT) {
                if (ev.value) {
                    Write(0x4a, usb_fd);
                } else {
                    Write(0xca, usb_fd);
                }
            }
        } else if (ev.type == EV_REL) { // movement
            if (abs(ev.value) >= 5) {
                Write(0x6f, usb_fd);
            }

            if (ev.code == REL_X) {
                if (ev.value < 0) {
                    Write3(0x42, usb_fd);
                } else {
                    Write3(0x43, usb_fd);
                }
            } else if (ev.code == REL_Y) {
                if (ev.value < 0) {
                    Write3(0x44, usb_fd);
                } else {
                    Write3(0x45, usb_fd);
                }
            } else if (ev.code == REL_WHEEL) {
                if (ev.value < 0) {
                    Write3(0x58, usb_fd);
                } else {
                    Write3(0x57, usb_fd);
                }
            }

            if (abs(ev.value) >= 5) {
                Write(0x6d, usb_fd);
            }
        }
    }

    // Clear the buffer.
    Write(0x38, usb_fd);

    close(usb_fd);
    close(in_fd);

    printf("Done capturing\n");

    for (;;) {
        // Try to read everything from stdin.
        char buffer[1024];
        read(STDIN_FILENO, buffer, sizeof(buffer));
    }

    return 0;
}
