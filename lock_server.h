// this is the lock server
// the lock client has a similar interface

#ifndef lock_server_h
#define lock_server_h

#include <string>
#include <map>
#include "lock_protocol.h"
#include "lock_client.h"
#include "rpc.h"

class lock_server {
 public:
  struct lock { // 锁
    enum lock_status { FREE, LOCKED };

    lock_protocol::lockid_t lid;  // 锁 id
    int status;                   // 锁的状态
    // 条件变量，当锁是 LOCKED 时，其他要获取该锁的线程必须等待；
    // 当锁的状态变为 FREE 时，需要唤醒等待该锁的进程
    pthread_cond_t lcond;

    lock(lock_protocol::lockid_t);
    lock(lock_protocol::lockid_t, int);
    ~lock(){};
  };

 protected:
  int nacquire;
  std::map<lock_protocol::lockid_t, lock*> lockid_lock; // 锁id=》锁
  pthread_mutex_t map_mutex; // 互斥量保护多线程访问 map

 public:
  lock_server();
  ~lock_server() {};
  lock_protocol::status stat(int clt, lock_protocol::lockid_t lid, int &);
  lock_protocol::status acquire(int clt, lock_protocol::lockid_t lid, int &);
  lock_protocol::status release(int clt, lock_protocol::lockid_t lid, int &);
};

#endif 







