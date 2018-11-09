#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include "asyncio/timer.h"
#include <errno.h>
#include <inttypes.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#ifndef OS_WINDOWS
#include <unistd.h>
#endif
 
static unsigned gPortBase = 63300;
#ifdef OS_WINDOWS
// Windows have low performance loopback interface
static uint64_t gTotalPacketNum = 640000ULL;
#else
static uint64_t gTotalPacketNum = 4000000ULL;
#endif
static unsigned gGroupSize = 1000;
static unsigned gMessageSize = 16;
static unsigned gBatchSend = 0;

struct Context {
  uint64_t totalPacketNum;
  unsigned groupSize;
  unsigned messageSize;
  int port;
  Context(int portArg) : totalPacketNum(gTotalPacketNum), groupSize(gGroupSize), messageSize(gMessageSize), port(portArg) {}    
};

enum AIOSenderTy {
  aioSenderBlocking = 0,
  aioSenderAsync,
  aioSenderCoroutine
};

enum AIOReceiverTy {
  aioReceiverBlocking = 0,
  aioReceiverAsync,
  aioReceiverAsyncTimer,
  aioReceiverAsyncRT,
  aioReceiverCoroutine
};

struct SenderCtx {
  Context *config;
  asyncBase *localBase;
  socketTy clientSocket;
  aioObject *client;
  unsigned counter;
  char buffer[65536];  
};

struct ReceiverCtx {
  Context *config;
  asyncBase *base;
  socketTy serverSocket;
  aioObject *server;
  AIOReceiverTy type;
  
  timeMark beginPt;
  timeMark endPt;  
  bool started;
  uint64_t oldPacketsNum;
  uint64_t packetsNum; 
  char buffer[65536];  
  
  ReceiverCtx() : started(false), oldPacketsNum(0), packetsNum(0) {}
};


const char *aioSenderName[] = {
  "blocking",
  "async",
  "coroutine"
};

const char *aioReceiverName[] = {
  "blocking",
  "async",
  "async+timer",
  "async+timer+rt",
  "coroutine"
};

// ======================================================================
// =                                                                    =
// =                         Senders                                    =
// =                                                                    =
// ======================================================================

void *test_sync_sender(void *arg)
{
  char msg[65536];
  memset(msg, 'm', sizeof(msg));
  SenderCtx *senderCtx = (SenderCtx*)arg;    
  
  sockaddr_in destAddr;
  destAddr.sin_family = AF_INET;  
  destAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
  destAddr.sin_port = htons(senderCtx->config->port);

  for (uint64_t i = 0; i < senderCtx->config->totalPacketNum; i++) {
    if (sendto(senderCtx->clientSocket, msg, senderCtx->config->messageSize, 0, (sockaddr*)&destAddr, sizeof(destAddr)) == -1) {
      fprintf(stderr, "sendto return error %s\n", strerror(errno));
      exit(1);
    }
  }
  
  return 0;
}

