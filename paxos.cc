#include "paxos.h"
#include "handle.h"
// #include <signal.h>
#include <stdio.h>
#include "tprintf.h"
#include "lang/verify.h"

// This module implements the proposer and acceptor of the Paxos
// distributed algorithm as described by Lamport's "Paxos Made
// Simple".  To kick off an instance of Paxos, the caller supplies a
// list of nodes, a proposed value, and invokes the proposer.  If the
// majority of the nodes agree on the proposed value after running
// this instance of Paxos, the acceptor invokes the upcall
// paxos_commit to inform higher layers of the agreed value for this
// instance.


bool
operator> (const prop_t &a, const prop_t &b)
{
  return (a.n > b.n || (a.n == b.n && a.m > b.m));
}

bool
operator>= (const prop_t &a, const prop_t &b)
{
  return (a.n > b.n || (a.n == b.n && a.m >= b.m));
}

std::string
print_members(const std::vector<std::string> &nodes)
{
  std::string s;
  s.clear();
  for (unsigned i = 0; i < nodes.size(); i++) {
    s += nodes[i];
    if (i < (nodes.size()-1))
      s += ",";
  }
  return s;
}

/**
 * 判断 m 是否是 nodes 中的一个元素
 */
bool isamember(std::string m, const std::vector<std::string> &nodes)
{
  for (unsigned i = 0; i < nodes.size(); i++) {
    if (nodes[i] == m) return 1;
  }
  return 0;
}

bool
proposer::isrunning()
{
  bool r;
  ScopedLock ml(&pxs_mutex);
  r = !stable;
  return r;
}

// check if the servers in l2 contains a majority of servers in l1
/**
 * @brief 判断 l2 中是否包含了大多数的 l1 中所含节点
 * 
 * @param l1 全集
 * @param l2 reply集
 */
bool
proposer::majority(const std::vector<std::string> &l1, 
		const std::vector<std::string> &l2)
{
  unsigned n = 0;

  for (unsigned i = 0; i < l1.size(); i++) {
    if (isamember(l1[i], l2))
      n++;
  }
  return n >= (l1.size() >> 1) + 1;
}

proposer::proposer(class paxos_change *_cfg, class acceptor *_acceptor, 
		   std::string _me)
  : cfg(_cfg), acc (_acceptor), me (_me), break1 (false), break2 (false), 
    stable (true)
{
  VERIFY (pthread_mutex_init(&pxs_mutex, NULL) == 0);
  my_n.n = 0;
  my_n.m = me;
}

/**
 * 本节点 local:acceptor 收到一次其他节点 other:proposer 的 prepare 之后，
 * 会递增 last_rnd，如果 local 想在当前 paxos 实例内提出新的提案，应该使用
 * 的 rnd 是 last_rnd+1，因为只有全局最高 rnd 的提案才有效
 */ 
void
proposer::setn()
{
  my_n.n = acc->get_n_h().n + 1 > my_n.n + 1 ? acc->get_n_h().n + 1 : my_n.n + 1;
}

/**
 * @brief 启动一个新的编号为 instance 的 paxos，在当前集群 cur_nodes 中对 newv 达成一致
 * 
 * @param instance 实例编号
 * @param cur_nodes 当前视图
 * @param newv 需要达成一致的 新视图
 */
bool proposer::run(int instance, std::vector<std::string> cur_nodes,
                   std::string newv) {
  std::vector<std::string> accepts;
  std::vector<std::string> nodes;
  std::string v; // 本次提案要提交的数据
  bool r = false;

  ScopedLock ml(&pxs_mutex);
  tprintf("start: initiate paxos for %s w. i=%d v=%s stable=%d\n",
          print_members(cur_nodes).c_str(), instance, newv.c_str(), stable);
  if (!stable) {  // already running proposer?
    tprintf("proposer::run: already running\n");
    return false;
  }
  stable = false;
  setn(); // 设置 rnd
  accepts.clear();
  v.clear();
  // 进行一次提案，phase1 通知其他节点，准备进行修改
  if (prepare(instance, accepts, cur_nodes, v)) {
    // 如果收到多数节点的应答，继续 phase2
    if (majority(cur_nodes, accepts)) {
      tprintf("paxos::manager: received a majority of prepare responses\n");
      
      /**
       * 只有在本轮提案过程中没有已经被接受的其他提案时，
       * 才请求提交自己的提案值
       * 否则使用已接受的提案值，相当于执行一次已接受提案值的恢复
       */
      if (v.size() == 0) v = newv;

      breakpoint1();

      nodes = accepts;
      accepts.clear();
      // phase2 向有回应的节点发送修改内容
      accept(instance, accepts, nodes, v);
      // 如果收到多数节点的应答，对本次修改达成共识
      if (majority(cur_nodes, accepts)) {
        tprintf("paxos::manager: received a majority of accept responses\n");

        breakpoint2();
        // phase3 在已应答的节点上触发实际修改
        decide(instance, accepts, v);
        r = true;
      } else {
        tprintf("paxos::manager: no majority of accept responses\n");
      }
    } else {
      tprintf("paxos::manager: no majority of prepare responses\n");
    }
  } else {
    tprintf("paxos::manager: prepare is rejected %d\n", stable);
  }
  stable = true;
  return r;
}

