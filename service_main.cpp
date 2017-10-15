#include "service_struct.h"
#include "service_thread.h"
#include "service_main.h"
#include "service_log.h"

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <vector>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include  <netinet/in.h>
#include  <netinet/tcp.h>
#include <iostream>
#include <string>
#include <fstream>

#include <event2/event.h>
#include <event2/util.h>
#include <event2/event_compat.h>

using namespace std;
struct settings  settings;

static struct event_base *main_base;
static conn* listen_conn = NULL;
static conn** conns;
static int max_fds;
struct stats stats;

static volatile bool allow_new_conns = true;

pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;

static void drive_machine(conn *c);
void event_handler(const int fd, const short which, void *arg);
static void conn_set_state(conn *c, enum conn_states state);
static bool update_event(conn *c, const int new_flags);
void accept_new_conns(const bool do_accept);



static const char *state_text(enum conn_states state) {
    const char* const statenames[] = {
        "conn_listening",
        "conn_waiting",
        "conn_read",
        "conn_write",
        "conn_closing",
        "conn_closed",
    };
    return statenames[state];
}

enum try_read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,            /** an error occurred (on the socket) (or client closed connection) */
    READ_MEMORY_ERROR      /** failed to allocate more memory */
};

#define INIT_SET(name, type) settings.name= service_map[#name].as<type>()

static struct event maxconnsevent;
static void maxconns_handler(const int fd, const short which, void *arg) {
    struct timeval t = {0, 10000};

    if (fd == -42 || allow_new_conns == false) {
        evtimer_set(&maxconnsevent, maxconns_handler, 0);
        event_base_set(main_base, &maxconnsevent);
        evtimer_add(&maxconnsevent, &t);
    } else {
        evtimer_del(&maxconnsevent);
        accept_new_conns(true);
    }
}

static void out_string(conn *c, const char *str)
{
    size_t len;
    assert(c != NULL);
    if (settings.verbose > 1)
        INFO_LOG << c->sfd << str;

    len = strlen(str);
    if((int)(len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity
         * */
        len = strlen(str);
    }
    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    return;
}

static void out_of_memory(conn *c, const char *ascii_error) {
    //const static char error_prefix[] = "SERVER_ERROR ";
    //const static int error_prefix_len = sizeof(error_prefix) - 1;
    out_string(c, ascii_error);
}

static enum try_read_result try_read_network(conn *c) {
    enum try_read_result gotdata = READ_NO_DATA_RECEIVED;
    int res;
    int num_allocs = 0;
    assert(c != NULL);

    while (1) {
        if (c->rbytes >= c->rsize) {
            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;
            char *new_rbuf = (char*)realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                STATS_LOCK();
                stats.malloc_fails++;
                STATS_UNLOCK();
                if (settings.verbose > 0) {
                    ERROR_LOG << "Couldn't realloc input buffer";
                }
                c->rbytes = 0; /* ignore what we read */
                out_of_memory(c, "SERVER_ERROR out of memory reading request");
                //c->write_and_go = conn_closing;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        int avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            pthread_mutex_lock(&c->thread->stats.mutex);
            c->thread->stats.bytes_read += res;
            pthread_mutex_unlock(&c->thread->stats.mutex);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            return READ_ERROR;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return READ_ERROR;
        }
    }
    return gotdata;
}

static void reset_cmd_handler(conn *c) {
        c->rcurr = c->rbuf;
        c->rbytes = 0;
        c->wcurr = c->wbuf;
        c->wbytes = 0;
        memset(c->rbuf, 0, c->rsize);
        memset(c->wbuf, 0, c->wsize);
        conn_set_state(c, conn_waiting);
}

static void conn_set_state(conn *c, enum conn_states state) {
    assert(c != NULL);
    assert(state >= conn_listening && state < conn_max_state);

    if (state != c->state) {
        if (settings.verbose > 2) {
            INFO_LOG << c->sfd << " going from " << state_text(c->state)
                     << " to " <<state_text(state);
        }

        c->state = state;
    }
}

static void conn_release_items(conn *c) {
    assert(c != NULL);
}


static void conn_cleanup(conn *c) {
    assert(c != NULL);
    conn_release_items(c);
}

static void conn_close(conn *c) {
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (settings.verbose > 1)
        INFO_LOG << c->sfd << " connection closed";

    conn_cleanup(c);

    conn_set_state(c, conn_closed);
    close(c->sfd);

    pthread_mutex_lock(&conn_lock);
    allow_new_conns = true;
    pthread_mutex_unlock(&conn_lock);

    STATS_LOCK();
    stats.curr_conns--;
    STATS_UNLOCK();

    return;
}

static void stats_init(void)
{
    stats.curr_items = 0;
    stats.total_items = 0;
    stats.curr_bytes = 0;
    stats.curr_conns = 0;
    stats.total_conns = 0;
    stats.rejected_conns = 0;
    stats.malloc_fails = 0;
    stats.reserved_fds = 0;
    stats.conn_structs = 0;
}

void accept_new_conns(const bool do_accept)
{
    conn* next;
    for(next = listen_conn; next; next = next->next)
    {
        if(do_accept)
        {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, settings.backlog) != 0)
            {
                ERROR_LOG << " listen ";
            }
        }
        else
        {
            update_event(next, 0);
            if(listen(next->sfd, 0) != 0)
            {
                ERROR_LOG << " listen ";
            }
        }

    }
    if(do_accept)
    {
        STATS_LOCK();
        stats.accepting_conns = true;
        STATS_UNLOCK();
    }
    else
    {
        STATS_LOCK();
        stats.accepting_conns = false;
        stats.listen_disabled_num++;
        STATS_UNLOCK();
        allow_new_conns = false;
        //maxconns_handler(-42, 0, 0);
    }
}

