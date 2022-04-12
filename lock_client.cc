// RPC stubs for clients to talk to lock_server

#include "lock_client.h"
#include "rpc.h"
#include <arpa/inet.h>

#include <sstream>
#include <iostream>
#include <stdio.h>

lock_client::lock_client(std::string dst)
{
  sockaddr_in dstsock;
  make_sockaddr(dst.c_str(), &dstsock); // 建立 socket 连接
  cl = new rpcc(dstsock); // 新建一个 rpc 客户端
  if (cl->bind() < 0) {
    printf("lock_client: call bind\n");
  }
  
}

/**
 * @brief 查询 lid 号锁的状态
 * 
 * @param lid 锁 id
 * @return int 
 */
int
lock_client::stat(lock_protocol::lockid_t lid)
{
  int r;
  // 使用 rpc 客户端调用 rpc 服务端的 stat 方法，传递参数（客户端 id，锁 id），获得返回值 r
  lock_protocol::status ret = cl->call(lock_protocol::stat, cl->id(), lid, r);
  VERIFY (ret == lock_protocol::OK); // 检查调用 rpc 之后，状态是否正常
  return r;
}

lock_protocol::status
lock_client::acquire(lock_protocol::lockid_t lid)
{
  int r;
  // 使用 rpc 客户端调用 rpc 服务端的 stat 方法，传递参数（客户端 id，锁 id），获得返回值 r
  lock_protocol::status ret = cl->call(lock_protocol::acquire, cl->id(), lid, r);
  VERIFY (ret == lock_protocol::OK); // 检查调用 rpc 之后，状态是否正常
  return r;
}

lock_protocol::status
lock_client::release(lock_protocol::lockid_t lid)
{
  int r;
  // 使用 rpc 客户端调用 rpc 服务端的 stat 方法，传递参数（客户端 id，锁 id），获得返回值 r
  lock_protocol::status ret = cl->call(lock_protocol::release, cl->id(), lid, r);
  VERIFY (ret == lock_protocol::OK); // 检查调用 rpc 之后，状态是否正常
  return r;
}

