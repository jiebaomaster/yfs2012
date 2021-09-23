#ifndef lock_server_cache_h
#define lock_server_cache_h

#include <string>

#include <map>
#include <set>
#include "lock_protocol.h"
#include "rpc.h"
#include "lock_server.h"


class lock_server_cache {
  enum lock_state {FREE, LOCKED, LOCKED_AND_WAIT, RETRYING};
  struct lock { // 锁
    std::string owner; // 锁当前被哪个客户端占用，host:port
    std::set<std::string> waiters; // 等待该锁的客户端
    bool revoked;
    lock_state state; // 锁的状态
    lock(): revoked(false), state(FREE) {}
  };
 private:
  int nacquire;
  std::map<lock_protocol::lockid_t, lock> lockid_lock; // 锁id => 锁，保存系统中所有锁的状态
  pthread_mutex_t map_mutex; // 互斥量保护多线程访问 map

 public:
  lock_server_cache();
  lock_protocol::status stat(lock_protocol::lockid_t, int &);
  lock_protocol::status acquire(lock_protocol::lockid_t, std::string id, int &);
  lock_protocol::status release(lock_protocol::lockid_t, std::string id, int &);
};

#endif