void test_aio_writecb(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{ 
  SenderCtx *senderCtx = (SenderCtx*)arg;  
  if (senderCtx->counter >= senderCtx->config->totalPacketNum) {
    postQuitOperation(base);
    return;
  }
  
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(senderCtx->config->port);
  
  senderCtx->counter += senderCtx->config->groupSize;  
  for (unsigned i = 0; i < senderCtx->config->groupSize-1; i++)
    aioWriteMsg(base, object, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, 0, 0);
  aioWriteMsg(base, object, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, test_aio_writecb, senderCtx);
}

void test_aio_writecb_nobatch(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  SenderCtx *senderCtx = (SenderCtx*)arg;
  if (senderCtx->counter >= senderCtx->config->totalPacketNum) {
    postQuitOperation(base);
    return;
  }

  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(senderCtx->config->port);

  senderCtx->counter++;
  aioWriteMsg(base, object, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, test_aio_writecb_nobatch, senderCtx);
}

void *test_aio_sender(void *arg)
{
  SenderCtx *senderCtx = (SenderCtx*)arg;
  asyncBase *localBase = createAsyncBase(amOSDefault);

  senderCtx->localBase = localBase;
  senderCtx->client = newSocketIo(localBase, senderCtx->clientSocket);  
  senderCtx->counter = 0;
  
  // Send loop start
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(senderCtx->config->port);

  if (gBatchSend) {
    senderCtx->counter += senderCtx->config->groupSize;
    for (unsigned i = 0; i < senderCtx->config->groupSize-1; i++)
      aioWriteMsg(localBase, senderCtx->client, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, 0, 0);
    aioWriteMsg(localBase, senderCtx->client, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, test_aio_writecb, senderCtx);
  } else {
    senderCtx->counter++;
    aioWriteMsg(localBase, senderCtx->client, &address, &senderCtx->buffer, senderCtx->config->messageSize, afNone, 0, test_aio_writecb_nobatch, senderCtx);
  }
  
  asyncLoop(localBase);
  return 0;
}

void test_coroutine_sender_coro(void *arg)
{
  char msg[65536];
  memset(msg, 'm', sizeof(msg));
  SenderCtx *senderCtx = (SenderCtx*)arg;      
  
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(senderCtx->config->port); 
  for (uint64_t i = 0; i < senderCtx->config->totalPacketNum; i++)
    ioWriteMsg(senderCtx->localBase, senderCtx->client, &address, msg, senderCtx->config->messageSize, afNone, 0);
}

void *test_coroutine_sender(void *arg)
{
  asyncBase *localBase = createAsyncBase(amOSDefault); 
  
  SenderCtx *senderCtx = (SenderCtx*)arg;  
  senderCtx->localBase = localBase;
  senderCtx->client = newSocketIo(localBase, senderCtx->clientSocket);
  coroutineCall(coroutineNew(test_coroutine_sender_coro, senderCtx, 0x40000));
  asyncLoop(localBase);  
  return 0;
}

// ======================================================================
// =                                                                    =
// =                         Receivers                                  =
// =                                                                    =
// ======================================================================


// Blocking synchronous receiver
void *test_sync_receiver(void *arg)
{
  char msg[65536];
  ReceiverCtx *receiverCtx = (ReceiverCtx*)arg;  
  
  for (;;) {
    for (unsigned i = 0; i < receiverCtx->config->groupSize; i++) {
      sockaddr_in addr;
      socketLenTy len = sizeof(addr);
      recvfrom(receiverCtx->serverSocket, msg, sizeof(msg), 0, (sockaddr*)&addr, &len);
      if (!receiverCtx->started) {
        receiverCtx->started = true;
        receiverCtx->beginPt = getTimeMark();
      }
    }
    
    receiverCtx->packetsNum += receiverCtx->config->groupSize;
    receiverCtx->endPt = getTimeMark();
  }
}

// Asynchronous receiver callback
void test_readcb(AsyncOpStatus status,
                 asyncBase *base,
                 aioObject *socket,
                 HostAddress address,
                 size_t transferred,
                 void *arg)
{
  ReceiverCtx *ctx = (ReceiverCtx*)arg;
  ctx->started = true;
  if (ctx->packetsNum == 0)
    ctx->beginPt = getTimeMark();
  ctx->packetsNum++;
  if (ctx->packetsNum % ctx->config->groupSize == 0)
    ctx->endPt = getTimeMark();
  aioReadMsg(base, socket, &ctx->buffer, sizeof(ctx->buffer), afNone, 0, test_readcb, ctx);
}

// Asynchronous receiver with timer callback
void test_readcb_timer(AsyncOpStatus status,
                       asyncBase *base,
                       aioObject *socket,
                       HostAddress address,
                       size_t transferred,
                       void *arg)
{
  ReceiverCtx *ctx = (ReceiverCtx*)arg;
  
  if (status == aosSuccess) {
    ctx->started = true;
    if (ctx->packetsNum == 0)
      ctx->beginPt = getTimeMark();
    ctx->packetsNum++;
    if (ctx->packetsNum % ctx->config->groupSize == 0)
      ctx->endPt = getTimeMark();
    aioReadMsg(base, socket, &ctx->buffer, sizeof(ctx->buffer), afNone, 1000000, test_readcb_timer, ctx);
  } else {
    if (!ctx->started || ctx->oldPacketsNum != ctx->packetsNum) {
      ctx->oldPacketsNum = ctx->packetsNum;
      aioReadMsg(base, socket, &ctx->buffer, sizeof(ctx->buffer), afNone, 1000000, test_readcb_timer, ctx);
    } else {
      postQuitOperation(base);
    }
  }
}

// Asynchronous receiver with RT timer callback
void test_readcb_timer_rt(AsyncOpStatus status,
                          asyncBase *base,
                          aioObject *socket,
                          HostAddress address,
                          size_t transferred,
                          void *arg)
{
  ReceiverCtx *ctx = (ReceiverCtx*)arg;
  
  if (status == aosSuccess) {
    ctx->started = true;
    if (ctx->packetsNum == 0)
      ctx->beginPt = getTimeMark();
    ctx->packetsNum++;
    if (ctx->packetsNum % ctx->config->groupSize == 0)
      ctx->endPt = getTimeMark();
    aioReadMsg(base, socket, &ctx->buffer, sizeof(ctx->buffer), afRealtime, 1000000, test_readcb_timer_rt, ctx);
  } else {
    if (!ctx->started || ctx->oldPacketsNum != ctx->packetsNum) {
      ctx->oldPacketsNum = ctx->packetsNum;
      aioReadMsg(base, socket, &ctx->buffer, sizeof(ctx->buffer), afRealtime, 1000000, test_readcb_timer_rt, ctx);
    } else {
      postQuitOperation(base);
    }
  }
}

// Asynchronous receiver thread
void *test_aio_receiver(void *arg)
{
  ReceiverCtx *ctx = (ReceiverCtx*)arg;
  
  switch (ctx->type) {
    case aioReceiverAsync :
      aioReadMsg(ctx->base, ctx->server, &ctx->buffer, sizeof(ctx->buffer), afNone, 0, test_readcb, ctx);
      break;
    case aioReceiverAsyncTimer :
      aioReadMsg(ctx->base, ctx->server, &ctx->buffer, sizeof(ctx->buffer), afNone, 1000000, test_readcb_timer, ctx);
      break;      
    case aioReceiverAsyncRT :
      aioReadMsg(ctx->base, ctx->server, &ctx->buffer, sizeof(ctx->buffer), afRealtime, 1000000, test_readcb_timer_rt, ctx);
      break;
    default :
      fprintf(stderr, "Invalid receiver type, exiting...\n");
      exit(1);
  }
  
  asyncLoop(ctx->base);
  return 0;
}

// Asynchronous receiver coroutine
void test_coroutine_receiver_coro(void *arg)
{
  char msg[65536];
  ReceiverCtx *receiverCtx = (ReceiverCtx*)arg;  
  
  for (;;) {
    for (unsigned i = 0; i < receiverCtx->config->groupSize; i++) {
      ssize_t result = ioReadMsg(receiverCtx->base, receiverCtx->server, msg, sizeof(msg), afNone, 1000000);
      if (result == -1)
        return;      

      if (!receiverCtx->started) {
        receiverCtx->started = true;
        receiverCtx->beginPt = getTimeMark();
      }
    }
    
    receiverCtx->packetsNum += receiverCtx->config->groupSize;
    receiverCtx->endPt = getTimeMark();
  }
}

// Asynchronous coroutine receiver thread
void *test_coroutine_receiver(void *arg)
{
  ReceiverCtx *receiverCtx = (ReceiverCtx*)arg;
  coroutineTy *receiverCoro = coroutineNew(test_coroutine_receiver_coro, receiverCtx, 0x40000);
  coroutineCall(receiverCoro);
  asyncLoop(receiverCtx->base);
  return 0;
}


// ======================================================================
// =                                                                    =
// =                       Benchmark function                           =
// =                                                                    =
// ======================================================================

void test_aio(unsigned senderThreads, unsigned receiverThreads, int port, AIOSenderTy senderTy, AIOReceiverTy receiverTy)
{
  asyncBase *base = createAsyncBase(amOSDefault);
  
  HostAddress address;  
  Context ctx(port);
  SenderCtx *allSenders = new SenderCtx[senderThreads];
  ReceiverCtx *allReceivers = new ReceiverCtx[receiverThreads];  
  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(ctx.port);
  socketTy serverSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, receiverTy == aioReceiverBlocking ? 0 : 1);
  socketReuseAddr(serverSocket);
  if (socketBind(serverSocket, &address) != 0)
    return;

  timeMark pt = getTimeMark();
  for (unsigned i = 0; i < receiverThreads; i++) {
    allReceivers[i].base = base;
    allReceivers[i].config = &ctx;
    allReceivers[i].type = receiverTy;
    allReceivers[i].serverSocket = serverSocket;
    if (receiverTy != aioReceiverBlocking)
      allReceivers[i].server = newSocketIo(base, serverSocket);
    allReceivers[i].beginPt = pt;
    allReceivers[i].endPt = pt;    
    
    switch (receiverTy) {
      case aioReceiverBlocking : {
        std::thread thread(test_sync_receiver, &allReceivers[i]);
        thread.detach();
        break;
      }
      case aioReceiverAsync :
      case aioReceiverAsyncTimer :
      case aioReceiverAsyncRT : {
        std::thread thread(test_aio_receiver, &allReceivers[i]);
        thread.detach();        
        break;
      }
      case aioReceiverCoroutine : {
        std::thread thread(test_coroutine_receiver, &allReceivers[i]);
        thread.detach();        
        break;
      }
      default :
        fprintf(stderr, "Invalid receiver type, exiting...\n");
        exit(1);
    }
  }  
  
  for (unsigned i = 0; i < senderThreads; i++) {
    address.family = AF_INET;
    address.ipv4 = INADDR_ANY;
    address.port = 0;
    socketTy clientSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, senderTy == aioSenderBlocking ? 0 : 1);
    if (socketBind(clientSocket, &address) != 0)
      exit(1);
    
    allSenders[i].config = &ctx;
    allSenders[i].clientSocket = clientSocket;
    
    switch (senderTy) {
      case aioSenderBlocking : {
        std::thread thread(test_sync_sender, &allSenders[i]);
        thread.detach();
        break;
      }
      case aioSenderAsync : {
        std::thread thread(test_aio_sender, &allSenders[i]);
        thread.detach();        
        break;
      }
      case aioSenderCoroutine : {
        std::thread thread(test_coroutine_sender, &allSenders[i]);
        thread.detach();        
        break;
      }
      
      default :
        fprintf(stderr, "Invalid sender type, exiting...\n");
        exit(1);
    }
  }
  
  for (;;) {
    bool receivingActive = false;
    timeMark pt = getTimeMark();
    for (unsigned i = 0; i < 1; i++) {
      ReceiverCtx &ctx = allReceivers[i];
      uint64_t diff = usDiff(ctx.endPt, pt);
      if (diff < 1000000) {
        receivingActive = true;
        break;
      }
    }
    
    if (!receivingActive)
      break;
    
#ifdef OS_WINDOWS
    Sleep(1000);
#else
    sleep(1);
#endif
  }  
  
  postQuitOperation(base);
  
  uint64_t packetsNum = allReceivers[0].packetsNum;
  timeMark beginPt = allReceivers[0].beginPt;
  timeMark endPt = allReceivers[0].endPt;
  for (unsigned i = 1; i < receiverThreads; i++) {
    if (allReceivers[i].beginPt.mark < beginPt.mark)
      beginPt = allReceivers[i].beginPt;
    if (allReceivers[i].endPt.mark > endPt.mark)
      endPt = allReceivers[i].endPt;
    packetsNum += allReceivers[i].packetsNum;
  }
  
  double totalSeconds = usDiff(beginPt, endPt) / 1000000.0;

  printf("Threads S/R %u/%u sender=%s, receiver=%s, total messages: %" PRIu64 ", packet lost: %.2lf%%, elapsed time: %.3lf, rate: %.3lf msg/s\n",
         senderThreads,
         receiverThreads,
         aioSenderName[senderTy],
         aioReceiverName[receiverTy],
         packetsNum,
         (1.0 - (packetsNum / (double)(gTotalPacketNum*senderThreads))) * 100.0,
         totalSeconds,
         packetsNum/totalSeconds);  
}