static void drive_machine(conn *c)
{
    bool stop = false;
    struct sockaddr_in addr;
    socklen_t addrlen;
    const char *str;
    int res = 0;
    int sfd;
    while(!stop)
    {
        switch(c->state)
        {
            case conn_listening:
                addrlen = sizeof(addr);
                sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
                if(sfd == -1)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        stop = true;
                    }
                    else if(errno == EMFILE)
                    {
                        if (settings.verbose > 0)
                            ERROR_LOG << "Too many open connections" ;
                         accept_new_conns(false);
                         stop = true;
                    }
                }
                else
                {
                    evutil_make_socket_nonblocking(sfd);
                    if(stats.curr_conns + stats.reserved_fds >= (unsigned int)(settings.maxconns - 1))
                    {
                        str = "ERROR Too many open connections\r\n";
                        res = write(sfd, str, strlen(str));
                        close(sfd);
                        STATS_LOCK();
                        stats.rejected_conns++;
                        STATS_UNLOCK();
                    }
                    else
                    {
                        dispatch_conn_new(sfd, conn_waiting, EV_READ | EV_PERSIST,
                                          DATA_BUFFER_SIZE);
                    }
                }

                stop = true;
                break;

            case conn_waiting:
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                         ERROR_LOG << "Couldn't update event";
                    conn_set_state(c, conn_closing);
                    break;
                }

                conn_set_state(c, conn_read);
                stop = true;
                break;

            case conn_read:
                reset_cmd_handler(c);
                INFO_LOG << "conn_read";
                res = try_read_network(c);
                switch (res) {
                    case READ_NO_DATA_RECEIVED:
                        conn_set_state(c, conn_waiting);
                        break;
                    case READ_DATA_RECEIVED:
                        out_string(c, c->rbuf);
                        update_event(c, EV_WRITE | EV_PERSIST);
                        break;
                    case READ_ERROR:
                        conn_set_state(c, conn_closing);
                        break;
                    case READ_MEMORY_ERROR: /* Failed to allocate more memory */
                        break;
                }
                break;
            case conn_write:
                INFO_LOG << "conn_write" << std::endl;
                {
                    int to_write = c->wbytes;
                    int has_write = 0;
                    int wr_bytes = 0;
                    while(has_write < to_write)
                    {
                        wr_bytes = write(c->sfd, c->wbuf + has_write, to_write);
                        if(wr_bytes >= 0)
                        {
                            to_write -= wr_bytes;
                            has_write += wr_bytes;
                        }
                        else
                        {
                            reset_cmd_handler(c);
                            conn_set_state(c, conn_closing);
                        }
                    }
                    if(has_write == c->wbytes)
                    {
                        conn_set_state(c, conn_read);
                        reset_cmd_handler(c);
                        update_event(c, EV_READ|EV_WRITE);
                    }
                    else
                    {
                        WARN_LOG << "to_write:" << to_write << " has_write:"
                            << has_write ;
                    }
                    stop = true;
                    break;
                }
            case conn_closing:
                INFO_LOG << "conn_closing" << c->thread->stats.bytes_read ;
                conn_close(c);
                stop = true;
                break;
            case conn_closed:
                INFO_LOG << "conn_closed ";
                break;
            default:
                break;

        }

    }

}

