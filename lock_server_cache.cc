// the caching lock server implementation

#include "lock_server_cache.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"

using std::string;

lock_server_cache::lock_server_cache()
{
  pthread_mutex_init(&map_mutex, NULL);
}

/**
 * @brief 获取 id 为 lid 的锁
 * 
 * @param lid 请求的锁的 id
 * @param id 客户端标识 host:port
 * @return int 
 */
lock_protocol::status lock_server_cache::acquire(lock_protocol::lockid_t lid,
                                                 std::string id, int &) {
  bool need_revoke = false; // 是否需要向客户端发送撤销锁请求
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&map_mutex);

  auto iter = lockid_lock.find(lid);
  if (iter == lockid_lock.end()) // 请求的锁不存在就新建一个
    iter = lockid_lock.insert({lid, lock()}).first;

  lock &lock = iter->second;
  switch (lock.state) {
    case FREE:
      lock.state = LOCKED;
      lock.owner = id;
      break;

    case LOCKED:
      lock.state = LOCKED_AND_WAIT;
      lock.waiters.insert(id);
      need_revoke = true;  // 有别的客户端在等待时，占用锁的客户端需要尽快释放
      ret = lock_protocol::RETRY;  // 告诉客户端需要重新请求锁
      break;

    case LOCKED_AND_WAIT:
      lock.waiters.insert(id);
      ret = lock_protocol::RETRY;
      break;

    case RETRYING:  // 锁服务器向等待该锁的某个客户端(记为 C)发送 retry 请求后
      if (lock.waiters.count(id)) {
        // 如果等待列表中含有 id，则此时发送请求的 id 就是客户端 C
        lock.waiters.erase(id);
        lock.owner = id;
        if (lock.waiters.size()) {  // 还有其他客户端在等待
          lock.state = lock_state::LOCKED_AND_WAIT;
          need_revoke = true;
        } else
          lock.state = lock_state::LOCKED;
      } else { // 否则加入等待队列，FIFO，C 肯定早于当前客户端
        lock.waiters.insert(id);
        ret = lock_protocol::RETRY;
      }
      break;

    default:
      break;
  }

  pthread_mutex_unlock(&map_mutex);
  // rpc 放在锁之外，防止分布式死锁
  if(need_revoke) { // 向客户端发送撤销锁请求
    int r;
    handle(lock.owner).safebind()->call(rlock_protocol::revoke, lid, r);
  }

  return ret;
}

/**
 * @brief 释放 id 为 lid 的锁
 * 
 * @param lid 释放的锁的 id
 * @param id 客户端标识 host:port
 * @return lock_protocol::status 
 */
lock_protocol::status lock_server_cache::release(lock_protocol::lockid_t lid,
                                                 std::string id, int &) {
  bool need_retry = false;   // 是否需要向客户端发送重试请求
  string client_need_retry;  // 需要发送重试请求的客户端
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
      lock.owner = "";
      need_retry = true;
      client_need_retry = *lock.waiters.begin();  // FIFO
      break;

    default:
      break;
  }

  pthread_mutex_unlock(&map_mutex);
  if (need_retry) {
    int r;
    handle(client_need_retry).safebind()->call(rlock_protocol::retry, lid, r);
  }

  return ret;
}

lock_protocol::status
lock_server_cache::stat(lock_protocol::lockid_t lid, int &r)
{
  tprintf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}

