// RPC stubs for clients to talk to lock_server, and cache the locks
// see lock_client.cache.h for protocol details.

#include "lock_client_cache_rsm.h"
#include "rpc.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include "tprintf.h"

#include "rsm_client.h"

using std::map;

static void *
releasethread(void *x)
{
  lock_client_cache_rsm *cc = (lock_client_cache_rsm *) x;
  cc->releaser();
  return 0;
}

int lock_client_cache_rsm::last_port = 0;

lock_client_cache_rsm::lock_client_cache_rsm(std::string xdst, 
				     class lock_release_user *_lu)
  : lock_client(xdst), lu(_lu), releasing_lock(50)
{
  srand(time(NULL)^last_port);
  rlock_port = ((rand()%32000) | (0x1 << 10));
  const char *hname;
  // VERIFY(gethostname(hname, 100) == 0);
  hname = "127.0.0.1";
  std::ostringstream host;
  host << hname << ":" << rlock_port;
  id = host.str();
  last_port = rlock_port;
  rpcs *rlsrpc = new rpcs(rlock_port);
  rlsrpc->reg(rlock_protocol::revoke, this, &lock_client_cache_rsm::revoke_handler);
  rlsrpc->reg(rlock_protocol::retry, this, &lock_client_cache_rsm::retry_handler);
  xid = 0;
  // You fill this in Step Two, Lab 7
  // - Create rsmc, and use the object to do RPC 
  //   calls instead of the rpcc object of lock_client
  // rpc 代理，通过 rsmc 发送的请求会被 rsm master 接收
  rsmc = new rsm_client(xdst);
  pthread_mutex_init(&map_mutex, NULL);
  pthread_t th;
  int r = pthread_create(&th, NULL, &releasethread, (void *) this);
  VERIFY (r == 0);
}


void
lock_client_cache_rsm::releaser()
{

  // This method should be a continuous loop, waiting to be notified of
  // freed locks that have been revoked by the server, so that it can
  // send a release RPC.
  int r;
  lock_protocol::status ret = lock_protocol::OK;
  lock_protocol::xid_t t_xid;
  map<lock_protocol::lockid_t, lock>::iterator iter;
  while(1) {
    releasing_lock.deq(&iter);

    pthread_mutex_lock(&map_mutex);
    auto &lock  = iter->second;
    lock.has_revoked = false;
    t_xid = nextXid();
    pthread_mutex_unlock(&map_mutex); // rpc 请求不应该发生在持有本地锁的时候

    // 释放锁之前，刷新当前锁对应文件的缓存，其他客户端在获取文件时先获取锁，触发锁的释放，刷新文件，保证分布式系统的文件一致性
    if(lu)
      lu->dorelease(lock.lid);
    ret = rsmc->call(lock_protocol::release, lock.lid, id, t_xid, r);
    
    pthread_mutex_lock(&map_mutex);

    lock.state = NONE; // 锁服务进行释放后，锁不属于该客户端了
    // 唤醒所有在锁释放期间，对该锁的申请请求
    // 因为真实释放只有这一次，所以必须是唤醒所有，不被唤醒的线程会永远等待下去
    VERIFY (pthread_cond_broadcast(&lock.release_queue) == 0);

    pthread_mutex_unlock(&map_mutex);
  }
}


