#ifndef SERVICE_MAIN_H_
#define SERVICE_MAIN_H_

#include <boost/program_options.hpp>

extern struct stats stats;

void STATS_LOCK();
void STATS_UNLOCK();

extern boost::program_options::variables_map service_map;

template<typename T> T Get(std::string cfg_key)
{
    if(service_map.find(cfg_key) == service_map.end())
        return T();
    else
        return service_map[cfg_key].as<T>();
}
conn* conn_new(int sfd, enum conn_states init_state,
               const int  event_flags, const int read_buffer_size,
               struct event_base *base);


extern struct settings  settings;
#endif
