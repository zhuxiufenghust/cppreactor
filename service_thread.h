#ifndef SERVICE_THREAD_H_
#include "service_struct.h"
#include <boost/function.hpp>

void setup_thread(uint32_t thread_num, boost::function<void()> worker_func);
void worker_func();

void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                                              int read_buffer_size);

void worker_thread_init(int nthreads, struct event_base *main_base);

void STATS_LOCK();
void STATS_UNLOCK();

#endif
