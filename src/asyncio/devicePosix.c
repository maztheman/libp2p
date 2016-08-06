#include "asyncio/device.h"
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>


iodevTy serialPortOpen(const char *name)
{
  int port = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
  return port != -1 ?
    port : 0;
}

void serialPortClose(iodevTy port)
{
  close(port);
}

int serialPortSetConfig(iodevTy port,
                        int speed,
                        int dataBits,
                        int stopBits,
                        int parity)
{
  struct termios tios;
  memset(&tios, 0, sizeof(tios));

  speed_t localSpeed;
  switch (speed) {
    case 110:
      localSpeed = B110;
      break;
    case 300:
      localSpeed = B300;
      break;
    case 600:
      localSpeed = B600;
      break;
    case 1200:
      localSpeed = B1200;
      break;
    case 2400:
      localSpeed = B2400;
      break;
    case 4800:
      localSpeed = B4800;
      break;
    case 9600:
      localSpeed = B9600;
      break;
    case 19200:
      localSpeed = B19200;
      break;
    case 38400:
      localSpeed = B38400;
      break;
    case 57600:
      localSpeed = B57600;
      break;
    case 115200:
      localSpeed = B115200;
      break;
#ifndef OS_QNX
    case 230400:
      localSpeed = B230400;
      break;
#ifndef OS_DARWIN
    case 460800:
      localSpeed = B460800;
      break;
    case 921600:
      localSpeed = B921600;
      break;
#endif
#endif
    default :
      localSpeed = B9600;
  }

  if (cfsetispeed(&tios, localSpeed) == -1 ||
      cfsetospeed(&tios, localSpeed) == -1)
    return 0;

  tios.c_cflag |= (CREAD | CLOCAL);

  tios.c_cflag &= ~CSIZE;
  switch (dataBits) {
    case 5:
      tios.c_cflag |= CS5;
      break;
    case 6:
      tios.c_cflag |= CS6;
      break;
    case 7:
      tios.c_cflag |= CS7;
      break;
    case 8:
    default:
      tios.c_cflag |= CS8;
      break;
  }

  if (stopBits == 1)
    tios.c_cflag &=~ CSTOPB;
  else
    tios.c_cflag |= CSTOPB;

  if (parity == 'N') {
    tios.c_cflag &=~ PARENB;    
  } else if (parity == 'E') {
    tios.c_cflag |= PARENB;
    tios.c_cflag &=~ PARODD;
  } else {
    tios.c_cflag |= PARENB;
    tios.c_cflag |= PARODD;
  }

  tios.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

  if (parity == 'N') {
    tios.c_iflag &= ~INPCK;
  } else {
    tios.c_iflag |= INPCK;
  }

  tios.c_oflag &= ~OPOST;
  tios.c_cc[VMIN] = 0;
  tios.c_cc[VTIME] = 0;

  tcflush(port, TCIOFLUSH);
  if (tcsetattr(port, TCSANOW, &tios) < 0) {
    return 0;
  } else {
    return 1;
  }
}

void serialPortFlush(iodevTy port)
{
  tcflush(port, TCIOFLUSH);
}
