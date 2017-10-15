#include "service_log.h"

boost::shared_ptr<log_type> my_logger;


void initlog()
{
    typedef sinks::synchronous_sink<sinks::text_file_backend> text_sink;
    boost::shared_ptr<text_sink> sink  = logging::add_file_log
    (
            keywords::open_mode = std::ios::app,
            keywords::file_name = "log_%Y%m%d%H.log",
            keywords::max_size = 1024 * 1024 * 1024,
            keywords::time_based_rotation=sinks::file::rotation_at_time_point(1,0,0),
            keywords::format =
            (
                expr::stream
                << expr::format_date_time< boost::posix_time::ptime >("TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                << "[" << expr::attr<boost::log::attributes::current_thread_id::value_type >("ThreadID")
                << "] <" << logging::trivial::severity
                << "> " << expr::smessage
            )
    );
    sink->locked_backend()->auto_flush(true);
    BOOST_LOG_FUNCTION();
    logging::add_common_attributes();
    BOOST_LOG_SCOPED_THREAD_TAG("ThreadID", boost::this_thread::get_id());
    my_logger = boost::make_shared<log_type>();
}

