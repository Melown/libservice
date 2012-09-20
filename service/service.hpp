#ifndef shared_service_service_hpp_included_
#define shared_service_service_hpp_included_

#include <boost/scoped_ptr.hpp>

#include "program.hpp"

namespace service {

class service : protected program {
public:
    service(const std::string &name, const std::string &version);

    ~service();

    int operator()(int argc, char *argv[]);

    bool isRunning();

protected:
    virtual void configuration(po::options_description &cmdline
                               , po::options_description &config) = 0;

    virtual void configure(const po::variables_map &vars) = 0;

    virtual void start() = 0;

    virtual int run() = 0;

private:
    bool daemonize_;

    struct SignalHandler;

    boost::scoped_ptr<SignalHandler> signalHandler_;
};

} // namespace service

#endif // shared_service_service_hpp_included_