static bool update_event(conn *c, const int new_flags) {
    assert(c != NULL);

    struct event_base *base = c->event.ev_base;
    return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}


void event_handler(const int fd, const short which, void *arg)
{
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose> 0)
             INFO_LOG << "Catastrophic: event fd doesn't match conn fd!";
        conn_close(c);
    }

    drive_machine(c);

    /* wait for next event */
    return;
}
void conn_free(conn *c)
{
    if (c)
    {
        assert(c != NULL);
        assert(c->sfd >= 0 && c->sfd < max_fds);

        conns[c->sfd] = NULL;
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        free(c);
    }
}

conn* conn_new(int sfd, enum conn_states init_state,
               const int  event_flags, const int read_buffer_size,
               struct event_base *base)
{
    assert(sfd >= 0 && sfd < max_fds);
    conn *c = conns[sfd];;
    if(c == NULL)
    {
        c = (conn*)malloc(sizeof(conn));
        if(c == NULL)
        {
            STATS_LOCK();
            stats.malloc_fails++;
            STATS_UNLOCK();
            ERROR_LOG << "Failed to allocate connection object";
            return NULL;
        }
    }
    c->rbuf = c->wbuf = 0;

    c->rsize = read_buffer_size;
    c->wsize = DATA_BUFFER_SIZE;

    c->rbuf = (char *)malloc((size_t)c->rsize);
    c->wbuf = (char *)malloc((size_t)c->wsize);

    if (c->rbuf == 0 || c->wbuf == 0)
    {
        conn_free(c);
        STATS_LOCK();
        stats.malloc_fails++;
        STATS_UNLOCK();
        ERROR_LOG << "Failed to allocate buffers for connection";
        return NULL;
    }
    STATS_LOCK();
    stats.conn_structs++;
    STATS_UNLOCK();

    c->sfd = sfd;
    conns[sfd] = c;

    if (init_state == conn_listening) {
        ERROR_LOG << sfd << " server listening \n";
    }
    c->state = init_state;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }

    STATS_LOCK();
    stats.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();

    return c;
}

static void conn_init(void) {
    /* We're unlikely to see an FD much higher than maxconns. */
    /* account for extra unexpected open FDs */
    int next_fd = dup(1);
    struct rlimit rl;

    max_fds = settings.maxconns + settings.headroom + next_fd;

    /* But if possible, get the actual highest FD we can
     * possibly ever see. */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        max_fds = rl.rlim_max;
    } else {
        ERROR_LOG << "Failed to query maximum file descriptor; "
                << "falling back to maxconns";
    }

    close(next_fd);

    if ((conns = (conn**)calloc(max_fds, sizeof(conn *))) == NULL) {
        ERROR_LOG << "Failed to allocate connection structures";
        /* This is unrecoverable so bail
         * out early. */
        exit(1);
    }
    std::cout << "max_fds " << max_fds << std::endl;
}

