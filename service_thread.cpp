#include "service_thread.h"
#include "service_main.h"
#include "service_struct.h"
#include "service_log.h"

#include <iostream>

#include <event2/event.h>
#include <event2/util.h>
#include <event2/event_compat.h>


#define ITEMS_PER_ALLOC 64

/* Free list of CQ_ITEM structs */
static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;
static int last_thread = -1;
static LIBEVENT_THREAD *threads;
static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;


static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;
static pthread_mutex_t worker_hang_lock;

static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;


using namespace std;

void STATS_LOCK() {
        pthread_mutex_lock(&stats_lock);
}

void STATS_UNLOCK() {
        pthread_mutex_unlock(&stats_lock);
}

static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}
static CQ_ITEM* cqi_new()
{
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist)
    {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);
    if(item == NULL)
    {
        int i;

        item = (CQ_ITEM*)malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item)
        {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            return NULL;
        }
        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }
    return item;
}


static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
    pthread_mutex_lock(&worker_hang_lock);
    pthread_mutex_unlock(&worker_hang_lock);
}

static void thread_libevent_process(int fd, short which, void *arg) {
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD*)arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        if (settings.verbose > 0)
            ERROR_LOG << "Can't read from libevent pipe" ;
    switch (buf[0]) {
        case 'c':
            ERROR_LOG << "new conn in work" ;
            item = cq_pop(me->new_conn_queue);
            if (NULL != item)
            {
                conn *c = conn_new(item->sfd, item->init_state, item->event_flags,
                                   item->read_buffer_size, me->base);
                if (c == NULL)
                {
                    if (settings.verbose > 0)
                    {
                        ERROR_LOG << "Can't listen for events on fd " <<  item->sfd ;

                    }
                    close(item->sfd);
                }
                else
                {
                    c->thread = me;
                }
                cqi_free(item);
            }
            break;

            /* we were told to pause and report in */
        case 'p':
            register_thread_initialized();
            break;
    }
}

static void setup_thread(LIBEVENT_THREAD *me)
{
    me->base = event_init();
    if (!me->base) {
        ERROR_LOG << "Can't allocate event base";
        exit(1);
    }

    event_set(&me->notify_event, me->notify_receive_fd,
              EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        ERROR_LOG << "Can't monitor libevent notify pipe";
        exit(1);
    }

    me->new_conn_queue = (struct conn_queue*)malloc(sizeof(struct conn_queue));
    if (me->new_conn_queue == NULL) {
        ERROR_LOG << "Failed to allocate memory for connection queue";
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);

    if (pthread_mutex_init(&me->stats.mutex, NULL) != 0) {
        ERROR_LOG << "Failed to initialize mutex";
        exit(EXIT_FAILURE);
    }

}

static void create_worker(void *(*func)(void *), void *arg) {
    pthread_t       thread;
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&thread, &attr, func, arg)) != 0) {
         ERROR_LOG << "Can't create thread: " << strerror(ret);
         exit(1);
    }
}


static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD*)arg;

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}



/* FIXME */
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                                              int read_buffer_size)
{
    std::cout << "new conn " << sfd << std::endl;

    CQ_ITEM* item = cqi_new();
    char buf[1];
    if (item == NULL) {
        close(sfd);
        ERROR_LOG << "Failed to allocate memory for connection object" ;
        return ;
    }
    int tid = (last_thread + 1) % settings.thread_num;

    LIBEVENT_THREAD *thread = threads + tid;
    last_thread = tid;


    item->sfd = sfd;
    item->init_state = init_state;
    item->event_flags = event_flags;
    item->read_buffer_size = read_buffer_size;

    cq_push(thread->new_conn_queue, item);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        ERROR_LOG << "Writing to thread notify pipe" ;
    }
}

static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

void worker_thread_init(int nthreads, struct event_base *main_base)
{
    pthread_mutex_init(&worker_hang_lock, NULL);

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;


    threads = (LIBEVENT_THREAD*)malloc(nthreads * sizeof(LIBEVENT_THREAD));
    if (!threads) {
        ERROR_LOG << "Can't allocate thread descriptors";
        exit(1);
    }

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    for(int i = 0; i < nthreads; i++) {
        int fds[2];
        if (pipe(fds)) {
            ERROR_LOG << "Can't create notify pipe";
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
        stats.reserved_fds += 5;
    }

    /* Create threads after we've done all the libevent setup. */
    for(int i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i]);
    }

    /* Wait for all the threads to set themselves up before returning.
     * */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);

    std::cout << "thread init_count " << init_count << std::endl;


}




