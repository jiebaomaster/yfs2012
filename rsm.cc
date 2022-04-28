//
// Replicated state machine implementation with a primary and several
// backups. The primary receives requests, assigns each a view stamp (a
// vid, and a sequence number) in the order of reception, and forwards
// them to all backups. A backup executes requests in the order that
// the primary stamps them and replies with an OK to the primary. The
// primary executes the request after it receives OKs from all backups,
// and sends the reply back to the client.
/**
 * 具有一个主节点和多个从节点的复制状态机实现。主服务器接收请求，按接收顺序为每个请求
 * 分配一个视图戳（一个 vid 和一个序列号），并将请求转发给所有从节点。从节点会按照主
 * 节点标记请求的顺序执行请求，并依次向主服务器回复 OK。主节点从所有从节点接收到 OK 
 * 之后执行请求，并将回复发送回客户端。
 */
//
// The config module will tell the RSM about a new view. If the
// primary in the previous view is a member of the new view, then it
// will stay the primary.  Otherwise, the smallest numbered node of
// the previous view will be the new primary.  In either case, the new
// primary will be a node from the previous view.  The configuration
// module constructs the sequence of views for the RSM and the RSM
// ensures there will be always one primary, who was a member of the
// last view.
/**
 * 新的视图会通过 config 模块通知 RSM。
 * 如果前一个视图的主节点也在新的视图中，主节点不变；否则，前一个视图中编号最小的
 * 节点将成为新的主节点。在任何一种情况下，新的主节点都是上一个视图中的节点，这是
 * 因为新加入的节点需要同步信息，要避免成为主节点。
 * config 模块为 RSM 构建了一系列的视图，而 RSM 保证一定会有一个主节点，且这个
 * 主节点是最新视图中的节点。
 */
//
// When a new node starts, the recovery thread is in charge of joining
// the RSM.  It will collect the internal RSM state from the primary;
// the primary asks the config module to add the new node and returns
// to the joining the internal RSM state (e.g., paxos log). Since
// there is only one primary, all joins happen in well-defined total
// order.
/**
 * 当新节点启动时，会创建一个 recovery 线程，并由他负责加入 RSM 的过程。将从主节点
 * 拉取内部 RSM 状态；向主节点 RPC join，主节点启动一个 paxos 在当前视图下对新节点
 * 的加入达成共识。因为只有一个主节点，所以所有 join 请求都以定义良好的总顺序发生。
 */
//
// The recovery thread also runs during a view change (e.g, when a node
// has failed).  After a failure some of the backups could have
// processed a request that the primary has not, but those results are
// not visible to clients (since the primary responds).  If the
// primary of the previous view is in the current view, then it will
// be the primary and its state is authoritive: the backups download
// from the primary the current state.  A primary waits until all
// backups have downloaded the state.  Once the RSM is in sync, the
// primary accepts requests again from clients.  If one of the backups
// is the new primary, then its state is authoritative.  In either
// scenario, the next view uses a node as primary that has the state
// resulting from processing all acknowledged client requests.
// Therefore, if the nodes sync up before processing the next request,
// the next view will have the correct state.
/**
 * recovery 线程也会在视图更改期间运行（例如，当节点出现故障时）。发生失败后，一些从节点
 * 可能已经处理了主服务器没有处理的请求，但这些结果对客户端不可见（因为主服务器有响应）。
 * 如果上一个视图的主节点在当前视图中，则主节点不变，并且其状态是正确的，从节点下载主节点
 * 的当前状态（将主节点状态同步到所有从节点，状态回滚）。一旦 RSM 完成同步，主服务器将再次
 * 接受来自客户端的请求。如果主节点发生了变动，则从新的主节点同步状态。在任何一种情况下，
 * 都会有一个节点成为主节点，该节点具有所有已确认的客户端请求所产生的状态。因此，如果集群在
 * 处理下一个请求之前完成同步，下一个视图将具有正确的状态。
 */
