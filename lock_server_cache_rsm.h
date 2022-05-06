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
    std::set<std::string> waiters; // 等待该锁的客户端
    bool revoked; // 是否已向持有锁的客户端发送 revoke
    lock_state state; // 锁的状态

    /* 处理 master 崩溃时，主节点不能确定是否已经处理最后一次请求的问题 */
    lock_protocol::xid_t xid; // 最后一次请求的序号
    lock_protocol::status lastRet; // 最后一次处理结果
    std::string lastRequirer; // 最后的需求者
    bool needRevoke; // 最后一次处理是否触发 revoke

    lock(lock_protocol::lockid_t lid): lid(lid), revoked(false), state(FREE), xid(-1) {}
    
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

  /* 序列化和反序列化锁服务器的状态 */
  std::string marshal_state() override;
  void unmarshal_state(std::string state) override;

  /* 锁服务的 RPC 请求处理函数 */
  int acquire(lock_protocol::lockid_t, std::string id, 
	      lock_protocol::xid_t, int &);
  int release(lock_protocol::lockid_t, std::string id, lock_protocol::xid_t,
	      int &);
};

#endif