lock_protocol::status
lock_client_cache_rsm::acquire(lock_protocol::lockid_t lid)
{
  int r;
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&map_mutex);
  auto iter = lockid_lock.find(lid);
  if(iter == lockid_lock.end()) {
    iter = lockid_lock.emplace(lid, lid).first;
  }
  lock &lock = iter->second;
  lock_protocol::xid_t t_xid;
  while(true) {
    switch (lock.state) {
      case NONE: // 第一次请求目标锁，发送
        lock.state = ACQUIRING;
        lock.retry = false;
        t_xid = nextXid();
        pthread_mutex_unlock(&map_mutex);
        ret = rsmc->call(lock_protocol::acquire, lid, id, t_xid, r);
        pthread_mutex_lock(&map_mutex);
        if (ret == lock_protocol::OK) {  // 成功从锁服务获取到锁

          lock.state = LOCKED;
          goto out;
        } else if (ret == lock_protocol::RETRY) {
          // 此时锁被其他客户端占用，需要等待锁服务通知重试
          if (!lock.retry) {
            /**
             * 如果获取锁需要重试，锁服务会回复 RETRY，并在可以分配之后给客户端
             * 发送 retry 请求使客户端再次请求。由于网络原因，服务端后发送的
             * retry 可能在 RETRY
             * 回复之前到达客户端(lock.retry==1)，此时没必要将自己加入
             * retryqueue, 直接重新获取就好了
             */
            pthread_cond_wait(&lock.retry_queue, &map_mutex);
          }
        }
        break;

      case ACQUIRING: // 正在请求目标锁 或者 正在等待锁服务通知重试
        if (lock.retry) {  // 已收到 重试 请求，重新获取锁
          lock.retry = false;
          t_xid = nextXid();
          pthread_mutex_unlock(&map_mutex);
          ret = rsmc->call(lock_protocol::acquire, lid, id, t_xid, r);
          pthread_mutex_lock(&map_mutex);
          if (ret == lock_protocol::OK) {
            lock.state = LOCKED;
            goto out;
          } else if (ret == lock_protocol::RETRY) {
            
            if (!lock.retry) pthread_cond_wait(&lock.retry_queue, &map_mutex);
          }
        } else {  // 此时客户端有其他线程正在获取锁，等待获取完成
          pthread_cond_wait(&lock.wait_queue, &map_mutex);
        }
        break;

      case FREE:  // 锁被当前客户端占有，且是空闲的，直接占有
        lock.state = LOCKED;
        goto out;

      case LOCKED:  // 客户端已有其他线程占有锁，等待其他线程释放锁
        pthread_cond_wait(&lock.wait_queue, &map_mutex);
        break;

      case RELEASING:  // 客户端正在释放锁时，有其他线程获取锁，等待释放后重新请求锁
        VERIFY (pthread_cond_wait(&lock.release_queue, &map_mutex) == 0);
        break;
    }
  }

out:
  pthread_mutex_unlock(&map_mutex);
  return ret;
}

lock_protocol::status
lock_client_cache_rsm::release(lock_protocol::lockid_t lid)
{
  int r;
  lock_protocol::status ret = lock_protocol::OK;
  pthread_mutex_lock(&map_mutex);
  
  auto iter = lockid_lock.find(lid);
  if(iter == lockid_lock.end()) {
    printf("ERROR: can't find lock with lockid = %llu\n", lid);
    pthread_mutex_unlock(&map_mutex);
    return lock_protocol::NOENT;
  }
  lock &lock = iter->second;
  if(lock.has_revoked) { // 已经接收到锁服务的撤销请求，需要真正请求锁服务释放锁
    lock.state = RELEASING; // 当前锁正在被释放
    releasing_lock.enq(iter);
  } else { // 否则不需要真的在服务端释放锁
    lock.state = FREE;
    // 从该锁的等待队列中唤醒一个
    pthread_cond_signal(&lock.wait_queue);
  }

  pthread_mutex_unlock(&map_mutex);
  return ret;
}

/**
 * @brief 处理锁服务发送的 撤销 请求，放弃本客户端对锁的占用
 */
rlock_protocol::status
lock_client_cache_rsm::revoke_handler(lock_protocol::lockid_t lid, 
			          lock_protocol::xid_t xid, int &)
{
  int r;
  rlock_protocol::status ret = rlock_protocol::OK;
  pthread_mutex_lock(&map_mutex);
  auto iter = lockid_lock.find(lid);
  if(iter == lockid_lock.end()) {
    printf("ERROR: can't find lock with lockid = %llu\n", lid);
    pthread_mutex_unlock(&map_mutex);
    return lock_protocol::NOENT;
  }
  lock &lock = iter->second;
  if(lock.state == FREE) { // 当前锁空闲，可以直接请求锁服务释放
    lock.state = RELEASING; // 当前锁正在被释放
    releasing_lock.enq(iter);
  } else { // 否则标记当前锁需要撤销，在下一次客户端释放锁时，请求锁服务释放
    lock.has_revoked = true;
  } 


  pthread_mutex_unlock(&map_mutex);
  return ret;
}

/**
 * @brief 处理锁服务发送的 重试 请求，再次尝试占用目标锁
 */
rlock_protocol::status
lock_client_cache_rsm::retry_handler(lock_protocol::lockid_t lid, 
			         lock_protocol::xid_t xid, int &)
{
  rlock_protocol::status ret = rlock_protocol::OK;
  pthread_mutex_lock(&map_mutex);
  auto iter = lockid_lock.find(lid);
  if(iter == lockid_lock.end()) {
    printf("ERROR: can't find lock with lockid = %llu\n", lid);
    pthread_mutex_unlock(&map_mutex);
    return lock_protocol::NOENT;
  }

  lock &lock = iter->second;
  lock.retry = true; // 标记已经收到 重试 请求
  // 唤醒一个对该锁的申请请求
  pthread_cond_signal(&lock.retry_queue);

  pthread_mutex_unlock(&map_mutex);
  return ret;
}


