#include "asyncioImpl.h"
#include "asyncio/coroutine.h"
#include "atomic.h"

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#define MAX_EVENTS 256

enum pipeDescrs {
  Read = 0,
  Write
};

typedef enum pipeCmd {
  Reset = 0,
  UserEvent
} pipeCmd;

__NO_PADDING_BEGIN
typedef struct pipeMsg {
  pipeCmd cmd;
  void *data;
} pipeMsg;

typedef struct epollBase {
  asyncBase B;
  int epollFd;
  int pipeFd[2];
  aioObject *pipeObject;
} epollBase;

typedef struct aioTimer {
  aioObjectRoot root;
  int fd;
  asyncOpRoot *op;
} aioTimer;
__NO_PADDING_END

static void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);
void epollPostEmptyOperation(asyncBase *base);
void epollNextFinishedOperation(asyncBase *base);
aioObject *epollNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *epollNewAsyncOp(void);
int epollCancelAsyncOp(asyncOpRoot *opptr);
void epollDeleteObject(aioObject *object);
void epollInitializeTimer(asyncBase *base, asyncOpRoot *op);
void epollStartTimer(asyncOpRoot *op, uint64_t usTimeout, int periodic);
void epollStopTimer(asyncOpRoot *op);
void epollActivate(aioUserEvent *op);
AsyncOpStatus epollAsyncConnect(asyncOpRoot *opptr);
AsyncOpStatus epollAsyncAccept(asyncOpRoot *opptr);
AsyncOpStatus epollAsyncRead(asyncOpRoot *opptr);
AsyncOpStatus epollAsyncWrite(asyncOpRoot *opptr);
AsyncOpStatus epollAsyncReadMsg(asyncOpRoot *op);
AsyncOpStatus epollAsyncWriteMsg(asyncOpRoot *op);

static struct asyncImpl epollImpl = {
  combiner,
  epollPostEmptyOperation,
  epollNextFinishedOperation,
  epollNewAioObject,
  epollNewAsyncOp,
  epollCancelAsyncOp,
  epollDeleteObject,
  epollInitializeTimer,
  epollStartTimer,
  epollStopTimer,
  epollActivate,
  epollAsyncConnect,
  epollAsyncAccept,
  epollAsyncRead,
  epollAsyncWrite,
  epollAsyncReadMsg,
  epollAsyncWriteMsg
};

static void epollControl(int epollFd, int action, uint32_t events, int fd, void *ptr)
{
  struct epoll_event ev;
  ev.events = events;
  ev.data.ptr = ptr;
  if (epoll_ctl(epollFd,
                action,
                fd,
                &ev) == -1)
    fprintf(stderr, "epoll_ctl error, errno: %s\n", strerror(errno));
}

static int getFd(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice :
      return object->hDevice;
    case ioObjectSocket :
      return object->hSocket;
    default :
      return -1;
  }
}

asyncBase *epollNewAsyncBase()
{
  epollBase *base = malloc(sizeof(epollBase));
  if (base) {
    pipe(base->pipeFd);
    base->B.methodImpl = epollImpl;
    base->epollFd = epoll_create(MAX_EVENTS);
    if (base->epollFd == -1) {
      fprintf(stderr, " * epollNewAsyncBase: epoll_create failed\n");
    }

    base->pipeObject = epollNewAioObject(&base->B, ioObjectDevice, &base->pipeFd[Read]);

    epollControl(base->epollFd, EPOLL_CTL_MOD, EPOLLIN | EPOLLONESHOT, base->pipeFd[Read], base->pipeObject);
  }

  return (asyncBase *)base;
}

void epollPostEmptyOperation(asyncBase *base)
{
  unsigned count = base->messageLoopThreadCounter;
  unsigned i;
  for (i = 0; i < count; i++) {
    pipeMsg msg = {Reset, 0};
    epollBase *localBase = (epollBase *)base;
    write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
  }
}

