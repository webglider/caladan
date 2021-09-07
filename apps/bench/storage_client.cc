extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
}

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>
#include <random>

#include "net.h"
#include "reflex.h"
#include "runtime.h"
#include "sync.h"
#include "thread.h"

namespace {

using namespace std::chrono;
using sec = duration<double>;

constexpr uint16_t kNetperfPort = 8080;
constexpr unsigned int kSectorSize = 512;

#define MAX_IO_SIZE 262144
#define SECTOR_SIZE 512
#define TOTAL_BLOCK_COUNT 547002288LL
#define LBA_ALIGNMENT (~0x7)
#define MAX_IO_DEPTH 128
#define MAX_CONNS 128

void ReadOne(rt::TcpConn *c, unsigned long lba, unsigned int lba_count) {
  binary_header_blk_t hdr;
  hdr.magic = sizeof(hdr);
  hdr.opcode = CMD_GET;
  hdr.lba = lba;
  hdr.lba_count = lba_count;
  hdr.tsc = microtime();
  ssize_t ret = c->WriteFull(&hdr, sizeof(hdr));
  if (ret != static_cast<ssize_t>(sizeof(hdr)))
    panic("write hdr failed, ret = %ld", ret);
}

void WriteOne(rt::TcpConn *c, unsigned long lba, unsigned int lba_count,
              const void *buf) {
  binary_header_blk_t hdr;
  hdr.magic = sizeof(hdr);
  hdr.opcode = CMD_SET;
  hdr.lba = lba;
  hdr.lba_count = lba_count;
  hdr.tsc = microtime();
  ssize_t ret = c->WriteFull(&hdr, sizeof(hdr));
  if (ret != static_cast<ssize_t>(sizeof(hdr)))
    panic("write hdr failed, ret = %ld", ret);
  ret = c->WriteFull(buf, lba_count * kSectorSize);
  if (ret != static_cast<ssize_t>(lba_count * kSectorSize))
    panic("write payload failed, ret = %ld", ret);
}

uint64_t CompleteOne(rt::TcpConn *c, void *buf) {
  binary_header_blk_t hdr;
  ssize_t ret = c->ReadFull(&hdr, sizeof(hdr));
  if (ret != static_cast<ssize_t>(sizeof(hdr)))
    panic("read hdr failed, ret = %ld", ret);
  if (hdr.opcode == CMD_GET) {
    ret = c->ReadFull(buf, hdr.lba_count * kSectorSize);
    if (ret != hdr.lba_count * kSectorSize)
      panic("read payload failed, ret = %ld", ret);
  }
  return microtime() - hdr.tsc;
}

std::vector<uint64_t> ClientWorker(std::unique_ptr<rt::TcpConn> c,
                                   uint64_t stop_time, int depth,
                                   int block_size, int read_ratio) {
  std::vector<uint64_t> samples;
  std::mt19937 rg(rand());
  std::uniform_int_distribution<size_t> wd(0, TOTAL_BLOCK_COUNT-1);
  std::uniform_int_distribution<uint32_t> read_d(0, 100);
  void *buf = malloc(block_size);

  while (depth--) {
    bool is_write = (read_d(rg) < (100-read_ratio));
    unsigned long lba = wd(rg) & LBA_ALIGNMENT;
    if (is_write) {
      WriteOne(c.get(), lba, block_size / kSectorSize, buf);
    } else {
      ReadOne(c.get(), lba, block_size / kSectorSize);
    }
  }

  while (microtime() <= stop_time) {
    samples.push_back(CompleteOne(c.get(), buf));
    bool is_write = (read_d(rg) < (100-read_ratio));
    unsigned long lba = wd(rg) & LBA_ALIGNMENT;
    if (is_write) {
      WriteOne(c.get(), lba, block_size / kSectorSize, buf);
    } else {
      ReadOne(c.get(), lba, block_size / kSectorSize);
    }
  }

  free(buf);
  return samples;
}

void RunClient(netaddr raddr, int threads, int time, int depth,
               int block_size, int read_ratio) {
  // setup experiment
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < threads; ++i) {
    std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, raddr));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
    conns.emplace_back(std::move(outc));
  }

  // |--- start experiment duration timing ---|
  barrier();
  uint64_t stop_time = microtime() + time * 1000000;
  barrier();

  // run the experiment
  std::vector<rt::Thread> ths;
  std::vector<uint64_t> samples[threads];
  for (int i = 0; i < threads; ++i) {
    ths.emplace_back(
        rt::Thread([&conns, &samples, i, stop_time, depth, block_size, read_ratio] {
          samples[i] =
              ClientWorker(std::move(conns[i]), stop_time, depth, block_size, read_ratio);
        }));
  }
  for (auto &t : ths) t.Join();

  // Accumulate timings from all workers
  std::vector<uint64_t> timings;
  for (int i = 0; i < threads; ++i) {
    auto &v = samples[i];
    timings.insert(timings.end(), v.begin(), v.end());
  }

  // report results
  std::sort(timings.begin(), timings.end());
  double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
  double mean = sum / timings.size();
  double count = static_cast<double>(timings.size());
  uint64_t p9 = timings[count * 0.9];
  uint64_t p99 = timings[count * 0.99];
  uint64_t p999 = timings[count * 0.999];
  uint64_t p9999 = timings[count * 0.9999];
  uint64_t min = timings[0];
  uint64_t max = timings[timings.size() - 1];
  double iops = count / (double)time;
  std::cout << std::setprecision(2) << std::fixed << " iops: " << iops
            << " n: " << timings.size() << " min: " << min << " mean: " << mean
            << " 90%: " << p9 << " 99%: " << p99 << " 99.9%: " << p999
            << " 99.99%: " << p9999 << " max: " << max << std::endl;
  std::cout << "Throughput: " << iops * block_size / 1000 / 1000 << "MB/s "
            << "Gbits / sec: " << iops * block_size * 8 / 1000 / 1000 / 1000 << std::endl;
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;
  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;
  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

}  // anonymous namespace

int main(int argc, char *argv[]) {
  if (argc < 8) {
    std::cerr << "usage: [cfg_file] [ip_addr] [threads] [samples] [depth] "
                 "[block_size] [read_ratio]"
              << std::endl;
    return -EINVAL;
  }

  netaddr raddr = {};
  int threads = 0, time = 0, depth = 0, block_size = 0, read_ratio = 0;
  int ret = StringToAddr(argv[2], &raddr.ip);
  if (ret) return -EINVAL;
  raddr.port = kNetperfPort;
  threads = std::stoi(argv[3], nullptr, 0);
  time = std::stoi(argv[4], nullptr, 0);
  depth = std::stoi(argv[5], nullptr, 0);
  block_size = std::stoi(argv[6], nullptr, 0);
  read_ratio = std::stoi(argv[7], nullptr, 0);

  return rt::RuntimeInit(argv[1], [=]() {
    RunClient(raddr, threads, time, depth, block_size, read_ratio);
  });
}
