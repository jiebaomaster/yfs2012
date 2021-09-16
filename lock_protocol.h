// lock protocol

#ifndef lock_protocol_h
#define lock_protocol_h

#include "rpc.h"

class lock_protocol {
 public:
  enum xxstatus { OK, RETRY, RPCERR, NOENT, IOERR };
  typedef int status;
  typedef unsigned long long lockid_t;
  enum rpc_numbers {
    acquire = 0x7001,
    release,
    stat
  };
};

// 锁
class lock {
 public:
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
#endif 