int main(int argc, char **argv)
{
  initializeSocketSubsystem();
  
  unsigned port = gPortBase;
  
  // Blocking tests
  test_aio(1, 1, port++, aioSenderBlocking, aioReceiverBlocking);
  test_aio(4, 1, port++, aioSenderBlocking, aioReceiverBlocking);
  test_aio(1, 2, port++, aioSenderBlocking, aioReceiverBlocking);
  test_aio(1, 4, port++, aioSenderBlocking, aioReceiverBlocking);
  test_aio(4, 4, port++, aioSenderBlocking, aioReceiverBlocking);
  
  // Senders test with blocking receiver
  test_aio(1, 1, port++, aioSenderAsync, aioReceiverBlocking);
  test_aio(4, 1, port++, aioSenderAsync, aioReceiverBlocking);
  test_aio(1, 1, port++, aioSenderCoroutine, aioReceiverBlocking);
  test_aio(4, 1, port++, aioSenderCoroutine, aioReceiverBlocking);

  // Receivers test with blocking sender
  test_aio(1, 1, port++, aioSenderBlocking, aioReceiverAsync);
  test_aio(4, 1, port++, aioSenderBlocking, aioReceiverAsync);
  test_aio(1, 1, port++, aioSenderBlocking, aioReceiverAsyncTimer);
  test_aio(4, 1, port++, aioSenderBlocking, aioReceiverAsyncTimer);
  test_aio(1, 1, port++, aioSenderBlocking, aioReceiverAsyncRT);
  test_aio(4, 1, port++, aioSenderBlocking, aioReceiverAsyncRT);
  test_aio(1, 1, port++, aioSenderBlocking, aioReceiverCoroutine);
  test_aio(4, 1, port++, aioSenderBlocking, aioReceiverCoroutine);

  // Multi-threading receivers
  test_aio(1, 2, port++, aioSenderBlocking, aioReceiverAsync);
  test_aio(1, 4, port++, aioSenderBlocking, aioReceiverAsync);
  test_aio(4, 4, port++, aioSenderBlocking, aioReceiverAsync);
  
  test_aio(1, 2, port++, aioSenderBlocking, aioReceiverAsyncTimer);
  test_aio(1, 4, port++, aioSenderBlocking, aioReceiverAsyncTimer);
  test_aio(4, 4, port++, aioSenderBlocking, aioReceiverAsyncTimer);

  test_aio(1, 2, port++, aioSenderBlocking, aioReceiverAsyncRT);
  test_aio(1, 4, port++, aioSenderBlocking, aioReceiverAsyncRT);
  test_aio(4, 4, port++, aioSenderBlocking, aioReceiverAsyncRT);
  
  test_aio(1, 2, port++, aioSenderBlocking, aioReceiverCoroutine);
  test_aio(1, 4, port++, aioSenderBlocking, aioReceiverCoroutine);
  test_aio(4, 4, port++, aioSenderBlocking, aioReceiverCoroutine);
  
  return 0;
}