//
// While the RSM in a view change (i.e., a node has failed, a new view
// has been formed, but the sync hasn't completed), another failure
// could happen, which complicates a view change.  During syncing the
// primary or backups can timeout, and initiate another Paxos round.
// There are 2 variables that RSM uses to keep track in what state it
// is:
//    - inviewchange: a node has failed and the RSM is performing a view change
//    - insync: this node is syncing its state
//
// If inviewchange is false and a node is the primary, then it can
// process client requests. If it is true, clients are told to retry
// later again.  While inviewchange is true, the RSM may go through several
// member list changes, one by one.   After a member list
// change completes, the nodes tries to sync. If the sync complets,
// the view change completes (and inviewchange is set to false).  If
// the sync fails, the node may start another member list change
// (inviewchange = true and insync = false).
//
/**
 * 当 RSM 正在视图更改时（即节点发生故障，新视图已形成，但同步尚未完成），可能会发生另一个故障，
 * 从而使视图更改复杂化。同步期间，主节点或主节点可能会超时，并启动另一轮 Paxos。RSM 使用
 * 两个变量来跟踪其状态：
 *  - inviewchange：节点出现故障，RSM 正在执行视图更改
 *  - insync：当前节点正在同步状态
 * 如果主节点的 inviewchange 为 false，则它可以处理客户端请求；否则，客户端会被告知稍后重试。
 * 在 inviewchange 为 true 时，RSM 可能会逐个经历几次成员列表的更改。成员列表更改完成后，
 * 集群将尝试进行同步。如果同步完成，则视图更改完成（inviewchange 设置为 false）。如果同步失败，
 * 节点可能会启动另一次成员列表更改（inviewchange=true 和 insync=false）。
 */
// The implementation should be used only with servers that run all
// requests run to completion; in particular, a request shouldn't
// block.  If a request blocks, the backup won't respond to the
// primary, and the primary won't execute the request.  A request may
// send an RPC to another host, but the RPC should be a one-way
// message to that host; the backup shouldn't do anything based on the
// response or execute after the response, because it is not
// guaranteed that all backup will receive the same response and
// execute in the same order.
/**
 * 该实现应仅用于运行所有请求直至完成的服务器；特别是，请求不应该被阻塞。
 * 如果请求被阻塞，从节点将不会响应主节点，主节点上也不会执行请求。
 * 从节点在处理请求时可以向另一个节点发送 RPC，但此 RPC 只能是单向通知；
 * 从节点不应基于 RPC 响应执行任何操作或在响应后执行处理，因为不能保证所有从节点
 * 都会收到相同的响应并以相同的顺序执行。
 */
//
// The implementation can be viewed as a layered system:
//       RSM module     ---- in charge of replication
//       config module  ---- in charge of view management
//       Paxos module   ---- in charge of running Paxos to agree on a value
//
// Each module has threads and internal locks. Furthermore, a thread
// may call down through the layers (e.g., to run Paxos's proposer).
// When Paxos's acceptor accepts a new value for an instance, a thread
// will invoke an upcall to inform higher layers of the new value.
// The rule is that a module releases its internal locks before it
// upcalls, but can keep its locks when calling down.
/**
 * 此种实现可以看作是一个分层的系统：
 *   RSM 模块     --- 管理复制集 
 *   config 模块  --- 负责视图管理
 *   Paxos 模块   --- 负责运行 Paxos 对某个值达成共识
 * 
 * 每个模块都有线程和内部锁。此外，线程可能跨越多层调用，例如，运行Paxos的proposer。
 * 当 Paxos 的 acceptor 接受一个实例的新值时，一个线程将进行向上调用来通知更高的层这个新值。
 * 规则是，模块在向上调用之前释放其内部锁，但在向下调用时可以保留其锁。
 */ 

#include <fstream>
#include <iostream>

#include "handle.h"
#include "rsm.h"
#include "tprintf.h"
#include "lang/verify.h"
#include "rsm_client.h"

using std::string;

static void *
recoverythread(void *x)
{
  rsm *r = (rsm *) x;
  r->recovery();
  return 0;
}