static void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  epollBase *base = (epollBase*)object->base;
  tag_t currentTag = tag;
  asyncOpRoot *newOp = op;
  int hasFd = object->type == ioObjectDevice || object->type == ioObjectSocket;

  while (currentTag) {
    int hasReadOp = object->readQueue.head != 0;
    int hasWriteOp = object->writeQueue.head != 0;

    if (currentTag & TAG_ERROR) {
      // EPOLLRDHUP mapped to TAG_ERROR, cancel all operations with aosDisconnected status
      int available;
      int fd = getFd((aioObject*)object);
      ioctl(fd, FIONREAD, &available);
      if (available == 0)
        cancelOperationList(&object->readQueue, &threadLocalQueue, aosDisconnected);
      cancelOperationList(&object->writeQueue, &threadLocalQueue, aosDisconnected);
    }

    if (currentTag & TAG_CANCELIO) {
      cancelOperationList(&object->readQueue, &threadLocalQueue, aosCanceled);
      cancelOperationList(&object->writeQueue, &threadLocalQueue, aosCanceled);
    }

    if (currentTag & TAG_DELETE) {
      // Perform delete and exit combiner
      object->destructor(object);
      return;
    }
    
    // Check for pending operations
    tag_t pendingOperationsNum;
    tag_t enqueuedOperationsNum = 0;
    tag_t needStart = currentTag;
    if ( (pendingOperationsNum = __tag_get_opcount(currentTag)) ) {
      if (newOp) {
        // Don't try synchonously execute operation second time
        processAction(newOp, actionType, &threadLocalQueue, &needStart);
        enqueuedOperationsNum = 1;
        newOp = 0;
      } else {
        while (enqueuedOperationsNum < pendingOperationsNum)
          processOperationList(object, &threadLocalQueue, &needStart, &enqueuedOperationsNum);
      }
    }

    if (needStart & TAG_READ_MASK)
      executeOperationList(&object->readQueue, &threadLocalQueue);
    if (needStart & TAG_WRITE_MASK)
      executeOperationList(&object->writeQueue, &threadLocalQueue);

    // I/O multiplexer configuration
    if (hasFd) {
      int fd = getFd((aioObject*)object);
      int needUpdate = 0;
      uint32_t events = 0;

      if (object->readQueue.head) {
        if (!hasReadOp || (currentTag & TAG_READ_MASK)) {
          needUpdate = 1;
          events |= EPOLLIN;
        }
      } else {
        if (hasReadOp && !(currentTag & TAG_READ_MASK))
          needUpdate = 1;
      }

      if (object->writeQueue.head) {
        if (!hasWriteOp || (currentTag & TAG_WRITE_MASK)) {
          events = EPOLLOUT;
          needUpdate = 1;
        }
      } else {
        if (hasWriteOp && !(currentTag & TAG_WRITE_MASK))
          needUpdate = 1;
      }

      if (needUpdate)
        epollControl(base->epollFd, EPOLL_CTL_MOD, events ? events | EPOLLONESHOT | EPOLLRDHUP : 0, fd, object);
    }
    
    // Try exit combiner
    tag_t processed = __tag_make_processed(currentTag, enqueuedOperationsNum);
    currentTag = __tag_atomic_fetch_and_add(&object->tag, -processed);
    currentTag -= processed;
  }
}

