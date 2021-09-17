// yfs client.  implements FS operations using extent and lock server
#include "yfs_client.h"
#include "extent_client.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

using std::string;

yfs_client::yfs_client(std::string extent_dst, std::string lock_dst)
{
  ec = new extent_client(extent_dst);

}

/**
 * @brief 将 string 转换为 inum
 * 
 * @param n 
 * @return yfs_client::inum 
 */
yfs_client::inum
yfs_client::n2i(std::string n)
{
  std::istringstream ist(n);
  unsigned long long finum;
  ist >> finum;
  return finum;
}

/**
 * @brief 将 inum 转换为 string
 * 
 * @param inum 
 * @return std::string 
 */
std::string
yfs_client::filename(inum inum)
{
  std::ostringstream ost;
  ost << inum;
  return ost.str();
}

/**
 * @brief 判断一个 inum 是不是文件
 * 
 * @param inum 
 * @return true 
 * @return false 
 */
bool
yfs_client::isfile(inum inum)
{
  if(inum & 0x80000000)
    return true;
  return false;
}

bool
yfs_client::isdir(inum inum)
{
  return ! isfile(inum);
}

int
yfs_client::getfile(inum inum, fileinfo &fin)
{
  int r = OK;

  printf("getfile %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  fin.atime = a.atime;
  fin.mtime = a.mtime;
  fin.ctime = a.ctime;
  fin.size = a.size;
  printf("getfile %016llx -> sz %llu\n", inum, fin.size);

 release:

  return r;
}

int
yfs_client::getdir(inum inum, dirinfo &din)
{
  int r = OK;

  printf("getdir %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }
  din.atime = a.atime;
  din.mtime = a.mtime;
  din.ctime = a.ctime;

 release:
  return r;
}

/**
 * @brief 生成一个随机的 inum
 * 高 32 位为 0，文件的 31 位为 1，目录的 31 位为 0
 * 
 * @param isfile 
 * @return int 
 */
int yfs_client::random_inum(bool isfile) {
  inum ret = (inum)(rand() & 0x7fffffff) | ( isfile << 31 );
  return ret & 0xffffffff;
}

int yfs_client::create(inum parent, const char* name, inum &inum) {
  int r = OK;
  string dir_data;
  string file_name;
  // 调用 get 获取父目录的目录项数据
  if(ec->get(parent, dir_data) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }
  file_name = "/" + string(name) + "/";
  if(dir_data.find(file_name) != string::npos) {
    return EXIST; // 父目录中已经有同名目录项
  }

  // 生成一个随机的 inum 作为新创建文件的 inum
  inum = random_inum(true);
  // 调用 put 创建一个空文件
  if(ec->put(inum, "") != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  // 在父目录中添加目录项
  dir_data.append(file_name + filename(inum) + "/");
  // 调用 put 更新父目录的目录项
  if(ec->put(parent, dir_data) != extent_protocol::OK)
    r = IOERR;

release:
  return r;
}

/**
 * @brief 在父目录 parent 中按名称 name 查找文件
 * 
 * @param parent 父目录
 * @param name 待查找文件的文件名
 * @param inum 目标文件 id
 * @param found 是否查找到目标文件
 * @return int 
 */
int yfs_client::lookup(inum parent, const char *name, inum &inum, bool *found) {
  int r = OK;
  size_t pos, end;
  string dir_data;   // 父目录的数据
  string file_name;  // 当前文件的文件名

  // 调用 get 获取父目录的目录项数据
  if (ec->get(parent, dir_data) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  // 目录中存储的数据为 /name1/inum1//name2/inum2/...
  // 用文件名查找目标文件的目录项
  file_name = "/" + string(name) + "/";
  if ((pos = dir_data.find(file_name)) == string::npos) {
    r = IOERR;
    goto release;
  }

  *found = true;
  pos += file_name.size();
  end = dir_data.find_first_of("/", pos);
  if (end != string::npos) { // 获取目标文件的 inum
    string inum_string = dir_data.substr(pos, end - pos);
    inum = n2i(inum_string);
  } else {
    r = IOERR;
  }

release:
  return r;
}

/**
 * @brief 读取 inum 目录的所有目录项，用一个列表返回
 * 
 * @param inum 目标目录的 id
 * @param dirents 目录项链表
 * @return int 
 */
int yfs_client::readdir(inum inum, std::list<dirent> &dirents) {
  int r = OK;
  string dir_data;
  size_t pos = 0, end;
  
  // 调用 get 获取目标目录的目录项数据
  if(ec->get(inum, dir_data) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  while(pos < dir_data.size()) {
    dirent d;
    // 获取 filename
    pos = dir_data.find("/", pos);
    if(pos == string::npos)
      break;
    end = dir_data.find_first_of("/", pos + 1);
    d.name = dir_data.substr(pos + 1, end - pos - 1);
    pos = end;
    // 获取 inum
    end = dir_data.find_first_of("/", pos + 1);
    d.inum = n2i(dir_data.substr(pos + 1, end - pos -1));
    pos = end + 1;
    // 加入结果集
    dirents.push_back(d);
  }
release:
  return r;
}


