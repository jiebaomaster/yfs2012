#ifndef yfs_client_h
#define yfs_client_h

#include <string>
//#include "yfs_protocol.h"
#include "extent_client.h"
#include <vector>

#include "lock_protocol.h"
#include "lock_client.h"
#include "lock_client_cache.h"

class yfs_client {
  extent_client *ec; // 文件储存服务客户端
  lock_client *lc; // 锁服务客户端

 public:

  typedef unsigned long long inum;
  enum xxstatus { OK, RPCERR, NOENT, IOERR, EXIST };
  typedef int status;

  struct fileinfo { // 文件信息
    unsigned long long size;
    unsigned long atime;
    unsigned long mtime;
    unsigned long ctime;
  };
  struct dirinfo { // 目录信息
    unsigned long atime;
    unsigned long mtime;
    unsigned long ctime;
  };
  struct dirent { // 目录项
    std::string name; // 文件/目录 名
    yfs_client::inum inum; // 标识文件/目录的唯一 id
                    // 高 32 位为 0，文件 31 位为 1，目录 31 位为 0
  };

 private:
  static std::string filename(inum);
  static inum n2i(std::string);
 public:

  yfs_client(std::string, std::string);
  ~yfs_client();

  bool isfile(inum);
  bool isdir(inum);

  int getfile(inum, fileinfo &);
  int getdir(inum, dirinfo &);
  int random_inum(bool);
  int create(inum, const char*, inum&);
  int lookup(inum, const char*, inum&, bool*);
  int readdir(inum, std::list<dirent>&);
  int setattr(inum, struct stat*);
  int read(inum, off_t, size_t, std::string&);
  int write(inum, off_t, size_t, const char*);
  int mkdir(inum, const char*, mode_t, inum&);
  int unlink(inum, const char*);
};

// 分布式锁的辅助类
class yfs_lock {
  lock_client *lc; // 锁服务客户端
  lock_protocol::lockid_t lid; // 本次锁定的锁 id

 public:
  // 构造时锁定
  yfs_lock(lock_client *lc, lock_protocol::lockid_t lid) : lc(lc), lid(lid) {
    lc->acquire(lid);
  }
  // 析构时释放
  ~yfs_lock() { lc->release(lid); }
};
#endif 
