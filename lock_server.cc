// the lock server implementation

#include "lock_server.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>

#include <sstream>

lock_server::lock::lock(lock_protocol::lockid_t lid) : lid(lid), status(lock::FREE) {
  pthread_cond_init(&lcond, NULL);
}

lock_server::lock::lock(lock_protocol::lockid_t lid, int stat) : lid(lid), status(stat) {
  pthread_cond_init(&lcond, NULL);
}

lock_server::lock_server() : nacquire(0) {
  pthread_mutex_init(&map_mutex, NULL);
}

/**
 * @brief 查询 lid 的状态
 *
 * @param clt rpc 客户端的 id
 * @param lid 查询的锁的 id
 * @param r
 * @return lock_protocol::status
 */
lock_protocol::status lock_server::stat(int clt, lock_protocol::lockid_t lid,
                                        int &r) {
  lock_protocol::status ret = lock_protocol::OK;
  printf("stat request from clt %d\n", clt);
  r = nacquire;
  return ret;
}

/**
 * @brief 获取 id 为 lid
 * 的锁。锁服务器是开放式的，如果请求的锁不存在, 就创建一个新锁加入到lockmap中,
 * 并确保新创建的锁被当前请求锁的客户端获取
 *
 * @param clt rpc 客户端的 id
 * @param lid 请求的锁的 id
 * @param r
 * @return lock_protocol::status
 */
lock_protocol::status lock_server::acquire(int clt, lock_protocol::lockid_t lid,
                                           int &r) {
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&map_mutex);
  auto it = lockid_lock.find(lid);

  if (it == lockid_lock.end()) {  // 请求的锁不存在，则分配一个新的锁
    lock *new_lock = new lock(lid, lock::LOCKED);  // 创建一个新锁
    lockid_lock.insert(std::make_pair(lid, new_lock));  // 插入新锁的映射关系
    pthread_mutex_unlock(&map_mutex);
    return ret;
  } else {
    // 如果当前锁不空闲，则阻塞在当前锁的条件变量上
    while (it->second->status != lock::FREE) {
      // 开始阻塞时释放 mutex，直到唤醒时重新获取 mutex，确保当前线程阻塞时
      // mutex 能被唤醒线程获取
      pthread_cond_wait(&(it->second->lcond), &map_mutex);
    }

    it->second->status = lock::LOCKED;  // 获取锁
    pthread_mutex_unlock(&map_mutex);
    return ret;
  }
}

/**
 * @brief 释放 id 为 lid 的锁
 *
 * @param clt rpc 客户端的 id
 * @param lid 释放的锁的 id
 * @param r
 * @return lock_protocol::status
 */
lock_protocol::status lock_server::release(int clt, lock_protocol::lockid_t lid,
                                           int &r) {
  lock_protocol::status ret = lock_protocol::OK;

  pthread_mutex_lock(&map_mutex);
  auto it = lockid_lock.find(lid);
  if (it == lockid_lock.end()) {  // 释放的锁不存在
    ret = lock_protocol::IOERR;   // 报错，不能释放一个不存在的锁
    pthread_mutex_unlock(&map_mutex);
    return ret;
  } else {
    it->second->status = lock::FREE;  // 释放锁
    pthread_cond_signal(
        &(it->second->lcond));  // 唤醒阻塞在当前条件变量上的线程
    pthread_mutex_unlock(&map_mutex);
    return ret;
  }
}