void epollNextFinishedOperation(asyncBase *base)
{
  int nfds, n;
  struct epoll_event events[MAX_EVENTS];
  epollBase *localBase = (epollBase *)base;
  messageLoopThreadId = __sync_fetch_and_add(&base->messageLoopThreadCounter, 1);

  while (1) {
    do {
      executeThreadLocalQueue();
      nfds = epoll_wait(localBase->epollFd, events, MAX_EVENTS, threadLocalQueue.head ? 0 : 500);
      time_t currentTime = time(0);
      if (currentTime % base->messageLoopThreadCounter == messageLoopThreadId)
        processTimeoutQueue(base, currentTime);
    } while (nfds <= 0 && errno == EINTR);

    for (n = 0; n < nfds; n++) {
      tag_t timerId;
      aioObjectRoot *object;
      __tagged_pointer_decode(events[n].data.ptr, (void**)&object, &timerId);
      if (object == &localBase->pipeObject->root) {
        pipeMsg msg;
        int available;
        int fd = localBase->pipeFd[Read];
        ioctl(fd, FIONREAD, &available);
        int i;
        for (i = 0; i < available / (int)sizeof(pipeMsg); i++) {
          read(fd, &msg, sizeof(pipeMsg));
          asyncOpRoot *op = (asyncOpRoot*)msg.data;
          switch (msg.cmd) {
            case Reset :
              while (threadLocalQueue.head)
                executeThreadLocalQueue();
              epollControl(localBase->epollFd, EPOLL_CTL_MOD, EPOLLIN | EPOLLONESHOT, fd, object);
              __uint_atomic_fetch_and_add(&base->messageLoopThreadCounter, 0u-1);
              return;
            case UserEvent :
              op->finishMethod(op);
              break;
          }
        }

        epollControl(localBase->epollFd, EPOLL_CTL_MOD, EPOLLIN | EPOLLONESHOT, fd, object);
      } else if (object->type == ioObjectTimer) {
        uint64_t data;
        aioTimer *timer = (aioTimer*)object;
        if (read(timer->fd, &data, sizeof(data))) {
          asyncOpRoot *op = timer->op;
          if (op->opCode == actUserEvent) {
            aioUserEvent *event = (aioUserEvent*)op;
            if (event->counter > 0 && --event->counter == 0) {
              epollStopTimer(op);
            } else {
              // We need rearm epoll for timer
              epollControl(localBase->epollFd,
                           EPOLL_CTL_MOD,
                           EPOLLIN | EPOLLONESHOT,
                           timer->fd,
                           __tagged_pointer_make(timer, opGetGeneration(op)));
            }

            op->finishMethod(op);
          } else {
            opCancel(op, opEncodeTag(op, timerId), aosTimeout);
          }
        }
      } else {
        tag_t eventMask = 0;
        if (events[n].events & EPOLLIN)
          eventMask |= TAG_READ;
        if (events[n].events & EPOLLOUT)
          eventMask |= TAG_WRITE;
        if (events[n].events & EPOLLRDHUP)
          eventMask |= TAG_ERROR;

        tag_t currentTag = __tag_atomic_fetch_and_add(&object->tag, eventMask);
        if (!currentTag)
          combiner(object, eventMask, 0, aaNone);
      }
    }
  }
}


aioObject *epollNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  epollBase *localBase = (epollBase*)base;
  aioObject *object = __tagged_alloc(sizeof(aioObject));
  initObjectRoot(&object->root, base, type, (aioObjectDestructor*)epollDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy *)data;
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy *)data;
      break;
    default :
      break;
  }

  object->u32 = 0;
  epollControl(localBase->epollFd, EPOLL_CTL_ADD, 0, getFd(object), object);
  return object;
}

asyncOpRoot *epollNewAsyncOp()
{
  asyncOp *op = malloc(sizeof(asyncOp));
  if (op) {
    op->internalBuffer = 0;
    op->internalBufferSize = 0;
  }

  return &op->root;
}

int epollCancelAsyncOp(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return 1;
}

void epollDeleteObject(aioObject *object)
{
  int fd = getFd(object);
  epollBase *localBase = (epollBase*)object->root.base;
  epollControl(localBase->epollFd, EPOLL_CTL_DEL, 0, fd, 0);
  close(fd);
  free(object);
}

void epollInitializeTimer(asyncBase *base, asyncOpRoot *op)
{
  epollBase *localBase = (epollBase*)base;
  aioTimer *timer = __tagged_alloc(sizeof(aioTimer));
  timer->root.base = base;
  timer->root.type = ioObjectTimer;
  timer->fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
  timer->op = op;
  epollControl(localBase->epollFd, EPOLL_CTL_ADD, 0, timer->fd, timer);
  op->timerId = timer;
}

void epollStartTimer(asyncOpRoot *op, uint64_t usTimeout, int periodic)
{
  struct itimerspec its;
  its.it_value.tv_sec = usTimeout / 1000000;
  its.it_value.tv_nsec = (usTimeout % 1000000) * 1000;
  its.it_interval.tv_sec = periodic ? its.it_value.tv_sec : 0;
  its.it_interval.tv_nsec = periodic ? its.it_value.tv_nsec : 0;

  aioTimer *timer = (aioTimer*)op->timerId;
  timerfd_settime(timer->fd, 0, &its, 0);
  epollControl(((epollBase*)timer->root.base)->epollFd,
               EPOLL_CTL_MOD,
               EPOLLIN | EPOLLONESHOT,
               timer->fd,
               __tagged_pointer_make(timer, opGetGeneration(op)));
}


