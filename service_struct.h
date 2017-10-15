#ifndef SERVICE_STRUCT_H_
#define SERVICE_STRUCT_H_
#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <pthread.h>
#include <boost/thread/mutex.hpp>
#include <string>

typedef struct conn_queue_item CQ_ITEM;
typedef struct conn_queue CQ;
enum conn_states
{
    conn_listening,
    conn_waiting,
    conn_read,
    conn_write,
    conn_closing,
    conn_closed,
    conn_max_state
};

struct conn_queue_item
{
    int sfd;
    enum conn_states  init_state;
    int event_flags;
    int read_buffer_size;
    CQ_ITEM *next;
};

struct conn_queue
{
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

struct thread_stats {
    pthread_mutex_t   mutex;
    uint64_t          bytes_read;
    uint64_t          bytes_written;
    uint64_t          conn_yields; /* # of yields for connections (-R option)*/
};
typedef struct
{
    pthread_t thread_id;
    struct event_base *base;
    struct event notify_event;
    int notify_receive_fd;
    int notify_send_fd;
    struct thread_stats stats;
    struct conn_queue *new_conn_queue;
}LIBEVENT_THREAD;

typedef struct {
    pthread_t thread_id;        /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
} LIBEVENT_DISPATCHER_THREAD;


typedef struct conn conn;
struct conn {
    int sfd;
    enum conn_states state;
    struct event event;
    short  ev_flags;
    short  which;   /** which events were just triggered */

    char   *rbuf;   /** buffer to read commands into */
    char   *rcurr;  /** but if we parsed some already, this is where we stopped */
    int    rsize;   /** total allocated size of rbuf */
    int    rbytes;  /** how much data, starting from rcur, do we have unparsed */

    char   *wbuf;
    char   *wcurr;
    int    wsize;
    int    wbytes;


    //enum conn_states  write_and_go;

    conn   *next;
    LIBEVENT_THREAD *thread;
};

struct stats {
    pthread_mutex_t mutex;
    unsigned int  curr_items;
    unsigned int  total_items;
    uint64_t      curr_bytes;
    unsigned int  curr_conns;
    unsigned int  total_conns;
    uint64_t      rejected_conns;
    uint64_t      malloc_fails;
    unsigned int  reserved_fds;
    unsigned int  conn_structs;
    bool accepting_conns;
    uint64_t listen_disabled_num;
};

struct settings{
    int thread_num;
    std::string ports;
    int backlog;
    int headroom;
    int maxconns;
    int verbose;
};

#define DATA_BUFFER_SIZE 2048

#endif
