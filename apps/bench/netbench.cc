extern "C" {
#include <base/log.h>
#include <net/ip.h>
}
#undef min
#undef max

#include "runtime.h"
#include "thread.h"
#include "sync.h"
#include "timer.h"
#include "net.h"
#include "fake_worker.h"
#include "proto.h"

#include <iostream>
#include <iomanip>
#include <utility>
#include <memory>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>

namespace {

using sec = std::chrono::duration<double, std::micro>;

const int kRxBufSize = 262144;
const double kBandwidth = 100e9;

// the number of worker threads to spawn.
int tx_threads;
// int rx_threads;

netaddr peer_addr[16];

int num_peers;

// duration of experiment
int duration;

// load
double load;

int flow_size;

// self index in peer list
int my_idx;

void ServerWorker(std::unique_ptr<rt::TcpConn> c) {
  std::cout << "server conn worker started\n";
  char *buf = (char *) malloc(kRxBufSize);

  while (true) {
    // Read data
    ssize_t ret = c->Read(buf, kRxBufSize);
    if (ret <= 0 || ret > kRxBufSize) {
      if (ret == 0 || ret == -ECONNRESET) break;
      panic("read failed, ret = %ld", ret);
    }
  }

  free(buf);
}

void ServerHandler() {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kNetbenchPort},
				  4096));
  if (q == nullptr) panic("couldn't listen for connections");

  std::cout << "server thread running\n";

  while (true) {
    rt::TcpConn *c = q->Accept();
    std::cout << "accepted conn\n";
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=]{ServerWorker(std::unique_ptr<rt::TcpConn>(c));}).Detach();
  }
}

void PoissonWorker(int my_idx, int num_nodes, int flow_size, int duration, double load)
{
  std::cout << "worker thread started\n";

  char *buf = (char *) malloc(kRxBufSize);

  // Seed the random generator.
  std::mt19937 g(microtime());

  std::vector<std::unique_ptr<rt::TcpConn>> conns;

  int npeers = 0;
  for(int i = 0; i < num_nodes; i++) {
    if(i == my_idx) {
      continue;
    }
    std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, peer_addr[i]));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
    std::cout << "connection created\n";
    conns.emplace_back(std::move(outc));
    npeers += 1;
  }

  std::cout << "Connected to all peers\n";

  std::uniform_int_distribution<> uniform_distr(0, npeers-1);
  

  // Create a packet transmit schedule.
  std::vector<double> sched;
  // TODO: Check MSS and MTU size if using Jumbo frames
  std::exponential_distribution<double> rd(kBandwidth * load / (flow_size * 8.0 / 1460 * 1500));
  std::vector<double> tmp;
  double total_time = 0;
  int64_t bytes_sent = 0;

  while(total_time < duration) {
    double ia_time = rd(g);
    tmp.push_back(ia_time);
    total_time += ia_time;
  }

  sched.push_back(tmp[0]);
  for (std::vector<double>::size_type j = 1; j < tmp.size(); ++j) {
    tmp[j] += tmp[j - 1];
    sched.push_back(static_cast<uint64_t>(tmp[j]*1e6));
  }

  int sched_idx = 0;


  barrier();
  uint64_t expstart = microtime();
  barrier();

  while(sched_idx < sched.size()) {
    barrier();
    uint64_t now = microtime();
    barrier();

    if(now - expstart >= duration*1e6) {
      break;
    }

    if(now - expstart >= sched[sched_idx]) {
      // pick random peer
      int peer_idx = uniform_distr(g);
      // send flow
      ssize_t ret = conns[peer_idx]->WriteFull(buf, flow_size);
      if (ret != flow_size)
        panic("write failed, ret = %ld", ret);
      bytes_sent += flow_size;
      sched_idx += 1;
    } else {
      rt::Sleep(sched[sched_idx] - (microtime() - expstart));
    }
    
  }

  std::cout << "Total bytes sent: " << bytes_sent << "\n";

  free(buf);

}


// int StringToAddr(const char *str, uint32_t *addr) {
//   uint8_t a, b, c, d;

//   if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4)
//     return -EINVAL;

//   *addr = MAKE_IP_ADDR(a, b, c, d);
//   return 0;
// }

void MainHandler(void *arg) {
  
  // Start server thread
  rt::Thread([=]{ServerHandler();}).Detach();

  std::cout << "Launched server thread\n";

  rt::Sleep(10*1e6);

  // Start TX threads
  std::vector<rt::Thread> ths;
  for(int i = 0; i < tx_threads; i++) {
    ths.emplace_back(rt::Thread([=]{(PoissonWorker(my_idx, num_peers, flow_size, duration, load));}));
  }

  for (auto &t : ths) t.Join();

}

} // anonymous namespace

int main(int argc, char *argv[]) {
  
  str_to_netaddr("192.168.11.116:8001", &peer_addr[0]);
  str_to_netaddr("192.168.11.117:8001", &peer_addr[1]);

  int ret;

  if (argc < 8) {
    std::cerr << "usage: [cfg_file] [num_peers] [my_idx] [flow-size] [tx-threads] [duration] [load]" << std::endl;
    return -EINVAL;
  }

  num_peers = std::stoi(argv[2], nullptr, 0);
  my_idx = std::stoi(argv[3], nullptr, 0);
  flow_size = std::stoi(argv[4], nullptr, 0);
  tx_threads = std::stoi(argv[5], nullptr, 0);
  duration = std::stoi(argv[6], nullptr, 0);
  load = std::stod(argv[7], nullptr);

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;

}
