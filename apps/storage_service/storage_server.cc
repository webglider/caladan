extern "C" {
#include <base/log.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
#include <runtime/runtime.h>
}

#include "net.h"
#include "sync.h"
#include "thread.h"

#include <iostream>
#include <memory>
#include <new>

#include "reflex.h"

#define RAM_DISK 1

constexpr unsigned int kSectorSize = 512;
constexpr uint64_t kStorageServicePort = 5000;
constexpr uint64_t kMaxSectorsPerPayload = 256;

uint64_t g_service_port;
#ifdef RAM_DISK
#define RAM_DISK_SIZE 134217728LL
std::unique_ptr<unsigned char[]> g_ram_disk;
#endif

class SharedTcpStream {
 public:
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : c_(c) {}
  ssize_t WriteFull(const void *buf, size_t len) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    return c_->WriteFull(buf, len);
  }
  ssize_t WritevFull(const struct iovec iov[], int iovcnt) {
    int i = 0;
    ssize_t sent = 0;
    struct iovec vs[iovcnt];
    memcpy(vs, iov, sizeof(*iov) * iovcnt);
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    while (iovcnt) {
      ssize_t ret = c_->Writev(&vs[i], iovcnt);
      if (ret <= 0) return ret;
      sent += ret;
      while (iovcnt && ret >= static_cast<ssize_t>(vs[i].iov_len)) {
        ret -= vs[i].iov_len;
        i++;
        iovcnt--;
      }
      if (ret) {
        vs[i].iov_base = (unsigned char *)vs[i].iov_base + ret;
        vs[i].iov_len -= ret;
      }
    }
    return sent;
  }

 private:
  std::shared_ptr<rt::TcpConn> c_;
  rt::Mutex sendMutex_;
};

class RequestContext { 
 public:
  RequestContext(std::shared_ptr<SharedTcpStream> c) : conn(c) {}
  binary_header_blk_t header;
  std::shared_ptr<SharedTcpStream> conn;
  char buf[kSectorSize * kMaxSectorsPerPayload];
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

void HandleGetRequest(RequestContext *ctx) {
  #ifndef RAM_DISK
  ssize_t ret = storage_read(ctx->buf, ctx->header.lba, ctx->header.lba_count);
  #else
  unsigned char *target_addr = g_ram_disk.get() + ((ctx->header.lba) % (RAM_DISK_SIZE/kSectorSize)) * kSectorSize;
  //memcpy(ctx->buf, target_addr, ctx->header.lba_count * kSectorSize);
  ssize_t ret = 0;
  #endif
  if (ret < 0)
  {
    log_err("storage_read failed: ret = %ld", ret);
    return;
  }
  size_t payload_size = kSectorSize * ctx->header.lba_count;
  struct iovec response[2] = {
      {
          .iov_base = &ctx->header,
          .iov_len = sizeof(ctx->header),
      },
      {
          .iov_base = target_addr,
          .iov_len = payload_size,
      },
  };
  ret = ctx->conn->WritevFull(response, 2);
  if (ret != static_cast<ssize_t>(sizeof(ctx->header) + payload_size)) {
    if (ret != -EPIPE && ret != -ECONNRESET)
      log_err_ratelimited("WritevFull failed: ret = %ld", ret);
  }
}

void HandleSetRequest(RequestContext *ctx) {
  #ifndef RAM_DISK
  ssize_t ret = storage_write(ctx->buf, ctx->header.lba, ctx->header.lba_count);
  #else
  unsigned char *target_addr = g_ram_disk.get() + ((ctx->header.lba) % (RAM_DISK_SIZE/kSectorSize)) * kSectorSize;
  memcpy(target_addr, ctx->buf, ctx->header.lba_count * kSectorSize);
  ssize_t ret = 0;
  #endif
  if (ret < 0)
    return;

  ret = ctx->conn->WriteFull(&ctx->header, sizeof(ctx->header));
  if (ret != static_cast<ssize_t>(sizeof(ctx->header))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void HandleRAMRequest(rt::TcpConn *c, binary_header_blk_t *h) {
  ssize_t ret = c->WriteFull(h, sizeof(*h));
  if (ret != static_cast<ssize_t>(sizeof(*h))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }

  unsigned char *target_addr = g_ram_disk.get() + ((h->lba) % (RAM_DISK_SIZE/kSectorSize)) * kSectorSize;
  size_t payload_size = h->lba_count * kSectorSize;
  if (h->opcode == CMD_GET) {
      ret = c->WriteFull(target_addr, payload_size);
      if (ret != static_cast<ssize_t>(payload_size)) {
        if (ret != 0 && ret != -ECONNRESET)
          log_err("tcp_read failed, ret = %ld", ret);
        return;
      }
  } else {
     ret = c->ReadFull(target_addr, payload_size);
     if (ret != static_cast<ssize_t>(payload_size)) {
        if (ret != 0 && ret != -ECONNRESET)
          log_err("tcp_read failed, ret = %ld", ret);
        return;
     }
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c) {
  auto resp = std::make_shared<SharedTcpStream>(c);
  while (true) {
    /* allocate context */
    auto ctx = new RequestContext(resp);
    binary_header_blk_t *h = &ctx->header;

    /* Receive a work request. */
    ssize_t ret = c->ReadFull(h, sizeof(*h));
    // log_err("read ret %ld", ret);
    if (ret != static_cast<ssize_t>(sizeof(*h))) {
      if (ret != 0 && ret != -ECONNRESET)
        log_err("read failed, ret = %ld", ret);
      delete ctx;
      return;
    }

    /* validate request */
    if (h->magic != sizeof(binary_header_blk_t) ||
        (h->opcode != CMD_GET && h->opcode != CMD_SET) ||
        h->lba_count > kMaxSectorsPerPayload) {
      log_err("invalid request %x %x %x", h->magic, h->opcode, h->lba_count);
      delete ctx;
      return;
    }

    HandleRAMRequest(c.get(), h);
    delete ctx;
#if 0
    /* spawn thread to handle storage request + response */
    if (h->opcode == CMD_SET) {
      size_t payload_size = h->lba_count * kSectorSize;
      ret = c->ReadFull(ctx->buf, payload_size);
      if (ret != static_cast<ssize_t>(payload_size)) {
        if (ret != 0 && ret != -ECONNRESET)
          log_err("tcp_read failed, ret = %ld", ret);
        delete ctx;
        return;
      }
      rt::Thread([=] {
        HandleSetRequest(ctx);
        delete ctx;
      })
          .Detach();
    } else {
      rt::Thread([=] {
        HandleGetRequest(ctx);
        delete ctx;
      })
          .Detach();
    }
#endif
  }
}

void MainHandler(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, g_service_port}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  #ifndef RAM_DISK
  BUG_ON(kSectorSize != storage_block_size());
  #endif
  
  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    std::cerr << "usage: [cfg_file] <port>" << std::endl;
    return -EINVAL;
  }

  g_service_port = kStorageServicePort;
  if(argc >= 3)
  {
    g_service_port = strtoll(argv[2], nullptr, 0);
    log_err("To listen on port %lu", g_service_port);
  }

  #ifdef RAM_DISK
  g_ram_disk.reset(new unsigned char[RAM_DISK_SIZE + kMaxSectorsPerPayload*kSectorSize]);
  log_err("allocated RAM disk");
  memset(g_ram_disk.get(), 0, RAM_DISK_SIZE);
  #endif

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
