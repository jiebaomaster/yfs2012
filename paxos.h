#ifndef paxos_h
#define paxos_h

#include <string>
#include <vector>
#include "rpc.h"
#include "paxos_protocol.h"
#include "log.h"


class paxos_change {
 public:
  virtual void paxos_commit(unsigned instance, std::string v) = 0;
  virtual ~paxos_change() {};
};

class acceptor {
 private:
  log *l;
  rpcs *pxs;
  paxos_change *cfg;
  std::string me;
  pthread_mutex_t pxs_mutex;

  // Acceptor state
  // last_rnd
  // 当前实例中，最后一次进行写前读取的 Proposer，以此决定在 phase2 中接受谁的提案
  prop_t n_h;		// number of the highest proposal seen in a prepare
  // {v_a, n_a} => {v, vrnd}
  // 当前实例中，最后被写入的值发生的 round
  prop_t n_a;		// number of highest proposal accepted
  // 当前实例中，最后被写入的值
  std::string v_a;	// value of highest proposal accepted
  /**
   * 一次 paxos 只能对一个值达成共识，如果要对一个值进行修改就要进行多次 paxos
   * 利用 instance 区分多次不同协商过程，一个 instance 对应一个已经达成的共识
   * instance 是全局统一的且递增的，且在 phase3 中同步
   * 在 phase1 中收到低实例号的请求会直接返回已经达成共识的值
   */
  // 最后达成共识的 paxos 实例号
  unsigned instance_h;	// number of the highest instance we have decided
  // 已通过的所有提案的记录，当有新的节点加入集群时，可由此获得集群的最新状态
  std::map<unsigned,std::string> values;	// vals of each instance

  void commit_wo(unsigned instance, std::string v);
  paxos_protocol::status preparereq(std::string src, 
          paxos_protocol::preparearg a,
          paxos_protocol::prepareres &r);
  paxos_protocol::status acceptreq(std::string src, 
          paxos_protocol::acceptarg a, bool &r);
  paxos_protocol::status decidereq(std::string src, 
          paxos_protocol::decidearg a, int &r);

  friend class log;

 public:
  acceptor(class paxos_change *cfg, bool _first, std::string _me, 
	std::string _value);
  ~acceptor() {};
  void commit(unsigned instance, std::string v);
  unsigned instance() { return instance_h; }
  std::string value(unsigned instance) { return values[instance]; }
  std::string dump();
  void restore(std::string);
  rpcs *get_rpcs() { return pxs; };
  prop_t get_n_h() { return n_h; };
  unsigned get_instance_h() { return instance_h; };
};

extern bool isamember(std::string m, const std::vector<std::string> &nodes);
extern std::string print_members(const std::vector<std::string> &nodes);

class proposer {
 private:
  log *l;
  paxos_change *cfg;
  acceptor *acc; // 本节点的 acceptor
  std::string me; // 节点标志字符串
  bool break1;
  bool break2;

  pthread_mutex_t pxs_mutex;

  // Proposer state
  bool stable; // 当前值是否已经稳定，稳定表示当前节点没有在执行 paxos
  // 当前 paxos 实例中最新的提案号，本地递增，也会在收到其他节点的提案时更新
  prop_t my_n;		// number of the last proposal used in this instance

  void setn();
  bool prepare(unsigned instance, std::vector<std::string> &accepts, 
         std::vector<std::string> nodes,
         std::string &v);
  void accept(unsigned instance, std::vector<std::string> &accepts, 
        std::vector<std::string> nodes, std::string v);
  void decide(unsigned instance, std::vector<std::string> accepts,
        std::string v);

  void breakpoint1();
  void breakpoint2();
  bool majority(const std::vector<std::string> &l1, const std::vector<std::string> &l2);

  friend class log;
 public:
  proposer(class paxos_change *cfg, class acceptor *_acceptor, std::string _me);
  ~proposer() {};
  bool run(int instance, std::vector<std::string> cnodes, std::string v);
  bool isrunning();
  void breakpoint(int b);
};



#endif /* paxos_h */
