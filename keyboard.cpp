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

char table[128];

void
MakeTable()
{
    table[1] = 110;

    for (int i = 2; i <= 13; i++) {
        table[i] = i;
    }

    for (int i = 14; i <= 27; i++) {
        table[i] = i + 1;
    }

    table[28] = 43;
    table[29] = 58;

    for (int i = 30; i <= 40; i++) {
        table[i] = i + 1;
    }

    table[41] = 1;
    table[42] = 44;
    table[43] = 29;

    for (int i = 42; i <= 53; i++) {
        table[i] = i + 2;
    }

    table[54] = 57;
    table[55] = 0; // ???
    table[56] = 60;
    table[57] = 61;
    table[58] = 30;

    table[59] = 112;
    table[60] = 113;
    table[61] = 114;
    table[62] = 115;

    table[97] = 64;
    table[100] = 62;
    table[102] = 80;
    table[103] = 83;
    table[104] = 85;
    table[105] = 79;
    table[106] = 89;
    table[107] = 81;
    table[108] = 84;
    table[109] = 86;
}

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

int
main(int argc, char* argv[])
{
    MakeTable();

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
            if (ev.code == KEY_INSERT) {
                break;
            }

            bool make = ev.value;
            char usb_code = table[ev.code];
            if (!usb_code) {
                printf("unexpected unmapped code: %d\n", ev.code);
                continue;
            }

            if (!make) {
                usb_code += 128;
            }

            Write(usb_code, usb_fd);
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
