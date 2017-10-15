#ifndef SERVICE_LOG_H_
#define SERVICE_LOG_H_

#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/logger.hpp>
#include <boost/log/utility/setup.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/utility/setup/file.hpp>

#include <boost/log/support/date_time.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/thread.hpp>



#include <fstream>
#include <iomanip>


namespace logging = boost::log;
namespace sinks = boost::log::sinks;
namespace src = boost::log::sources;
namespace attrs = boost::log::attributes;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;


using namespace logging::trivial;
typedef src::severity_logger_mt<severity_level>  log_type;
extern boost::shared_ptr<log_type> my_logger;

void initlog();
#define TRACE_LOG BOOST_LOG_SEV(*my_logger, trace)
#define INFO_LOG  BOOST_LOG_SEV(*my_logger, error)
#define DEBUG_LOG BOOST_LOG_SEV(*my_logger, debug)
#define WARN_LOG  BOOST_LOG_SEV(*my_logger, warning)
#define ERROR_LOG BOOST_LOG_SEV(*my_logger, error)

#endif
