// extent client interface.

#ifndef extent_client_h
#define extent_client_h

#include <string>
#include <map>
#include "extent_protocol.h"
#include "rpc.h"

// rpc 客户端的接口类，调用 rpc 服务，使得我们可以灵活修改更换 rpc 框架
class extent_client {
 private:
 protected:
  rpcc *cl;

 public:
  extent_client(std::string dst);

  virtual extent_protocol::status get(extent_protocol::extentid_t eid, 
			      std::string &buf);
  virtual extent_protocol::status getattr(extent_protocol::extentid_t eid, 
				  extent_protocol::attr &a);
  virtual extent_protocol::status put(extent_protocol::extentid_t eid, std::string buf);
  virtual extent_protocol::status remove(extent_protocol::extentid_t eid);
};

class extent_client_cache : public extent_client {
  enum file_state {NONE, UPDATED, MODIFIED, REMOVED};
  struct extent {
    std::string data;
    file_state state;
    extent_protocol::attr attr;
    extent() : state(NONE){}
  };
public:
  extent_client_cache(std::string dst);

  extent_protocol::status get(extent_protocol::extentid_t, std::string&) override;
  extent_protocol::status getattr(extent_protocol::extentid_t, extent_protocol::attr&) override;
  extent_protocol::status put(extent_protocol::extentid_t, std::string) override;
  extent_protocol::status remove(extent_protocol::extentid_t) override;

  extent_protocol::status flush(extent_protocol::extentid_t);

private:
  std::map<extent_protocol::extentid_t, extent> file_cached;
  pthread_mutex_t extent_mutex;
};

#endif 