void epollStopTimer(asyncOpRoot *op)
{
  uint64_t data;
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  aioTimer *timer = (aioTimer*)op->timerId;
  timerfd_settime(timer->fd, 0, &its, 0);
  epollControl(((epollBase*)timer->root.base)->epollFd, EPOLL_CTL_MOD, 0, timer->fd, &timer->root);
  while (read(timer->fd, &data, sizeof(data)) > 0)
    continue;
}


void epollActivate(aioUserEvent *op)
{
  pipeMsg msg = {UserEvent, (void *)op};
  epollBase *localBase = (epollBase *)op->base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}


AsyncOpStatus epollAsyncConnect(asyncOpRoot *opptr)
{ 
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);
  if (op->state == 0) {
    op->state = 1;
    struct sockaddr_in localAddress;
    localAddress.sin_family = op->host.family;
    localAddress.sin_addr.s_addr = op->host.ipv4;
    localAddress.sin_port = op->host.port;
    int result = connect(fd, (struct sockaddr *)&localAddress, sizeof(localAddress));
    if (result == -1 && errno != EINPROGRESS)
      return aosUnknownError;
    else
      return aosPending;
  } else {
    int error;
    socklen_t size = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
    return (error == 0) ? aosSuccess : aosUnknownError;
  }
}


AsyncOpStatus epollAsyncAccept(asyncOpRoot *opptr)
{
  struct sockaddr_in clientAddr;
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);
  socklen_t clientAddrSize = sizeof(clientAddr);
  op->acceptSocket =
    accept(fd, (struct sockaddr *)&clientAddr, &clientAddrSize);

  if (op->acceptSocket != -1) {
    int current = fcntl(op->acceptSocket, F_GETFL);
    fcntl(op->acceptSocket, F_SETFL, O_NONBLOCK | current);
    op->host.family = 0;
    op->host.ipv4 = clientAddr.sin_addr.s_addr;
    op->host.port = clientAddr.sin_port;
    return aosSuccess;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus epollAsyncRead(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  ssize_t bytesRead = read(fd,
                           (uint8_t *)op->buffer + op->bytesTransferred,
                           op->transactionSize - op->bytesTransferred);

  if (bytesRead > 0) {
    op->bytesTransferred += (size_t)bytesRead;
    if (op->root.flags & afWaitAll && op->bytesTransferred < op->transactionSize)
      return aosPending;
    else
      return aosSuccess;
  } else if (bytesRead == 0) {
    return op->transactionSize - op->bytesTransferred > 0 ? aosDisconnected : aosSuccess;
  } else {
    return errno == EAGAIN ? aosPending : aosUnknownError;
  }
}


AsyncOpStatus epollAsyncWrite(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  ssize_t bytesWritten = write(fd,
                               (uint8_t *)op->buffer + op->bytesTransferred,
                               op->transactionSize - op->bytesTransferred);
  if (bytesWritten > 0) {
    op->bytesTransferred += (size_t)bytesWritten;
    if (op->root.flags & afWaitAll && op->bytesTransferred < op->transactionSize)
      return aosPending;
    else
      return aosSuccess;
  } else if (bytesWritten == 0) {
    return op->transactionSize - op->bytesTransferred > 0 ? aosDisconnected : aosSuccess;
  } else {
    return errno == EAGAIN ? aosPending : aosUnknownError;
  }
}


AsyncOpStatus epollAsyncReadMsg(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  struct sockaddr_in source;
  socklen_t addrlen = sizeof(source);
  ssize_t result = recvfrom(fd, op->buffer, op->transactionSize, 0, (struct sockaddr*)&source, &addrlen);
  if (result != -1) {
    op->host.family = 0;
    op->host.ipv4 = source.sin_addr.s_addr;
    op->host.port = source.sin_port;
    op->bytesTransferred = (size_t)result;
    return aosSuccess;
  } else {
    if (errno == EAGAIN)
      return aosPending;
    if (errno == ENOMEM)
      return aosBufferTooSmall;
    else
      return aosUnknownError;
  }
}


AsyncOpStatus epollAsyncWriteMsg(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  struct sockaddr_in remoteAddress;
  remoteAddress.sin_family = op->host.family;
  remoteAddress.sin_addr.s_addr = op->host.ipv4;
  remoteAddress.sin_port = op->host.port;
  ssize_t result = sendto(fd, op->buffer, op->transactionSize, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
  if (result != -1) {
    return aosSuccess;
  }

  return aosPending;
}