// proposer::run() calls prepare to send prepare RPCs to nodes
// and collect responses. if one of those nodes
// replies with an oldinstance, return false.
// otherwise fill in accepts with set of nodes that accepted,
// set v to the v_a with the highest n_a, and return true.
bool
proposer::prepare(unsigned instance, std::vector<std::string> &accepts, 
         std::vector<std::string> nodes,
         std::string &v)
{
  // You fill this in for Lab 6
  // Note: if got an "oldinstance" reply, commit the instance using
  // acc->commit(...), and return false.
  paxos_protocol::preparearg a = {// 请求体
    .instance = instance, // 本次实例编号
    .n = my_n // rnd
  };
  paxos_protocol::prepareres r;  // 返回体
  int ret = 0;
  prop_t n_max = {.n = 0};

  for (const auto &n : nodes) {
    // 1. 向 nodes 请求 acceptor::preparereq
    handle h(n);
    pthread_mutex_unlock(&pxs_mutex);
    auto cl = h.safebind();
    if(cl)
      ret = cl->call(paxos_protocol::preparereq, me, a, r, rpcc::to(1000));
    pthread_mutex_lock(&pxs_mutex);
    if(cl) {
      if (ret == paxos_protocol::OK) {
        // 本次请求被 accptor 判断为已过期，直接用已达成共识的值更新本地视图
        if (r.oldinstance) { 
          acc->commit(instance, r.v_a);
          return false;
        }
        // 2. 在 accepts 中统计实际完成准备的节点
        if (r.accept) {
          accepts.push_back(n);
          /**
           * 在 prepare 阶段发现本次投票已经存在 accepted 的提案了
           * 
           * 如果已经有节点在本次 paxos 实例中接受了值 v1，
           * 已接受的值不能被改变，则应放弃当前提案的值，
           * 转而提案已经接受的值 v1
           * 这时实际上可以认为X执行了一次(不知是否已经中断的)其他Proposer的修复
           * 
           * X将看到的最大vrnd对应的v作为X的phase-2将要写入的值
           */
          if (r.n_a.n > n_max.n) {
            v = r.v_a;
            n_max = r.n_a;
          }
        }
      }
    }
  }
  return true;
}

// run() calls this to send out accept RPCs to accepts.
// fill in accepts with list of nodes that accepted.
void
proposer::accept(unsigned instance, std::vector<std::string> &accepts,
        std::vector<std::string> nodes, std::string v)
{
  // You fill this in for Lab 6
  paxos_protocol::acceptarg a = { // 请求体
    .instance = instance, // 本次实例编号
    .n = my_n, // rnd
    .v = v // 本次实例请求接受的值，可以是本节点提案值或者已接受值的恢复
  };
  bool r;
  int ret = 0;

  for (const auto &n : nodes) {
    // 1. 向 nodes 请求 acceptor::acceptreq
    handle h(n);
    VERIFY(pthread_mutex_unlock(&pxs_mutex) == 0);
    auto cl = h.safebind();
    if(cl) 
      ret = cl->call(paxos_protocol::acceptreq, me, a, r, rpcc::to(1000));
    VERIFY(pthread_mutex_lock(&pxs_mutex) == 0);
    // 2. 在 accepts 中统计实际达成共识的节点
    if(cl) {
      if (ret == paxos_protocol::OK) {
        if (r) {
          accepts.push_back(n);
        }
      }
    }
  }
}

void
proposer::decide(unsigned instance, std::vector<std::string> accepts, 
	      std::string v)
{
  // You fill this in for Lab 6
  // rpc call acceptor::decidereq to accepts
  paxos_protocol::decidearg a = {
    .instance = instance, // 本次达成共识的实例编号
    .v = v // 已达成共识的提案值
  };
  int r;

  for (const auto &n : accepts) {
    // 向 nodes 请求 acceptor::decidereq
    handle h(n);
    VERIFY(pthread_mutex_unlock(&pxs_mutex) == 0);
    auto cl = h.safebind();
    auto res = cl->call(paxos_protocol::decidereq, me, a, r, rpcc::to(1000));
    VERIFY(pthread_mutex_lock(&pxs_mutex) == 0);
  }
}

