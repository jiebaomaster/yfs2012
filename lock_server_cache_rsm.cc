// the caching lock server implementation

#include "lock_server_cache_rsm.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"

using std::string;
using std::map;

static void *
revokethread(void *x)
{
  lock_server_cache_rsm *sc = (lock_server_cache_rsm *) x;
  sc->revoker();
  return 0;
}

static void *
retrythread(void *x)
{
  lock_server_cache_rsm *sc = (lock_server_cache_rsm *) x;
  sc->retryer();
  return 0;
}

lock_server_cache_rsm::lock_server_cache_rsm(class rsm *_rsm) 
  : rsm (_rsm), revoking_locks(50), retring_locks(50)
{
  pthread_t th;
  int r = pthread_create(&th, NULL, &revokethread, (void *) this);
  VERIFY (r == 0);
  r = pthread_create(&th, NULL, &retrythread, (void *) this);
  VERIFY (r == 0);
  pthread_mutex_init(&map_mutex, NULL);
}

void
lock_server_cache_rsm::revoker()
{

  // This method should be a continuous loop, that sends revoke
  // messages to lock holders whenever another client wants the
  // same lock
  int r;
  map<lock_protocol::lockid_t, lock>::iterator iter;
  while(1) {
    revoking_locks.deq(&iter);

    pthread_mutex_lock(&map_mutex);
    auto &lock = iter->second;
    auto h = handle(lock.owner);
    pthread_mutex_unlock(&map_mutex);

    auto cl = h.safebind();
    cl->call(rlock_protocol::revoke, lock.lid, lock.xid, r);
  }
}


void
lock_server_cache_rsm::retryer()
{

  // This method should be a continuous loop, waiting for locks
  // to be released and then sending retry messages to those who
  // are waiting for it.
  int r;
  map<lock_protocol::lockid_t, lock>::iterator iter;
  while (1) {
    retring_locks.deq(&iter);

    pthread_mutex_lock(&map_mutex);
    auto &lock = iter->second;
    lock.owner = "";
    string client_need_retry = *lock.waiters.begin();  // 需要发送重试请求的客户端
    auto h = handle(client_need_retry);
    pthread_mutex_unlock(&map_mutex);

    auto cl = h.safebind();
    cl->call(rlock_protocol::retry, lock.lid, lock.xid, r);
  }
}


int lock_server_cache_rsm::acquire(lock_protocol::lockid_t lid, std::string id, 
             lock_protocol::xid_t xid, int &)
{
  lock_protocol::status ret = lock_protocol::OK;

  pthread_mutex_lock(&map_mutex);

  auto iter = lockid_lock.find(lid);
  if (iter == lockid_lock.end()) // 请求的锁不存在就新建一个
    iter = lockid_lock.insert({lid, lock(lid)}).first;

  lock &lock = iter->second;
  switch (lock.state) {
    case FREE:
      lock.state = LOCKED;
      lock.owner = id;
      break;

    case LOCKED:
      lock.state = LOCKED_AND_WAIT;
      lock.waiters.insert(id);
      revoking_locks.enq(iter); // 有别的客户端在等待时，占用锁的客户端需要尽快释放
      ret = lock_protocol::RETRY;  // 告诉客户端需要重新请求锁
      break;

    case LOCKED_AND_WAIT:
      lock.waiters.insert(id);
      ret = lock_protocol::RETRY;
      break;

    case RETRYING:  // 锁服务器向等待该锁的某个客户端(记为 C)发送 retry 请求后
      if (lock.waiters.count(id)) {
        // 如果等待列表中含有 id，则此时发送请求的 id 就是客户端 C
        // 将锁分配给客户端 C
        lock.waiters.erase(id);
        lock.owner = id;
        if (lock.waiters.size()) {
          // 还有其他客户端在等待，分配给 C 之后通知 C 这个锁需要立即释放
          lock.state = lock_state::LOCKED_AND_WAIT;
          revoking_locks.enq(iter);
        } else {
          lock.state = lock_state::LOCKED;
        }
      } else { // 否则加入等待队列，FIFO，C 肯定早于当前客户端
        lock.waiters.insert(id);
        ret = lock_protocol::RETRY;
      }
      break;

    default:
      break;
  }

  pthread_mutex_unlock(&map_mutex);

  return ret;
}

int 
lock_server_cache_rsm::release(lock_protocol::lockid_t lid, std::string id, 
         lock_protocol::xid_t xid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;

  pthread_mutex_lock(&map_mutex);

  auto iter = lockid_lock.find(lid);
  if (iter == lockid_lock.end()) {
    pthread_mutex_unlock(&map_mutex);
    printf("ERROR: can't find lock with lockid = %llu\n", lid);
    return lock_protocol::NOENT;
  }

  lock &lock = iter->second;
  switch (lock.state) {
    case FREE:
    case RETRYING:
      printf("ERROR: can't releaer a lock with state RETRYING/FREE\n");
      ret = lock_protocol::IOERR;
      break;

    case LOCKED:  // 释放一个没有人等待的锁
      lock.state = lock_state::FREE;
      lock.owner = "";
      break;

    case LOCKED_AND_WAIT:  // 释放一个有人等待的锁，挑选客户端发送重试请求
      lock.state = lock_state::RETRYING;
      retring_locks.enq(iter);
      break;

    default:
      break;
  }

  pthread_mutex_unlock(&map_mutex);

  return ret;
}

std::string
lock_server_cache_rsm::marshal_state()
{
  std::ostringstream ost;
  std::string r;
  return r;
}

void
lock_server_cache_rsm::unmarshal_state(std::string state)
{
}

lock_protocol::status
lock_server_cache_rsm::stat(lock_protocol::lockid_t lid, int &r)
{
  printf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}

