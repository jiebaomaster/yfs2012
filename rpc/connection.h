#ifndef connection_h
#define connection_h 1

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstddef>

#include <map>

#include "pollmgr.h"

class connection;

class chanmgr {
	public:
		virtual bool got_pdu(connection *c, char *b, int sz) = 0;
		virtual ~chanmgr() {}
};

class connection : public aio_callback {
	public:
		struct charbuf {
			charbuf(): buf(NULL), sz(0), solong(0) {}
			charbuf (char *b, int s) : buf(b), sz(s), solong(0){}
			char *buf; // 缓冲区，每次有新消息都重新分配
			int sz; // 缓冲区大小
			int solong; // 已经使用的缓冲区大小
		};

		connection(chanmgr *m1, int f1, int lossytest=0);
		~connection();

		int channo() { return fd_; }
		bool isdead();
		void closeconn();
		// 发送缓冲区 b 中的数据
		bool send(char *b, int sz);
		// 本链接注册在事件循环中的回调函数
		void write_cb(int s);
		void read_cb(int s);

		void incref();
		void decref();
		int ref();
                
                int compare(connection *another);
	private:

		bool readpdu();
		bool writepdu();

		chanmgr *mgr_; // 所属事件循环
		const int fd_;
		bool dead_;

		charbuf wpdu_; // 写缓冲区
		charbuf rpdu_; // 读缓冲区
                
                struct timeval create_time_;

		int waiters_;
		int refno_;
		const int lossy_;

		pthread_mutex_t m_;
		pthread_mutex_t ref_m_;
		pthread_cond_t send_complete_;
		pthread_cond_t send_wait_;
};

// 用来监听新链接的 tcp 套接字
class tcpsconn {
	public:
		tcpsconn(chanmgr *m1, int port, int lossytest=0);
		~tcpsconn();
                inline int port() { return port_; }
		void accept_conn();
	private:
                int port_;
		pthread_mutex_t m_;
		pthread_t th_;
		int pipe_[2]; // 用来监听关闭
		int tcp_; // 用来监听新连接
		chanmgr *mgr_;
		int lossy_;
		std::map<int, connection *> conns_;

		void process_accept();
};

struct bundle {
	bundle(chanmgr *m, int s, int l):mgr(m),tcp(s),lossy(l) {}
	chanmgr *mgr;
	int tcp;
	int lossy;
};

void start_accept_thread(chanmgr *mgr, int port, pthread_t *th, int *fd = NULL, int lossy=0);
connection *connect_to_dst(const sockaddr_in &dst, chanmgr *mgr, int lossy=0);
#endif
