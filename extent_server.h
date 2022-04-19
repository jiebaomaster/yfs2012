// this is the extent server

#ifndef extent_server_h
#define extent_server_h

#include <string>
#include <map>
#include "extent_protocol.h"

/**
 * 文件存储服务，模拟远程磁盘，提供以 i-number 标识的文件操作
 */
class extent_server {

 public:
  struct extent { // 文件类型
    std::string data; // 文件数据
    extent_protocol::attr attr; // 文件属性
  };

  // id->文件
  std::map<extent_protocol::extentid_t, extent> file_map;
  pthread_mutex_t map_mutex; // 控制 map 的互斥访问

  extent_server();

  /* 对文件的操作 */
  int put(extent_protocol::extentid_t id, std::string, int &);
  int get(extent_protocol::extentid_t id, std::string &);
  int getattr(extent_protocol::extentid_t id, extent_protocol::attr &);
  int remove(extent_protocol::extentid_t id, int &);
};

#endif 







