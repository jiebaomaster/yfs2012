#include "extent_client.h"

using std::string;

extent_client_cache::extent_client_cache(string dst) : extent_client(dst) {
  pthread_mutex_init(&extent_mutex, NULL);
}

/**
 * @brief 获取文件
 * 
 * @param eid 
 * @param buf 
 * @return extent_protocol::status 
 */
extent_protocol::status
extent_client_cache::get(extent_protocol::extentid_t eid, string &buf) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock _m(&extent_mutex);
  
  auto iter = file_cached.find(eid);
  if (iter != file_cached.end()) {
    extent &extent = iter->second;
    switch (extent.state) {
      case NONE:
        ret = cl->call(extent_protocol::get, eid, buf);
        if (ret == extent_protocol::OK) {
          extent.data = buf;
          extent.state = UPDATED;
          extent.attr.atime = time(NULL);
          extent.attr.size = buf.size();
        }
        break;
      case UPDATED:
      case MODIFIED: // 本地缓存的文件可以直接获取，不需要请求文件服务器
        buf = extent.data;
        extent.attr.atime = time(NULL); // 更新访问时间
        break;
      case REMOVED:
      default: // 访问已被删除的文件，返回错误
        ret = extent_protocol::NOENT;
        break;
    }
  } else { // 第一次获取文件，需要请求文件服务器
    ret = cl->call(extent_protocol::get, eid, buf);
    if (ret == extent_protocol::OK) {
      file_cached[eid].data = buf;
      file_cached[eid].state = UPDATED;
      file_cached[eid].attr.atime = time(NULL);
      file_cached[eid].attr.size = buf.size();
      file_cached[eid].attr.ctime = 0;
      file_cached[eid].attr.mtime = 0;
    }
  }

  return ret;
}

/**
 * @brief 获取文件的属性
 * 
 * @param eid 
 * @param a 
 * @return extent_protocol::status 
 */
extent_protocol::status 
extent_client_cache::getattr(extent_protocol::extentid_t eid, extent_protocol::attr &attr) {
  extent_protocol::status ret = extent_protocol::OK;
  extent_protocol::attr tmp;
  ScopedLock _m(&extent_mutex);
  auto iter = file_cached.find(eid);
  if (iter != file_cached.end()) {
    extent &extent = iter->second;
    switch (extent.state) {
      case NONE:
      case UPDATED:
      case MODIFIED:
        if (!extent.attr.atime || !extent.attr.ctime || !extent.attr.mtime) {
          ret = cl->call(extent_protocol::getattr, eid, tmp);
          if (ret == extent_protocol::OK) {
            // 本地的时间可能比远程的更新
            if (!extent.attr.atime) extent.attr.atime = tmp.atime;
            if (!extent.attr.ctime) extent.attr.ctime = tmp.ctime;
            if (!extent.attr.mtime) extent.attr.mtime = tmp.mtime;
            if (extent.state == NONE) extent.attr.size = tmp.size;
          }
        }
        attr = extent.attr; // 
        break;
      case REMOVED:
      default:
        ret = extent_protocol::NOENT;
        break;
    }
  } else {
    ret = cl->call(extent_protocol::getattr, eid, tmp);
    if (ret == extent_protocol::OK) {
      file_cached[eid].state = NONE; // 文件仅获取属性，此时并没有相关缓存
      file_cached[eid].attr = tmp;
      attr = file_cached[eid].attr;
    }
  }

  return ret;
}

extent_protocol::status 
extent_client_cache::put(
  extent_protocol::extentid_t eid, std::string buf) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock _m(&extent_mutex);

  auto iter = file_cached.find(eid);
  if (iter != file_cached.end()) {
    extent &extent = iter->second;
    switch (extent.state) {
      case NONE:
      case UPDATED:
      case MODIFIED:
        extent.data = buf;
        extent.state = MODIFIED;
        extent.attr.size = buf.size();
        extent.attr.mtime = time(NULL);
        extent.attr.ctime = time(NULL);
        break;
      case REMOVED:  // 不能修改已删除的文件，报错
      default:
        ret = extent_protocol::NOENT;
        break;
    }
  } else {
    file_cached[eid].data = buf;
    file_cached[eid].state = MODIFIED;
    file_cached[eid].attr.size = buf.size();
    file_cached[eid].attr.mtime = time(NULL);
    file_cached[eid].attr.atime = time(NULL);
    file_cached[eid].attr.ctime = time(NULL);
  }

  return ret;
}

extent_protocol::status 
extent_client_cache::remove(extent_protocol::extentid_t eid) {
  extent_protocol::status ret = extent_protocol::OK;
  ScopedLock _m(&extent_mutex);
  
  switch (file_cached[eid].state) {
    case NONE:
    case UPDATED:
    case MODIFIED:
      // 删除一个文件只需标记文件被删除了，真正删除在 flush 时
      file_cached[eid].state = REMOVED;
      break;
    case REMOVED: // 文件不能重复删除，报错
    default:
      ret = extent_protocol::NOENT;
      break;
  }

  return ret;
}


extent_protocol::status
extent_client_cache::flush(extent_protocol::extentid_t eid) {
  extent_protocol::status ret = extent_protocol::OK;
  int r;
  ScopedLock _m(&extent_mutex);

  auto iter = file_cached.find(eid);
  if (iter != file_cached.end()) {
    extent &extent = iter->second;
    
    switch (extent.state) {
      case NONE: // 本地不存在的文件不用刷新
      case UPDATED: // 已经是最新的文件不用刷新
        break;
      case MODIFIED: // 已修改的文件将修改后内容提交到文件服务器
        ret = cl->call(extent_protocol::put, eid, extent.data, r);
        break;
      case REMOVED: // 被删除的文件请求文件服务器正式删除
        ret = cl->call(extent_protocol::remove, eid);
        break;
    }
    // 文件刷新后本地缓存也删除，下次用到时重新从文件服务器获取，保证文件内容总是最新的
    file_cached.erase(iter);
  } else 
    ret = extent_protocol::NOENT;

  return ret; 
}
