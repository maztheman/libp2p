#include "asyncio/http.h"

#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"
#include <string.h>

static const char *httpClientPool = "HTTPClient";
static const char *httpPoolId = "HTTP";
static const char *httpPoolTimerId = "HTTPTimer";

typedef enum {
  httpOpConnect = 0,
  httpOpRequest
} HttpOpTy;

typedef struct ioHttpRequestArg {
  HTTPClient *client;
  HTTPOp *op;
  const char *request;
  size_t requestSize;
} ioHttpRequestArg;

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  int resultCode;
} coroReturnStruct;

static AsyncOpStatus httpParseStart(asyncOpRoot *opptr);

static asyncOpRoot *alloc()
{
  return (asyncOpRoot*)__tagged_alloc(sizeof(HTTPOp));
}

static int cancel(asyncOpRoot *opptr)
{
  HTTPClient *client = (HTTPClient*)opptr->object;
  cancelIo(client->isHttps ? (aioObjectRoot*)client->sslSocket : (aioObjectRoot*)client->plainSocket);
  return 0;
}

static void connectFinish(asyncOpRoot *opptr)
{
  ((httpConnectCb*)opptr->callback)(opGetStatus(opptr), (HTTPClient*)opptr->object, opptr->arg);
}

static void requestFinish(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)opptr->object;
  client->contentType = op->contentType;

  // Append zero byte
  *(char*)dynamicBufferAlloc(&client->out, 1) = 0;

  // Write pointer and body size to client
  client->body.data = (uint8_t*)client->out.data + op->bodyOffset;
  client->body.size = client->out.size - op->bodyOffset - 1;

  ((httpRequestCb*)opptr->callback)(opGetStatus(opptr), client, op->resultCode, opptr->arg);
}


static void coroutineConnectCb(AsyncOpStatus status, HTTPClient *client, void *arg)
{
  __UNUSED(client)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineRequestCb(AsyncOpStatus status, HTTPClient *client, int resultCode, void *arg)
{
  __UNUSED(client)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->resultCode = resultCode;
  coroutineCall(r->coroutine);
}

void httpRequestProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  asyncOpRoot *opptr = (asyncOpRoot*)arg;
  HTTPClient *client = (HTTPClient*)opptr->object;
  httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
  resumeParent(opptr, status);
}

void httpsRequestProc(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  asyncOpRoot *opptr = (asyncOpRoot*)arg;
  HTTPClient *client = (HTTPClient*)opptr->object;
  httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
  resumeParent(opptr, status);
}

void httpConnectProc(AsyncOpStatus status, aioObject *object, void *arg)
{
  __UNUSED(object)
  resumeParent((asyncOpRoot*)arg, status);
}

void httpsConnectProc(AsyncOpStatus status, SSLSocket *object, void *arg)
{
  __UNUSED(object)
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus httpConnectStart(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)opptr->object;
  if (op->state == 0) {
    op->state = 1;
    if (client->isHttps)
      aioSslConnect(client->sslSocket, &op->address, 0, httpsConnectProc, op);
    else
      aioConnect(client->plainSocket, &op->address, 0, httpConnectProc, op);
    return aosPending;
  } else {
    return aosSuccess;
  }
}

static AsyncOpStatus httpParseStart(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)op->root.object;

  if (op->state == 0) {
    dynamicBufferClear(&client->out);
    httpInit(&client->state);
    op->state = 1;
  }

  for (;;) {
    switch (httpParse(&client->state, op->parseCallback, op)) {
      case httpResultOk :
        return aosSuccess;
      case httpResultNeedMoreData : {
        // copy 'tail' to begin of buffer
        size_t offset = httpDataRemaining(&client->state);
        if (offset)
          memcpy(client->inBuffer, httpDataPtr(&client->state), offset);

        asyncOpRoot *readOp;
        size_t bytesTransferred = 0;
        if (client->isHttps)
          readOp = implSslRead(client->sslSocket,
                               client->inBuffer+offset,
                               client->inBufferSize-offset,
                               afNone,
                               0,
                               httpsRequestProc,
                               op,
                               &bytesTransferred);
        else
          readOp = implRead(client->plainSocket,
                            client->inBuffer+offset,
                            client->inBufferSize-offset,
                            afNone,
                            0,
                            httpRequestProc,
                            op,
                            &bytesTransferred);

        client->inBufferOffset = offset;
        if (readOp) {
          opStart(readOp);
          return aosPending;
        } else {
          httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+bytesTransferred);
        }
        break;
      }
      case httpResultError :
        return aosUnknownError;
    }
  }
}


static HTTPOp *allocHttpOp(aioExecuteProc executeProc,
                           aioFinishProc finishProc,
                           HTTPClient *client,
                           int type,
                           httpParseCb parseCallback,
                           void *callback,
                           void *arg,
                           uint64_t timeout)
{
  HTTPOp *op = (HTTPOp*)
    initAsyncOpRoot(httpPoolId, httpPoolTimerId, alloc, executeProc, cancel, finishProc, &client->root, callback, arg, 0, type, timeout);

  op->parseCallback = parseCallback;
  op->resultCode = 0;
  op->contentType.data = 0;
  op->bodyOffset = 0;
  op->state = 0;
  return op;
}