int listen_socket(int port)
{
    int sfd;
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    int ret = evutil_make_listen_socket_reuseable(sfd);
    if(ret)
    {
        std::cout << "make socket reusable "<< std::endl;
        close(sfd);
        return -1;
    }
    ret = evutil_make_socket_nonblocking(sfd);
    if(ret)
    {
        std::cout << "make socket noblock " << std::endl;
        close(sfd);
        return -1;
    }

    //other options
    int flags =1;
    struct linger ling = {0, 0};
    ret = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    if(ret)
    {
        close(sfd);
        return -1;
    }
    ret = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
    if(ret)
    {
        close(sfd);
        return -1;
    }
    ret = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    if(ret)
    {
        close(sfd);
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    ret = bind(sfd, (struct sockaddr*) &addr, sizeof(addr));
    if(ret)
    {
        close(sfd);
        return -1;
    }
    ret = listen(sfd, settings.backlog);
    if(ret)
    {
        close(sfd);
        return -1;
    }

    return sfd;
}

int server_socket(int port)
{
    int sfd = listen_socket(port);
    if(sfd == -1)
    {
        ERROR_LOG << "create listen socket on port " << port ;
        return 0; //failed
    }
    conn* listen_conn_add;
    listen_conn_add = conn_new(sfd, conn_listening,
                               EV_READ|EV_PERSIST, DATA_BUFFER_SIZE, main_base);
    if(!listen_conn_add)
    {
        ERROR_LOG << "create listen conn failed " ;
        return 1; //failed
    }

    listen_conn_add->next = listen_conn;
    listen_conn = listen_conn_add;

    return 1;
}

void server_sockets(const string& port_list)
{
    vector<string> port_vec;
    boost::algorithm::split(port_vec, port_list, boost::is_any_of(","));
    for(int i = 0; i < (int)port_vec.size(); i++)
    {
        int port = boost::lexical_cast<int>(port_vec[i]);
        std::cout << "listen " << port << std::endl;
        /*int success =*/ server_socket(port);
    }


}


int parse_config(const std::string& file_name)
{
    std::cout << "config " << file_name << std::endl;
    using namespace boost::program_options;
    boost::program_options::variables_map service_map;

    options_description cfg_opt;
    cfg_opt.add_options()
        ("thread_num", value<int>()->default_value(1), "worker thread number")
        ("ports", value<std::string>()->default_value("8080"), "service listen port like 80,8080")
        ("backlog", value<int>()->default_value(100), "max listen num")
        ("headroom", value<int>()->default_value(10), "fd headroom")
        ("maxconns", value<int>()->default_value(1000), "max conntion num")
        ("verbose", value<int>()->default_value(0), "verbose");

    store(parse_config_file<char>(file_name.c_str(), cfg_opt), service_map);

    INIT_SET(thread_num, int);
    INIT_SET(ports, string);
    INIT_SET(backlog, int);
    INIT_SET(headroom, int);
    INIT_SET(maxconns, int);
    INIT_SET(verbose, int);

    return 0;
}

int main(int argc, char** argv)
{
    initlog();
    INFO_LOG << "test";
    using namespace logging::trivial;
    using namespace boost::program_options;
    options_description cmdline_opt;
    variables_map cmdline_vm;
    cmdline_opt.add_options()("config,c", value<std::string>(), "config file path")
        ("help,h", "print help message");
    store(parse_command_line(argc, argv, cmdline_opt), cmdline_vm);
    notify(cmdline_vm);
    if(cmdline_vm.count("help"))
    {
        std::cout << cmdline_opt << std::endl;
        return 0;
    }

    std::string file_name;
    if(cmdline_vm.count("config"))
    {
        file_name = cmdline_vm["config"].as<std::string>();
        parse_config(file_name);
    }
    else
    {
        std::cout << cmdline_opt << std::endl;
        return 0;
    }

    std::cout << "test" << std::endl;


    conn_init();
    stats_init();
    //init libevent event_base
    main_base = event_base_new();
    server_sockets(settings.ports);

    worker_thread_init(settings.thread_num, main_base);
    if (event_base_loop(main_base, 0) != 0) {
        return EXIT_FAILURE;
    }
    return 0;
}
