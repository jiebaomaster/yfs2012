// the extent server implementation

#include "extent_server.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sstream>

extent_server::extent_server() {
  pthread_mutex_init(&map_mutex, NULL);
  int ret;
  // 系统启动后，需要创建一个空的 root 目录
  put(1, "", ret);
}

int extent_server::put(extent_protocol::extentid_t id, std::string buf, int &) {
  ScopedLock _l(&map_mutex);

  extent_protocol::attr attr;
  attr.atime = attr.ctime = attr.mtime = time(NULL);
  if (file_map.find(id) != file_map.end()) {
    // 已经存在的文件，不用修改创建时间
    attr.ctime = file_map[id].attr.atime;
  }
  attr.size = buf.size();
  file_map[id].data = buf;
  file_map[id].attr = attr;

  return extent_protocol::OK;
}

int extent_server::get(extent_protocol::extentid_t id, std::string &buf) {
  ScopedLock _l(&map_mutex);

  if (file_map.find(id) != file_map.end()) {
    file_map[id].attr.atime = time(NULL);
    buf = file_map[id].data;
    return extent_protocol::OK;
  }
  return extent_protocol::NOENT;
}

int extent_server::getattr(extent_protocol::extentid_t id,
                           extent_protocol::attr &a) {
  // You fill this in for Lab 2.
  // You replace this with a real implementation. We send a phony response
  // for now because it's difficult to get FUSE to do anything (including
  // unmount) if getattr fails.
  ScopedLock _l(&map_mutex);

  if (file_map.find(id) != file_map.end()) {
    a = file_map[id].attr;
    return extent_protocol::OK;
  }
  return extent_protocol::NOENT;
}

int extent_server::remove(extent_protocol::extentid_t id, int &) {
  // You fill this in for Lab 2.
  ScopedLock _l(&map_mutex);

  auto iter = file_map.find(id);
  if (iter != file_map.end()) {
    file_map.erase(iter);
    return extent_protocol::OK;
  }
  return extent_protocol::NOENT;
}
