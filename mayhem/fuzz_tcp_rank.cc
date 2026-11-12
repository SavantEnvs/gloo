// Round-trips the rendezvous records the TCP transport exchanges through the store (Rank and
// Address) and parses fuzzed IP strings. Rank(bytes) is only specified for bytes produced by
// Rank::bytes(), so inputs are turned into Rank fields first rather than fed in as raw bytes.
#include <fuzzer/FuzzedDataProvider.h>

#include <sys/socket.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include "gloo/transport/tcp/address.h"
#include "gloo/transport/tcp/context.h"

namespace tcp = gloo::transport::tcp;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider fdp(data, size);

  const std::string hostname = fdp.ConsumeRandomLengthString(64);
  // A Rank always carries a serialized Address, which is exactly this many bytes.
  const size_t kAddressSize = sizeof(sockaddr_storage) + sizeof(ssize_t);
  std::vector<char> addrBytes = fdp.ConsumeBytes<char>(kAddressSize);
  addrBytes.resize(kAddressSize);
  const std::vector<char> rawAddr =
      fdp.ConsumeBytes<char>(fdp.ConsumeIntegralInRange<size_t>(0, 160));
  const size_t numIds = fdp.ConsumeIntegralInRange<size_t>(0, 8);
  std::vector<ssize_t> ids;
  for (size_t i = 0; i < numIds; i++) {
    ids.push_back(fdp.ConsumeIntegral<ssize_t>());
  }
  const std::string ip = fdp.ConsumeRandomLengthString(48);
  const uint16_t port = fdp.ConsumeIntegral<uint16_t>();

  const tcp::Rank rank(hostname, addrBytes, ids);
  const tcp::Rank back(rank.bytes());
  if (back.hostname != hostname || back.addressBytes != addrBytes ||
      back.pairIdentifiers != ids) {
    __builtin_trap();
  }

  // Address(bytes) rejects any size but one serialized Address by throwing.
  try {
    const tcp::Address addr(rawAddr);
    (void)addr.str();
    if (addr.bytes() != rawAddr) {
      __builtin_trap();
    }
  } catch (const std::exception&) {
  }

  try {
    const tcp::Address addr(ip, port, fdp.ConsumeIntegral<ssize_t>());
    (void)addr.str();
    const tcp::Address copy(addr.bytes());
    if (copy.getSeq() != addr.getSeq()) {
      __builtin_trap();
    }
  } catch (const std::exception&) {
  }
  return 0;
}