rsm::rsm(std::string _first, std::string _me) 
  : stf(0), primary(_first), insync (false), inviewchange (true), vid_commit(0),
    partitioned (false), dopartition(false), break1(false), break2(false)
{
  pthread_t th;

  last_myvs.vid = 0;
  last_myvs.seqno = 0;
  myvs = last_myvs;
  myvs.seqno = 1;

  pthread_mutex_init(&rsm_mutex, NULL);
  pthread_mutex_init(&invoke_mutex, NULL);
  pthread_cond_init(&recovery_cond, NULL);
  pthread_cond_init(&sync_cond, NULL);

  cfg = new config(_first, _me, this);

  if (_first == _me) {
    // Commit the first view here. We can not have acceptor::acceptor
    // do the commit, since at that time this->cfg is not initialized
    commit_change(1);
  }
  rsmrpc = cfg->get_rpcs();
  rsmrpc->reg(rsm_client_protocol::invoke, this, &rsm::client_invoke);
  rsmrpc->reg(rsm_client_protocol::members, this, &rsm::client_members);
  rsmrpc->reg(rsm_protocol::invoke, this, &rsm::invoke);
  rsmrpc->reg(rsm_protocol::transferreq, this, &rsm::transferreq);
  rsmrpc->reg(rsm_protocol::transferdonereq, this, &rsm::transferdonereq);
  rsmrpc->reg(rsm_protocol::joinreq, this, &rsm::joinreq);

  // tester must be on different port, otherwise it may partition itself
  testsvr = new rpcs(atoi(_me.c_str()) + 1);
  testsvr->reg(rsm_test_protocol::net_repair, this, &rsm::test_net_repairreq);
  testsvr->reg(rsm_test_protocol::breakpoint, this, &rsm::breakpointreq);

  /**
   * 实例化rsm服务端后，创建一个线程，从主服务同步状态
   */
  {
      ScopedLock ml(&rsm_mutex);
      VERIFY(pthread_create(&th, NULL, &recoverythread, (void *) this) == 0);
  }
}

void
rsm::reg1(int proc, handler *h)
{
  ScopedLock ml(&rsm_mutex);
  procs[proc] = h;
}

// The recovery thread runs this function
void
rsm::recovery()
{
  bool r = true;
  ScopedLock ml(&rsm_mutex);

  while (1) {
    // 如果当前节点还不在 视图 中，通过 config 将当前节点加入视图
    while (!cfg->ismember(cfg->myaddr(), vid_commit)) {
      if (join(primary)) {
        tprintf("recovery: joined\n");
        commit_change_wo(cfg->vid());
      } else {
        VERIFY(pthread_mutex_unlock(&rsm_mutex) == 0);
        sleep(5);  // XXX make another node in cfg primary?
        VERIFY(pthread_mutex_lock(&rsm_mutex) == 0);
      }
    }
    vid_insync = vid_commit;
    tprintf("recovery: sync vid_insync %d\n", vid_insync);
    if (primary == cfg->myaddr()) {
      r = sync_with_backups();
    } else {
      r = sync_with_primary();
    }
    tprintf("recovery: sync done\n");

    // If there was a commited viewchange during the synchronization, restart
    // the recovery
    if (vid_insync != vid_commit)
      continue;

    if (r) { 
      myvs.vid = vid_commit;
      myvs.seqno = 1;
      inviewchange = false;
    }
    tprintf("recovery: go to sleep %d %d\n", insync, inviewchange);
    // 执行一次数据恢复之后，睡眠，等待下次需要恢复的时机
    pthread_cond_wait(&recovery_cond, &rsm_mutex);
  }
}

bool
rsm::sync_with_backups()
{
  pthread_mutex_unlock(&rsm_mutex);
  {
    // Make sure that the state of lock_server_cache_rsm is stable during 
    // synchronization; otherwise, the primary's state may be more recent
    // than replicas after the synchronization.
    ScopedLock ml(&invoke_mutex);
    // By acquiring and releasing the invoke_mutex once, we make sure that
    // the state of lock_server_cache_rsm will not be changed until all
    // replicas are synchronized. The reason is that client_invoke arrives
    // after this point of time will see inviewchange == true, and returns
    // BUSY.
  }
  pthread_mutex_lock(&rsm_mutex);
  // Start accepting synchronization request (statetransferreq) now!
  insync = true;
  // You fill this in for Lab 7
  // Wait until
  //   - all backups in view vid_insync are synchronized
  //   - or there is a committed viewchange
  insync = false;
  return true;
}


bool
rsm::sync_with_primary()
{
  // Remember the primary of vid_insync
  std::string m = primary;
  // You fill this in for Lab 7
  // Keep synchronizing with primary until the synchronization succeeds,
  // or there is a commited viewchange
  return true;
}


/**
 * Call to transfer state from m to the local node.
 * Assumes that rsm_mutex is already held.
 */
