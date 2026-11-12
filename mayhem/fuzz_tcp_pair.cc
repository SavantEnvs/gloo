// Drives gloo's TCP Pair read path with attacker-controlled wire bytes.
//
// A raw loopback socket plays the remote peer: it completes the connection handshake (the
// initiator writes the pair's sequence number to the device's listener) and then sends the fuzz
// input, which the Pair parses in sync mode. Byte 0 selects whether an unbound
// receive is posted; the rest is the raw stream (6-word preamble followed by payload).
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

#include "gloo/transport/tcp/address.h"
#include "gloo/transport/tcp/context.h"
#include "gloo/transport/tcp/device.h"
#include "gloo/transport/tcp/pair.h"

namespace tcp = gloo::transport::tcp;

namespace {

// Shared across inputs on purpose: the device only owns the listening socket and its epoll
// thread; every per-input object (context, pair, buffers, sockets) is created and destroyed
// inside LLVMFuzzerTestOneInput.
std::shared_ptr<tcp::Device> getDevice() {
  static std::shared_ptr<tcp::Device> device = [] {
    tcp::attr attr;
    attr.hostname = "127.0.0.1";
    attr.ai_family = AF_INET;
    return std::static_pointer_cast<tcp::Device>(tcp::CreateDevice(attr));
  }();
  return device;
}

struct Fd {
  int fd;
  ~Fd() {
    if (fd >= 0) {
      close(fd);
    }
  }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  const bool postUnbound = data[0] & 1;
  data++;
  size--;

  auto device = getDevice();
  auto ctx = std::make_shared<tcp::Context>(device, 0, 2);
  // The transport context leaves its timeout uninitialized; callers (gloo's rendezvous, the tests)
  // always set it. Everything below ends on data or EOF, so the value is never actually waited out.
  ctx->setTimeout(std::chrono::seconds(30));
  auto& slot = ctx->createPair(1);
  auto* pair = static_cast<tcp::Pair*>(slot.get());

  const tcp::Address& local = pair->address();
  const auto ss = local.getSockaddr();
  const gloo::transport::tcp::sequence_number_t seq = local.getSeq();

  Fd peer{socket(ss.ss_family, SOCK_STREAM, 0)};
  if (peer.fd < 0 ||
      connect(peer.fd, reinterpret_cast<const sockaddr*>(&ss), sizeof(sockaddr_in)) != 0) {
    return 0;
  }
  if (write(peer.fd, &seq, sizeof(seq)) != sizeof(seq)) {
    return 0;
  }

  // The remote address has the local pair's sockaddr and a larger sequence number, which makes
  // the local pair the passive side: it adopts the socket the raw peer just opened.
  tcp::Address remote(ss, seq + 1);
  pair->connect(remote.bytes());

  char buf0[64] = {};
  char buf1[16] = {};
  char ubuf[64] = {};
  auto recv0 = pair->createRecvBuffer(0, buf0, sizeof(buf0));
  auto recv1 = pair->createRecvBuffer(1, buf1, sizeof(buf1));
  std::unique_ptr<gloo::transport::UnboundBuffer> unbound;
  if (postUnbound) {
    unbound = ctx->createUnboundBuffer(ubuf, sizeof(ubuf));
    unbound->recv(1, 7, 0, sizeof(ubuf));
  }
  // Unbound-buffer operations are async-only; setSync() flushes the queued notification and
  // switches the socket to blocking reads for the rest of the input.
  pair->setSync(true, false);

  if (size > 0) {
    (void)send(peer.fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
  }
  shutdown(peer.fd, SHUT_WR);

  // In sync mode Buffer::waitRecv() reads from the pair until buffer 0 completes (handling any
  // other operation it meets on the way). Every read consumes at least one 48-byte preamble and
  // the closed stream ends the loop with an exception, so this terminates for any input.
  try {
    for (;;) {
      recv0->waitRecv();
    }
  } catch (const std::exception&) {
  }
  return 0;
}
