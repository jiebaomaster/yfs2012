#include "rsm_client.h"
#include <vector>
#include <arpa/inet.h>
#include <stdio.h>
#include <handle.h>
#include "lang/verify.h"


rsm_client::rsm_client(std::string dst)
{
  printf("create rsm_client\n");
  std::vector<std::string> mems;

  pthread_mutex_init(&rsm_client_mutex, NULL);
  sockaddr_in dstsock;
  make_sockaddr(dst.c_str(), &dstsock);
  primary = dst;

  {
    ScopedLock ml(&rsm_client_mutex);
    VERIFY (init_members());
  }
  printf("rsm_client: done\n");
}

// Assumes caller holds rsm_client_mutex 
void
rsm_client::primary_failure()
{
  // You fill this in for Lab 7
}

/**
 * @brief 向集群的主节点发请求，重复尝试，直到请求成功
 * 
 * @param proc 
 * @param req 
 * @param rep 
 */
rsm_protocol::status
rsm_client::invoke(int proc, std::string req, std::string &rep)
{
  int ret;
  ScopedLock ml(&rsm_client_mutex);
  while (1) {
    printf("rsm_client::invoke proc %x primary %s\n", proc, primary.c_str());
    handle h(primary);

    VERIFY(pthread_mutex_unlock(&rsm_client_mutex)==0);
    rpcc *cl = h.safebind();
    if (cl) {
      ret = cl->call(rsm_client_protocol::invoke, proc, req, 
                     rep, rpcc::to(5000));
    }
    VERIFY(pthread_mutex_lock(&rsm_client_mutex)==0);

    if (!cl) {
      goto prim_fail;
    }

    printf("rsm_client::invoke proc %x primary %s ret %d\n", proc, 
           primary.c_str(), ret);
    if (ret == rsm_client_protocol::OK) {
      break;
    }
    // RSM 集群正在同步，过段时间再次请求
    if (ret == rsm_client_protocol::BUSY) {
      printf("rsm is busy %s\n", primary.c_str());
      sleep(3);
      continue;
    }
    // 本次请求发给了非主节点，即本地 primary 失效了，需要拉取新的 集群视图
    // 因为选主只需要大多数节点的回复，所以本节点可能不知道重新选主了
    // 而原先的 primary 返回他不是主节点了，则他肯定知道最新的集群视图
    if (ret == rsm_client_protocol::NOTPRIMARY) {
      printf("primary %s isn't the primary--let's get a complete list of mems\n", 
             primary.c_str());
      if (init_members())
        continue;
    }
prim_fail:
    printf("primary %s failed ret %d\n", primary.c_str(), ret);
    primary_failure();
    printf ("rsm_client::invoke: retry new primary %s\n", primary.c_str());
  }
  return ret;
}

/**
 * @brief 从当前 primary 节点拉取最新的 集群视图
 */
bool
rsm_client::init_members()
{
  printf("rsm_client::init_members get members!\n");
  handle h(primary);
  std::vector<std::string> new_view; // 拉取的新集群视图
  VERIFY(pthread_mutex_unlock(&rsm_client_mutex)==0);
  int ret;
  rpcc *cl = h.safebind();
  if (cl) {
    ret = cl->call(rsm_client_protocol::members, 0, new_view, 
                   rpcc::to(1000)); 
  }
  VERIFY(pthread_mutex_lock(&rsm_client_mutex)==0);
  if (cl == 0 || ret != rsm_protocol::OK)
    return false;
  if (new_view.size() < 1) {
    printf("rsm_client::init_members do not know any members!\n");
    VERIFY(0);
  }
  known_mems = new_view; // 更新视图
  primary = known_mems.back(); // 更新主节点
  known_mems.pop_back();

  printf("rsm_client::init_members: primary %s\n", primary.c_str());

  return true;
}

