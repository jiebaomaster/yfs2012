#ifndef lock_server_cache_rsm_h
#define lock_server_cache_rsm_h

#include <string>
#include <map>
#include <unordered_map>
#include <set>

#include "lock_protocol.h"
#include "rpc.h"
#include "rsm_state_transfer.h"
#include "rsm.h"

class lock_server_cache_rsm : public rsm_state_transfer {
  enum lock_state {FREE, LOCKED, LOCKED_AND_WAIT, RETRYING};
  struct last_request {
    lock_protocol::xid_t xid; // 最后一次请求的序号
    lock_protocol::status lastRet; // 最后一次处理结果
    /**
     * 最后一次处理是否触发 revoke
     * master 奔溃时可能会丢失 revoke，新的 master 遇到重复请求时应该触发 revoke
     */
    bool needRevoke;
  };
  struct lock { // 锁
    lock_protocol::lockid_t lid;
    std::string owner; // 锁当前被哪个客户端占用，host:port
    std::set<std::string> waiters; // 等待该锁的客户端
    bool revoked; // 是否已向持有锁的客户端发送 revoke
    lock_state state; // 锁的状态

    /**
     * per client per lock last request
     * 处理节点发生崩溃时，遇到的重复请求问题
     */
    std::unordered_map<std::string, last_request> requests;

    lock(lock_protocol::lockid_t lid): lid(lid), revoked(false), state(FREE) {}
    
    void dumpLock() { // 打印锁的状态
      printf("dumpLock=> [lockid %llu] owner %s,\ndumpLock=> waiters {", lid,
             owner.c_str());
      for (auto &s : waiters) {
        printf("%s, ", s.c_str());
      }
      printf("}\ndumpLock=> requests: {\n");
      for (auto &r : requests) {
        printf("dumpLock=> [%s => %llu %d %d]\n", r.first.c_str(), r.second.xid,
               r.second.lastRet, r.second.needRevoke);
      }
      printf("dumpLock=> end <=\n");
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
