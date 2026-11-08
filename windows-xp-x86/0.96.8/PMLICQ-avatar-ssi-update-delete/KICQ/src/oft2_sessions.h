#pragma once
// Thread-safe, bounded OFT2 rendezvous session registry.
// No network or Miranda UI side effects: the caller must drive each transition.
#include <array>
#include <map>
#include <mutex>
#include <stdint.h>
#include <string>

namespace oft2 {
enum class Phase { Offered, Accepted, Connecting, Transferring, Completed, Rejected, Cancelled, Failed };
struct SessionKey {
 uint32_t uin = 0;
 std::array<unsigned char, 8> cookie{};
 bool operator<(const SessionKey &b) const {
  if (uin != b.uin) return uin < b.uin;
  return cookie < b.cookie;
 }
};
struct Session {
 SessionKey key;
 Phase phase = Phase::Offered;
 bool incoming = true;
 uint64_t total = 0;
 uint64_t transferred = 0;
 std::string filename;
};
class Sessions {
 std::mutex lock_;
 std::map<SessionKey, Session> sessions_;
 static bool allowed(Phase a, Phase b) {
  if (b == Phase::Cancelled || b == Phase::Failed) return a != Phase::Completed && a != Phase::Rejected && a != Phase::Cancelled && a != Phase::Failed;
  switch (a) {
  case Phase::Offered: return b == Phase::Accepted || b == Phase::Rejected;
  case Phase::Accepted: return b == Phase::Connecting;
  case Phase::Connecting: return b == Phase::Transferring;
  case Phase::Transferring: return b == Phase::Completed;
  default: return false;
  }
 }
public:
 bool add(const Session &s) {
  if (!s.key.uin || s.filename.empty() || s.filename.size() > 255) return false;
  std::lock_guard<std::mutex> guard(lock_);
  if (sessions_.size() >= 128) return false;
  return sessions_.emplace(s.key, s).second;
 }
 bool get(const SessionKey &k, Session &out) {
  std::lock_guard<std::mutex> guard(lock_);
  auto it = sessions_.find(k);
  if (it == sessions_.end()) return false;
  out = it->second; return true;
 }
 bool transition(const SessionKey &k, Phase next) {
  std::lock_guard<std::mutex> guard(lock_);
  auto it = sessions_.find(k);
  if (it == sessions_.end() || !allowed(it->second.phase, next)) return false;
  if (next == Phase::Completed && it->second.transferred != it->second.total) return false;
  it->second.phase = next; return true;
 }
 bool progress(const SessionKey &k, uint64_t bytes) {
  std::lock_guard<std::mutex> guard(lock_);
  auto it = sessions_.find(k);
  if (it == sessions_.end() || it->second.phase != Phase::Transferring || bytes < it->second.transferred || bytes > it->second.total) return false;
  it->second.transferred = bytes; return true;
 }
 bool erase(const SessionKey &k) {
  std::lock_guard<std::mutex> guard(lock_);
  return sessions_.erase(k) != 0;
 }
 void clear() { std::lock_guard<std::mutex> guard(lock_); sessions_.clear(); }
};
}
