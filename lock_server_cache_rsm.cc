// the caching lock server implementation

#include "lock_server_cache_rsm.h"
#include <sstream>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "lang/verify.h"
#include "handle.h"
#include "tprintf.h"
#include "slock.h"

using std::string;
using std::map;
using std::set;

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
  rsm->set_state_transfer(this);
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
    // 只有 master 才需要给客户端发送 RPC
    if(!rsm->amiprimary()) continue;
    
    pthread_mutex_lock(&map_mutex);
    auto &lock = iter->second;
    auto h = handle(lock.owner);
    pthread_mutex_unlock(&map_mutex);

    auto cl = h.safebind();
    cl->call(rlock_protocol::revoke, lock.lid, lock.xid, r);
    // TODO revoke 失败，代表 lock_client 崩溃了，则应该认为锁被释放了
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
    // 只有 master 才需要给客户端发送 RPC
    if(!rsm->amiprimary()) continue;

    pthread_mutex_lock(&map_mutex);
    auto &lock = iter->second;
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

  ScopedLock _l(&map_mutex);

  auto iter = lockid_lock.find(lid);
  if (iter == lockid_lock.end()) // 请求的锁不存在就新建一个
    iter = lockid_lock.insert({lid, lock(lid)}).first;

  lock &lock = iter->second;

  // 处理重复的请求
  if (lock.xid == xid && lock.lastRequirer == id) {
    printf("lock_server_cache_rsm::acquire duplicate lid %llu [%s:%llu]\n", lid,
           id.c_str(), xid);
    // 重发 revoke，防止 revoke 因为 master 崩溃丢失
    if (lock.needRevoke) {
      revoking_locks.enq(iter);
    }
    return lock.lastRet;
  }
  
  lock.needRevoke = false;
  switch (lock.state) {
    case FREE:
      lock.state = LOCKED;
      lock.owner = id;
      break;

    case LOCKED:
      lock.state = LOCKED_AND_WAIT;
      lock.waiters.insert(id);
      lock.needRevoke = true;  // 有别的客户端在等待时，占用锁的客户端需要尽快释放
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
          lock.needRevoke = true;
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

  // 更新本次请求的处理结果
  lock.xid = xid;
  lock.lastRequirer = id;
  lock.lastRet = ret;
  if (lock.needRevoke) {
    revoking_locks.enq(iter);
  }

  return ret;
}

int 
lock_server_cache_rsm::release(lock_protocol::lockid_t lid, std::string id, 
         lock_protocol::xid_t xid, int &r)
{
  lock_protocol::status ret = lock_protocol::OK;

  ScopedLock _l(&map_mutex);

  auto iter = lockid_lock.find(lid);
  if (iter == lockid_lock.end()) {
    printf("ERROR: can't find lock with lockid = %llu\n", lid);
    return lock_protocol::NOENT;
  }

  lock &lock = iter->second;
  // 处理重复的请求
  if (lock.xid == xid && lock.lastRequirer == id) {
    printf("lock_server_cache_rsm::release duplicate lid %llu [%s:%llu]\n", lid,
           id.c_str(), xid);

    return lock.lastRet;
  }

  switch (lock.state) {
    case FREE:
    case RETRYING:
      printf("ERROR: can't release a lock with state RETRYING/FREE\n");
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
  
  // 更新本次请求的处理结果
  lock.xid = xid;
  lock.lastRequirer = id;
  lock.lastRet = ret;
  lock.needRevoke = false;

  return ret;
}

/**
 * @brief 序列化所锁服务器的锁状态
 */
std::string
lock_server_cache_rsm::marshal_state()
{
  ScopedLock _l(&map_mutex);

  marshall state;
  state << static_cast<unsigned long long>(lockid_lock.size());
  for (const auto &lockpair : lockid_lock) {
    const auto &lock = lockpair.second;
    state << lock.lid << lock.owner << lock.revoked << lock.state;
    state << lock.xid << lock.lastRet << lock.lastRequirer << lock.needRevoke;
    state << static_cast<unsigned long long>(lock.waiters.size());
    for (auto const &waiter : lock.waiters) {
      state << waiter;
    }
  }

  return state.str();
}

/**
 * @brief 反序列化状态，恢复锁服务的状态
 */
void
lock_server_cache_rsm::unmarshal_state(std::string state)
{
  unmarshall s(state);
  unsigned int locksize;
  s >> locksize;
  lock_protocol::lockid_t lid;
  map<lock_protocol::lockid_t, lock> locks;
  for (unsigned int i = 0; i < locksize; i++) {
    s >> lid;
    auto iter = locks.insert({lid, lock(lid)}).first;
    auto &l = iter->second;
    int l_state;
    s >> l.owner >> l.revoked >> l_state;
    s >> l.xid >> l.lastRet >> l.lastRequirer >> l.needRevoke;
    l.state = static_cast<enum lock_state>(l_state);
    unsigned int waiterSize;
    string w;
    set<string> waiters;
    s >> waiterSize;
    for (unsigned int j = 0; j < waiterSize; j++) {
      s >> w;
      waiters.insert(w);
    }
    l.waiters = waiters;
  }
  // 先将状态反序列化到临时变量 locks 中，然后获取锁替换 lockid_lock
  {
    ScopedLock _l(&map_mutex);
    lockid_lock = locks;
  }
}

lock_protocol::status
lock_server_cache_rsm::stat(lock_protocol::lockid_t lid, int &r)
{
  printf("stat request\n");
  r = nacquire;
  return lock_protocol::OK;
}