bool
rsm::statetransfer(std::string m)
{
  // Code will be provided in Lab 7
  rsm_protocol::transferres r;
  handle h(m);
  int ret;
  tprintf("rsm::statetransfer: contact %s w. my last_myvs(%d,%d)\n", 
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  VERIFY(pthread_mutex_unlock(&rsm_mutex)==0);
  rpcc *cl = h.safebind();
  if (cl) {
    ret = cl->call(rsm_protocol::transferreq, cfg->myaddr(), 
                             last_myvs, vid_insync, r, rpcc::to(1000));
  }
  VERIFY(pthread_mutex_lock(&rsm_mutex)==0);
  if (cl == 0 || ret != rsm_protocol::OK) {
    tprintf("rsm::statetransfer: couldn't reach %s %lx %d\n", m.c_str(), 
	   (long unsigned) cl, ret);
    return false;
  }
  if (stf && last_myvs != r.last) {
    stf->unmarshal_state(r.state);
  }
  last_myvs = r.last;
  tprintf("rsm::statetransfer transfer from %s success, vs(%d,%d)\n", 
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  return true;
}

bool
rsm::statetransferdone(std::string m) {
  // You fill this in for Lab 7
  // - Inform primary that this slave has synchronized for vid_insync
  return true;
}


bool
rsm::join(std::string m) {
  handle h(m);
  int ret;
  rsm_protocol::joinres r;

  tprintf("rsm::join: %s mylast (%d,%d)\n", m.c_str(), last_myvs.vid, 
          last_myvs.seqno);
  VERIFY(pthread_mutex_unlock(&rsm_mutex)==0);
  rpcc *cl = h.safebind();
  if (cl != 0) {
    ret = cl->call(rsm_protocol::joinreq, cfg->myaddr(), last_myvs, 
		   r, rpcc::to(120000));
  }
  VERIFY(pthread_mutex_lock(&rsm_mutex)==0);

  if (cl == 0 || ret != rsm_protocol::OK) {
    tprintf("rsm::join: couldn't reach %s %p %d\n", m.c_str(), 
	   cl, ret);
    return false;
  }
  tprintf("rsm::join: succeeded %s\n", r.log.c_str());
  cfg->restore(r.log);
  return true;
}

/*
 * Config informs rsm whenever it has successfully 
 * completed a view change
 */
void 
rsm::commit_change(unsigned vid) 
{
  ScopedLock ml(&rsm_mutex);
  commit_change_wo(vid);
}

void 
rsm::commit_change_wo(unsigned vid) 
{
  if (vid <= vid_commit)
    return;
  tprintf("commit_change: new view (%d)  last vs (%d,%d) %s insync %d\n", 
	 vid, last_myvs.vid, last_myvs.seqno, primary.c_str(), insync);
  vid_commit = vid;
  inviewchange = true;
  set_primary(vid);
  pthread_cond_signal(&recovery_cond);
  if (cfg->ismember(cfg->myaddr(), vid_commit))
    breakpoint2();
}


/**
 * @brief 在本地执行对 lock_server_cache_rsm 的请求
 * 请求在 lock_smain.cc 中通过 rsm.reg 注册到 procs 中
 * 
 * @param procno 请求的类型编号
 * @param req 请求参数
 * @param r 返回值
 */
void
rsm::execute(int procno, std::string req, std::string &r)
{
  tprintf("execute\n");
  handler *h = procs[procno];
  VERIFY(h);
  unmarshall args(req);
  marshall rep;
  std::string reps;
  rsm_protocol::status ret = h->fn(args, rep);
  marshall rep1;
  rep1 << ret;
  rep1 << rep.str();
  r = rep1.str();
}

// 对集群的所有操作都通过主节点执行，主节点转发操作给从节点
// Clients call client_invoke to invoke a procedure on the replicated state
// machine: the primary receives the request, assigns it a sequence
// number, and invokes it on all members of the replicated state
// machine.
// 处理 rsm_client 发来的调用请求，开启一个 rsm 内的调用过程
rsm_client_protocol::status
rsm::client_invoke(int procno, std::string req, std::string &r)
{
  int ret = rsm_client_protocol::OK;
  // 整个请求的处理期间都需要持有锁，保持请求的按序执行
  ScopedLock _l(&invoke_mutex);
  // You fill this in for Lab 7
  // 正在同步的不能处理客户端请求
  if(inviewchange) {
    ret = rsm_client_protocol::BUSY;
    return ret;
  }
  // 非 master 不能处理客户端请求
  if(cfg->myaddr() != primary) {
    ret = rsm_client_protocol::NOTPRIMARY;
    return ret;
  }

  auto vs = myvs;
  myvs.seqno++; // 更新序列号
  auto m = cfg->get_view(vid_commit);
  int dummy;
  for(auto &n : m) { // 转发请求给视图中所有 slave
    if(n == cfg->myaddr()) continue;

    handle h(n);
    auto cl = h.safebind();
    if(cl)
      ret = cl->call(rsm_protocol::invoke, procno, vs, req, dummy, rpcc::to(1000));
    else {
      // TODO slave 奔溃，触发视图修改
      printf("client_invoke slave %s clash!\n", n);
      ret = rsm_client_protocol::BUSY;
      return ret;
    }
    // rsm 内部错误
    if(ret == rsm_protocol::ERR) {
      printf("client_invoke invoke on slave %s error\n", n);
      ret = rsm_client_protocol::BUSY;
      return ret;
    }
  }
  // 在 master 执行锁协议调用
  execute(procno, req, r);

  return ret;
}

// 
// The primary calls the internal invoke at each member of the
// replicated state machine 
//
// the replica must execute requests in order (with no gaps) 
// according to requests' seqno 
// 在 rsm 内部，slave 处理 master 发来的调用
rsm_protocol::status
rsm::invoke(int proc, viewstamp vs, std::string req, int &dummy)
{
  rsm_protocol::status ret = rsm_protocol::OK;
  // You fill this in for Lab 7
  string r;
  ScopedLock _l(&invoke_mutex);
  if(myvs > vs) { // 更小序号的请求已经被处理过了
    goto out;
  }
  if (myvs > vs                       // 请求必须按序到达
      || inviewchange                 // 正在同步
      || cfg->myaddr() == primary) {  // slave 才能处理
    ret = rsm_protocol::ERR;
    goto out;
  }
  // 在本地执行请求
  execute(proc, req, r);
  // 更新期望的 viewstamp
  myvs.seqno++;

out:
  return ret;
}

/**
 * RPC handler: Send back the local node's state to the caller
 */
rsm_protocol::status
rsm::transferreq(std::string src, viewstamp last, unsigned vid, 
rsm_protocol::transferres &r)
{
  ScopedLock ml(&rsm_mutex);
  int ret = rsm_protocol::OK;
  // Code will be provided in Lab 7
  tprintf("transferreq from %s (%d,%d) vs (%d,%d)\n", src.c_str(), 
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);
  if (!insync || vid != vid_insync) {
     return rsm_protocol::BUSY;
  }
  if (stf && last != last_myvs) 
    r.state = stf->marshal_state();
  r.last = last_myvs;
  return ret;
}

/**
  * RPC handler: Inform the local node (the primary) that node m has synchronized
  * for view vid
  */
rsm_protocol::status
rsm::transferdonereq(std::string m, unsigned vid, int &)
{
  int ret = rsm_protocol::OK;
  ScopedLock ml(&rsm_mutex);
  // You fill this in for Lab 7
  // - Return BUSY if I am not insync, or if the slave is not synchronizing
  //   for the same view with me
  // - Remove the slave from the list of unsynchronized backups
  // - Wake up recovery thread if all backups are synchronized
  return ret;
}

// a node that wants to join an RSM as a server sends a
// joinreq to the RSM's current primary; this is the
// handler for that RPC.
// 主节点处理新节点的添加
rsm_protocol::status
rsm::joinreq(std::string m, viewstamp last, rsm_protocol::joinres &r)
{
  int ret = rsm_protocol::OK;

  ScopedLock ml(&rsm_mutex);
  tprintf("joinreq: src %s last (%d,%d) mylast (%d,%d)\n", m.c_str(), 
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);
  if (cfg->ismember(m, vid_commit)) { // 已经在当前视图中了，不用处理
    tprintf("joinreq: is still a member\n");
    r.log = cfg->dump();
  } else if (cfg->myaddr() != primary) { // 本节点已经不是主节点了，拒绝
    tprintf("joinreq: busy\n");
    ret = rsm_protocol::BUSY;
  } else {
    // We cache vid_commit to avoid adding m to a view which already contains 
    // m due to race condition
    unsigned vid_cache = vid_commit;
    VERIFY (pthread_mutex_unlock(&rsm_mutex) == 0);
    bool succ = cfg->add(m, vid_cache);
    VERIFY (pthread_mutex_lock(&rsm_mutex) == 0);
    if (cfg->ismember(m, cfg->vid())) { // 加入成功
      r.log = cfg->dump();
      tprintf("joinreq: ret %d log %s\n:", ret, r.log.c_str());
    } else {
      tprintf("joinreq: failed; proposer couldn't add %d\n", succ);
      ret = rsm_protocol::BUSY;
    }
  }
  return ret;
}

/*
 * RPC handler: Send back all the nodes this local knows about to client
 * so the client can switch to a different primary 
 * when it existing primary fails
 */
rsm_client_protocol::status
rsm::client_members(int i, std::vector<std::string> &r)
{
  std::vector<std::string> m;
  ScopedLock ml(&rsm_mutex);
  m = cfg->get_view(vid_commit);
  m.push_back(primary); // primary 单独放在视图最后返回
  r = m;
  tprintf("rsm::client_members return %s m %s\n", print_members(m).c_str(),
	 primary.c_str());
  return rsm_client_protocol::OK;
}

// if primary is member of new view, that node is primary
// otherwise, the lowest number node of the previous view.
// caller should hold rsm_mutex
void
rsm::set_primary(unsigned vid)
{
  std::vector<std::string> c = cfg->get_view(vid);
  std::vector<std::string> p = cfg->get_view(vid - 1);
  VERIFY (c.size() > 0);

  if (isamember(primary,c)) {
    tprintf("set_primary: primary stays %s\n", primary.c_str());
    return;
  }

  VERIFY(p.size() > 0);
  for (unsigned i = 0; i < p.size(); i++) {
    if (isamember(p[i], c)) {
      primary = p[i];
      tprintf("set_primary: primary is %s\n", primary.c_str());
      return;
    }
  }
  VERIFY(0);
}

bool
rsm::amiprimary()
{
  ScopedLock ml(&rsm_mutex);
  return primary == cfg->myaddr() && !inviewchange;
}


// Testing server

// Simulate partitions

// assumes caller holds rsm_mutex
void
rsm::net_repair_wo(bool heal)
{
  std::vector<std::string> m;
  m = cfg->get_view(vid_commit);
  for (unsigned i  = 0; i < m.size(); i++) {
    if (m[i] != cfg->myaddr()) {
        handle h(m[i]);
	tprintf("rsm::net_repair_wo: %s %d\n", m[i].c_str(), heal);
	if (h.safebind()) h.safebind()->set_reachable(heal);
    }
  }
  rsmrpc->set_reachable(heal);
}

rsm_test_protocol::status 
rsm::test_net_repairreq(int heal, int &r)
{
  ScopedLock ml(&rsm_mutex);
  tprintf("rsm::test_net_repairreq: %d (dopartition %d, partitioned %d)\n", 
	 heal, dopartition, partitioned);
  if (heal) {
    net_repair_wo(heal);
    partitioned = false;
  } else {
    dopartition = true;
    partitioned = false;
  }
  r = rsm_test_protocol::OK;
  return r;
}

// simulate failure at breakpoint 1 and 2

void 
rsm::breakpoint1()
{
  if (break1) {
    tprintf("Dying at breakpoint 1 in rsm!\n");
    exit(1);
  }
}

void 
rsm::breakpoint2()
{
  if (break2) {
    tprintf("Dying at breakpoint 2 in rsm!\n");
    exit(1);
  }
}

void 
rsm::partition1()
{
  if (dopartition) {
    net_repair_wo(false);
    dopartition = false;
    partitioned = true;
  }
}

rsm_test_protocol::status
rsm::breakpointreq(int b, int &r)
{
  r = rsm_test_protocol::OK;
  ScopedLock ml(&rsm_mutex);
  tprintf("rsm::breakpointreq: %d\n", b);
  if (b == 1) break1 = true;
  else if (b == 2) break2 = true;
  else if (b == 3 || b == 4) cfg->breakpoint(b);
  else r = rsm_test_protocol::ERR;
  return r;
}