acceptor::acceptor(class paxos_change *_cfg, bool _first, std::string _me, 
	     std::string _value)
  : cfg(_cfg), me (_me), instance_h(0)
{
  VERIFY (pthread_mutex_init(&pxs_mutex, NULL) == 0);

  n_h.n = 0;
  n_h.m = me;
  n_a.n = 0;
  n_a.m = me;
  v_a.clear();

  l = new log (this, me);

  // 本节点是集群中的第一个节点
  if (instance_h == 0 && _first) {
    values[1] = _value;
    l->loginstance(1, _value);
    instance_h = 1;
  }

  pxs = new rpcs(atoi(_me.c_str()));
  pxs->reg(paxos_protocol::preparereq, this, &acceptor::preparereq);
  pxs->reg(paxos_protocol::acceptreq, this, &acceptor::acceptreq);
  pxs->reg(paxos_protocol::decidereq, this, &acceptor::decidereq);
}

paxos_protocol::status
acceptor::preparereq(std::string src, paxos_protocol::preparearg a,
    paxos_protocol::prepareres &r)
{
  // You fill this in for Lab 6
  // Remember to initialize *BOTH* r.accept and r.oldinstance appropriately.
  // Remember to *log* the proposal if the proposal is accepted.
  ScopedLock ml(&pxs_mutex);

  if (instance_h >= a.instance) {
    // 收到一个已经过期的 paxos 实例，
    // 已经决策完毕，直接返回最新的决策结果 v_a
    r.oldinstance = true;
    r.accept = false;
    r.v_a = v_a;
  } else if(a.n > n_h) { 
    // 正在进行的 paxos 实例
    // 准备接受更高的 round
    n_h = a.n; // 记录最新的提案轮次 last_rnd
    r.oldinstance = false;
    r.accept = true;
    r.n_a = n_a;
    r.v_a = v_a;
    l->logprop(n_h);
  } else {
    // 正在进行的 paxos 实例
    // 收到一个比 last_rnd 轮次更低的提案，拒绝
    r.accept = false;
    r.oldinstance = false;
  }

  return paxos_protocol::OK;
}

// the src argument is only for debug purpose
paxos_protocol::status
acceptor::acceptreq(std::string src, paxos_protocol::acceptarg a, bool &r)
{
  // You fill this in for Lab 6
  // Remember to *log* the accept if the proposal is accepted.
  ScopedLock ml(&pxs_mutex);

  if(a.n >= n_h) {
    // 收到一个等于 last_rnd 的写入请求
    // 接受写入
    n_a = a.n;
    v_a = a.v;
    r = true;
    l->logaccept(n_a, v_a);
  } else {
    // 拒绝轮次低于 last_rnd 的写入请求
    r = false;
  }

  return paxos_protocol::OK;
}

// the src argument is only for debug purpose
paxos_protocol::status
acceptor::decidereq(std::string src, paxos_protocol::decidearg a, int &r)
{
  ScopedLock ml(&pxs_mutex);
  tprintf("decidereq for accepted instance %d (my instance %d) v=%s\n", 
	 a.instance, instance_h, v_a.c_str());
  if (a.instance == instance_h + 1) {
    // 新版本的更新请求，提交最新的 v
    VERIFY(v_a == a.v); // 本地已接受的 v 和 请求更新的 v 应该要一致
    commit_wo(a.instance, v_a);
  } else if (a.instance <= instance_h) { // 更老版本的更新请求，忽略
    // we are ahead ignore.
  } else {
    // we are behind
    VERIFY(0);
  }
  return paxos_protocol::OK;
}

void
acceptor::commit_wo(unsigned instance, std::string value)
{
  //assume pxs_mutex is held
  tprintf("acceptor::commit: instance=%d has v= %s\n", instance, value.c_str());
  if (instance > instance_h) {
    tprintf("commit: highestaccepteinstance = %d\n", instance);
    values[instance] = value;
    l->loginstance(instance, value);
    instance_h = instance;
    // 重置 last_rnd=0 和 {v, vrnd}={0,0}
    n_h.n = 0;
    n_h.m = me;
    n_a.n = 0;
    n_a.m = me;
    v_a.clear();
    if (cfg) {
      pthread_mutex_unlock(&pxs_mutex);
      cfg->paxos_commit(instance, value);
      pthread_mutex_lock(&pxs_mutex);
    }
  }
}

void
acceptor::commit(unsigned instance, std::string value)
{
  ScopedLock ml(&pxs_mutex);
  commit_wo(instance, value);
}

std::string
acceptor::dump()
{
  return l->dump();
}

void
acceptor::restore(std::string s)
{
  l->restore(s);
  l->logread();
}



// For testing purposes

// Call this from your code between phases prepare and accept of proposer
void
proposer::breakpoint1()
{
  if (break1) {
    tprintf("Dying at breakpoint 1!\n");
    exit(1);
  }
}

// Call this from your code between phases accept and decide of proposer
void
proposer::breakpoint2()
{
  if (break2) {
    tprintf("Dying at breakpoint 2!\n");
    exit(1);
  }
}

void
proposer::breakpoint(int b)
{
  if (b == 3) {
    tprintf("Proposer: breakpoint 1\n");
    break1 = true;
  } else if (b == 4) {
    tprintf("Proposer: breakpoint 2\n");
    break2 = true;
  }
}