void httpParseDefault(HttpComponent *component, void *arg)
{
  HTTPOp *op = (HTTPOp*)arg;
  HTTPClient *client = (HTTPClient*)op->root.object;
  switch (component->type) {
    case httpDtStartLine : {
      op->resultCode = component->startLine.code;
      break;
    }

    case httpDtHeaderEntry : {
      switch (component->header.entryType) {
        case hhContentType : {
          char *out = (char*)dynamicBufferAlloc(&client->out, component->header.stringValue.size+1);
          memcpy(out, component->header.stringValue.data, component->header.stringValue.size);
          out[component->header.stringValue.size] = 0;

          op->contentType.data = out;
          op->contentType.size = component->header.stringValue.size;
          break;
        }
      }

      break;
    }

    case httpDtData :
    case httpDtDataFragment : {
      if (op->bodyOffset == 0)
          op->bodyOffset = client->out.offset;

      char *out = (char*)dynamicBufferAlloc(&client->out, component->data.size);
      memcpy(out, component->data.data, component->data.size);
      break;
    }
  }
}

static void httpClientDestructor(aioObjectRoot *root)
{
  HTTPClient *client = (HTTPClient*)root;
  if (client->isHttps)
    sslSocketDelete(client->sslSocket);
  else
    deleteAioObject(client->plainSocket);
  objectRelease(root, httpClientPool);
}

HTTPClient *httpClientNew(asyncBase *base, aioObject *socket)
{
  HTTPClient *client = objectGet(httpClientPool);
  if (!client) {
    client = (HTTPClient*)malloc(sizeof(HTTPClient));
    client->inBuffer = (uint8_t*)malloc(65536);
    client->inBufferSize = 65536;
    dynamicBufferInit(&client->out, 65536);
  }

  initObjectRoot(&client->root, base, ioObjectUserDefined, httpClientDestructor);
  client->isHttps = 0;
  client->inBufferOffset = 0;
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->plainSocket = socket;
  return client;
}

HTTPClient *httpsClientNew(asyncBase *base, SSLSocket *socket)
{
  HTTPClient *client = objectGet(httpClientPool);
  if (!client) {
    client = (HTTPClient*)malloc(sizeof(HTTPClient));
    client->inBuffer = (uint8_t*)malloc(65536);
    client->inBufferSize = 65536;
    dynamicBufferInit(&client->out, 65536);
  }

  initObjectRoot(&client->root, base, ioObjectUserDefined, httpClientDestructor);
  client->isHttps = 1;
  client->inBufferOffset = 0;
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->sslSocket = socket;
  return client;
}

void httpClientDelete(HTTPClient *client)
{
  objectDelete(&client->root);
}

void aioHttpConnect(HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpConnectCb callback,
                    void *arg)
{
  HTTPOp *op = allocHttpOp(httpConnectStart, connectFinish, client, httpOpConnect, 0, (void*)callback, arg, usTimeout);
  op->address = *address;
  opStart(&op->root);
}

void writeCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object);
  __UNUSED(transferred);
  asyncOpRoot *op = (asyncOpRoot*)arg;
  if (status == aosSuccess)
    opStart(op);
  else
    opCancel(op, opGetGeneration(op), status);
}

void sslWriteCb(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
{
  __UNUSED(object);
  __UNUSED(transferred);
  asyncOpRoot *op = (asyncOpRoot*)arg;
  if (status == aosSuccess)
    opStart(op);
  else
    opCancel(op, opGetGeneration(op), status);
}

void aioHttpRequest(HTTPClient *client,
                    const char *request,
                    size_t requestSize,
                    uint64_t usTimeout,
                    httpParseCb parseCallback,
                    httpRequestCb callback,
                    void *arg)
{
  HTTPOp *op = allocHttpOp(httpParseStart, requestFinish, client, httpOpConnect, parseCallback, (void*)callback, arg, usTimeout);
  if (client->isHttps)
    aioSslWrite(client->sslSocket, request, requestSize, afNone, 0, sslWriteCb, op);
  else
    aioWrite(client->plainSocket, request, requestSize, afNone, 0, writeCb, op);
}


int ioHttpConnect(HTTPClient *client, const HostAddress *address, uint64_t usTimeout)
{
  HTTPOp *op = allocHttpOp(httpConnectStart, 0, client, httpOpConnect, 0, 0, 0, usTimeout);
  op->address = *address;
  combinerCall(&client->root, 1, &op->root, aaStart);
  coroutineYield();
  AsyncOpStatus status = opGetStatus(&op->root);
  objectRelease(&op->root, op->root.poolId);
  objectDecrementReference(&client->root, 1);
  return status == aosSuccess ? 0 : -(int)status;
}

void ioHttpRequestStart(void *arg)
{
  ioHttpRequestArg *hrArgs = (ioHttpRequestArg*)arg;
  if (hrArgs->client->isHttps)
    aioSslWrite(hrArgs->client->sslSocket, hrArgs->request, hrArgs->requestSize, afNone, 0, sslWriteCb, hrArgs->op);
  else
    aioWrite(hrArgs->client->plainSocket, hrArgs->request, hrArgs->requestSize, afNone, 0, writeCb, hrArgs->op);
}

int ioHttpRequest(HTTPClient *client,
                  const char *request,
                  size_t requestSize,
                  uint64_t usTimeout,
                  httpParseCb parseCallback)
{
  ioHttpRequestArg hrArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  HTTPOp *op = allocHttpOp(httpParseStart, requestFinish, client, httpOpRequest, parseCallback, (void*)coroutineRequestCb, &r, usTimeout);
  hrArgs.client = client;
  hrArgs.op = op;
  hrArgs.request = request;
  hrArgs.requestSize = requestSize;
  coroutineSetYieldCallback(ioHttpRequestStart, &hrArgs);
  coroutineYield();
  return r.status == aosSuccess ? r.resultCode : -r.status;
}
