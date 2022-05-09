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
using std::unordered_map;

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
    cl->call(rlock_protocol::revoke, lock.lid, lock.requests[lock.owner].xid, r);
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
    cl->call(rlock_protocol::retry, lock.lid, lock.requests[client_need_retry].xid, r);
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
  auto &lr = lock.requests[id];
  if (lr.xid == xid) {  // 重复的请求
    tprintf("lock_server_cache_rsm::acquire duplicate lid %llu [%s]%llu\n", lid,
            id.c_str(), xid);

    if (lr.needRevoke) {
      tprintf(
          "lock_server_cache_rsm::acquire duplicate needRevoke lid %llu "
          "[%s]%llu\n",
          lid, id.c_str(), xid);

      revoking_locks.enq(iter);
    }

    return lr.lastRet;
  } else if (lr.xid > xid) {  // 过期的请求
    tprintf(
        "lock_server_cache_rsm::acquire out of date lid %llu [%s] %llu vs "
        "%llu\n",
        lid, id.c_str(), xid, lr.xid);
    return lock_protocol::IOERR;
  }

  lr.needRevoke = false;
  switch (lock.state) {
    case FREE:
      lock.state = LOCKED;
      lock.owner = id;
      break;

    case LOCKED:
      lock.state = LOCKED_AND_WAIT;
      lock.waiters.insert(id);
      lr.needRevoke = true;  // 有别的客户端在等待时，占用锁的客户端需要尽快释放
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
          lr.needRevoke = true;
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
  lr.xid = xid;
  lr.lastRet = ret;
  if (lr.needRevoke) {
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
    tprintf("ERROR: can't find lock with lockid = %llu\n", lid);
    return lock_protocol::NOENT;
  }

  lock &lock = iter->second;

  auto &lr = lock.requests[id];
  // 释放一个已释放的锁，处理重复的请求
  if (lr.xid == 0) {
    tprintf("lock_server_cache_rsm::release duplicate lid %llu [%s]%llu\n", lid,
           id.c_str(), xid);

    return lr.lastRet;
  }

  switch (lock.state) {
    case FREE:
    case RETRYING:
      tprintf("ERROR: can't release a lock with state RETRYING/FREE\n");
      ret = lock_protocol::IOERR;
      break;

    case LOCKED:  // 释放一个没有人等待的锁
      // 必须由持有锁的客户端释放，且 xid 是申请锁时的 xid
      if (lock.owner != id || lr.xid != xid) {
        tprintf(
            "lock_server_cache_rsm::release ERROR release [%s]%llu 's lock by "
            "[%s]%llu\n",
            lock.owner.c_str(), lr.xid, id.c_str(), xid);
        return lock_protocol::IOERR;
      }

      lock.state = lock_state::FREE;
      lock.owner = "";
      lr.xid = 0;
      break;

    case LOCKED_AND_WAIT:  // 释放一个有人等待的锁，挑选客户端发送重试请求
      if (lock.owner != id || lr.xid != xid) {
        tprintf(
            "lock_server_cache_rsm::release ERROR release [%s]%llu 's lock by "
            "[%s]%llu\n",
            lock.owner.c_str(), lr.xid, id.c_str(), xid);
        return lock_protocol::IOERR;
      }

      lock.state = lock_state::RETRYING;
      lock.owner = "";
      lr.xid = 0;
      // 触发等待的锁客户端进行 retry
      retring_locks.enq(iter);
      break;

    default:
      break;
  }
  
  // 更新本次请求的处理结果
  lr.lastRet = ret;
  lr.needRevoke = false;

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
    // waiters
    state << static_cast<unsigned long long>(lock.waiters.size());
    for (auto const &waiter : lock.waiters) {
      state << waiter;
    }
    // requests
    state << static_cast<unsigned long long>(lock.requests.size());
    for (auto const &request : lock.requests) {
      state << request.first << request.second.xid << request.second.lastRet
            << request.second.needRevoke;
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
    l.state = static_cast<enum lock_state>(l_state);
    // waiters
    unsigned int waiterSize;
    string w;
    set<string> waiters;
    s >> waiterSize;
    for (unsigned int j = 0; j < waiterSize; j++) {
      s >> w;
      waiters.insert(w);
    }
    l.waiters = waiters;
    // requests
    string cl;
    lock_protocol::xid_t xid;
    lock_protocol::status lastRet;
    bool needRevoke;
    unordered_map<string, last_request> requests;
    unsigned int requestSize;
    s >> requestSize;
    for (unsigned int j = 0; j < requestSize; j++) {
      s >> cl >> xid >> lastRet >> needRevoke;
      requests[cl].xid = xid;
      requests[cl].lastRet = lastRet;
      requests[cl].needRevoke = needRevoke;
    }
    l.requests = requests;
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

