#ifndef lock_server_cache_rsm_h
#define lock_server_cache_rsm_h

#include <string>
#include <map>
#include <set>

#include "lock_protocol.h"
#include "rpc.h"
#include "rsm_state_transfer.h"
#include "rsm.h"

class lock_server_cache_rsm : public rsm_state_transfer {
  enum lock_state {FREE, LOCKED, LOCKED_AND_WAIT, RETRYING};
  struct lock { // 锁
    lock_protocol::lockid_t lid;
    std::string owner; // 锁当前被哪个客户端占用，host:port
    lock_protocol::xid_t xid; 
    std::set<std::string> waiters; // 等待该锁的客户端
    bool revoked; // 是否已向持有锁的客户端发送 revoke
    lock_state state; // 锁的状态
    lock(lock_protocol::lockid_t lid): lid(lid), revoked(false), state(FREE) {}
    
    void dumpLock() { // 打印锁的状态
      printf("[lockid %llu, xid %llu] owner %s, waiters {", lid, xid, owner.c_str());
      for(auto &s : waiters) {
        printf("%s, ", s.c_str());
      }
      printf("}\n");
    }
  };
 private:
  int nacquire;
  std::map<lock_protocol::lockid_t, lock> lockid_lock; // 锁id => 锁，保存系统中所有锁的状态
  pthread_mutex_t map_mutex; // 互斥量保护多线程访问 map
  class rsm *rsm;
  fifo<std::map<lock_protocol::lockid_t, lock>::iterator> revoking_locks;
  fifo<std::map<lock_protocol::lockid_t, lock>::iterator> retring_locks;
 public:
  lock_server_cache_rsm(class rsm *rsm = 0);
  lock_protocol::status stat(lock_protocol::lockid_t, int &);
  void revoker();
  void retryer();
  std::string marshal_state();
  void unmarshal_state(std::string state);
  int acquire(lock_protocol::lockid_t, std::string id, 
	      lock_protocol::xid_t, int &);
  int release(lock_protocol::lockid_t, std::string id, lock_protocol::xid_t,
	      int &);
};

#endif
