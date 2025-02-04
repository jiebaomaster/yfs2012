// lock client interface.

#ifndef lock_client_cache_rsm_h

#define lock_client_cache_rsm_h

#include <string>
#include "lock_protocol.h"
#include "rpc.h"
#include "fifo.h"
#include "lock_client.h"
#include "lang/verify.h"

#include "rsm_client.h"

// Classes that inherit lock_release_user can override dorelease so that 
// that they will be called when lock_client releases a lock.
// You will not need to do anything with this class until Lab 5.
class lock_release_user {
 public:
  virtual void dorelease(lock_protocol::lockid_t) = 0;
  virtual ~lock_release_user() {};
};


class lock_client_cache_rsm;

// Clients that caches locks.  The server can revoke locks using 
// lock_revoke_server.
class lock_client_cache_rsm : public lock_client {
  enum lock_state { NONE, FREE, LOCKED, ACQUIRING, RELEASING };
  struct lock { // 锁
    lock_protocol::lockid_t lid;
    bool has_revoked;  // 是否收到锁释放请求，若已经收到，则在下一次释放锁的时候需要请求锁服务释放锁
    bool retry; // 是否收到 重试 请求
    lock_state state;
    /**
     * 请求服务的编号
     * acquire 采用递增编号
     * release 采用相应的 acquire 编号
     */
    lock_protocol::xid_t xid;
    pthread_cond_t wait_queue;  // 客户端已有其他线程占有锁，等待其他线程释放锁
    pthread_cond_t
        release_queue;  // 客户端正在释放锁时，有其他线程获取锁，等待释放后重新请求锁
    pthread_cond_t retry_queue;  // 服务端的锁已被其他客户端占用，等待重新请求锁
    lock(lock_protocol::lockid_t lid) : lid(lid), has_revoked(false), retry(false), state(NONE) {
      pthread_cond_init(&wait_queue, NULL);
      pthread_cond_init(&release_queue, NULL);
      pthread_cond_init(&retry_queue, NULL);
    }
  };

 private:
  friend class rsm_client;
  rsm_client *rsmc; // 用于代替 rpc 客户端，发送请求
  class lock_release_user *lu; // 辅助类，执行文件缓存的刷新
  int rlock_port;
  std::string hostname;
  std::string id;
  lock_protocol::xid_t xid; // 请求服务的编号
  std::map<lock_protocol::lockid_t, lock> lockid_lock; // 记录客户端持有的所有锁
  pthread_mutex_t map_mutex;
  fifo<std::map<lock_protocol::lockid_t, lock>::iterator> releasing_lock; // 正在释放的锁队列
  lock_protocol::xid_t nextXid() {
    // 必须持有锁
    return xid++;
  }
 public:
  static int last_port;
  lock_client_cache_rsm(std::string xdst, class lock_release_user *l = 0);
  virtual ~lock_client_cache_rsm() {};
  lock_protocol::status acquire(lock_protocol::lockid_t);
  virtual lock_protocol::status release(lock_protocol::lockid_t);
  void releaser();
  rlock_protocol::status revoke_handler(lock_protocol::lockid_t, 
				        lock_protocol::xid_t, int &);
  rlock_protocol::status retry_handler(lock_protocol::lockid_t, 
				       lock_protocol::xid_t, int &);
};


#endif
