#ifndef __SCOPED_LOCK__
#define __SCOPED_LOCK__

#include <pthread.h>
#include "lang/verify.h"
struct ScopedLock {
	private:
		pthread_mutex_t *m_;
	public:
		ScopedLock(pthread_mutex_t *m): m_(m) { // 在构造的时候锁定
			VERIFY(pthread_mutex_lock(m_)==0);
		}
		~ScopedLock() { // 在析构的时候解锁
			VERIFY(pthread_mutex_unlock(m_)==0);
		}
};
#endif  /*__SCOPED_LOCK__*/
