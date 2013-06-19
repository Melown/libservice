#include <cstdint>
#include <memory>
#include <atomic>
#include <system_error>
#include <signal.h>

#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/utility/in_place_factory.hpp>

#include <boost/interprocess/anonymous_shared_memory.hpp>

#include "service.hpp"
#include "pidfile.hpp"

namespace asio = boost::asio;
namespace fs = boost::filesystem;
namespace bi = boost::interprocess;

namespace service {

runable::~runable()
{
}

namespace {
    inline void* getAddress(bi::mapped_region &mem, std::size_t offset = 0)
    {
        return static_cast<char*>(mem.get_address()) + offset;
    }
}

struct service::SignalHandler : boost::noncopyable {
public:
    SignalHandler(dbglog::module &log, const program &owner)
        : signals_(ios_, SIGTERM, SIGINT, SIGHUP)
        , mem_(bi::anonymous_shared_memory(1024))
        , terminated_(* new (getAddress(mem_)) std::atomic_bool(false))
          // TODO: what about alignment?
        , logRotateEvent_(* new (getAddress(mem_, sizeof(std::atomic_bool)))
                          std::atomic<std::uint64_t>(0))
        , lastLogRotateEvent_(0)
        , log_(log), owner_(owner)
    {
    }

    struct ScopedHandler {
        ScopedHandler(SignalHandler &h) : h(h) { h.start(); }
        ~ScopedHandler() { h.stop(); }

        SignalHandler &h;
    };

    void terminate() { terminated_ = true; }

    /** Processes events and returns whether we should terminate.
     */
    bool process() {
        ios_.poll();

        // TODO: last event should be (process-local) atomic to handle multiple
        // threads calling this function
        auto value(logRotateEvent_.load());
        if (value != lastLogRotateEvent_) {
            LOG(info3, log_) << "Logrotate: <" << owner_.logFile() << ">.";
            dbglog::log_file(owner_.logFile().string());
            LOG(info4, log_)
                << "Service " << owner_.name << '-' << owner_.version
                << ": log rotated.";
            lastLogRotateEvent_ = value;
        }

        return terminated_;
    }

private:
    void start() {
        signals_.async_wait(boost::bind(&SignalHandler::signal, this
                                        , asio::placeholders::error
                                        , asio::placeholders::signal_number));
    }

    void stop() {
        signals_.cancel();
    }

    void signal(const boost::system::error_code &e, int signo) {
        if (e) {
            if (boost::asio::error::operation_aborted == e) {
                return;
            }
            start();
        }

        LOG(debug, log_)
            << "SignalHandler received signal: <" << signo << ">.";
        switch (signo) {
        case SIGTERM:
        case SIGINT:
            LOG(info2, log_) << "Terminate signal: <" << signo << ">.";
            terminated_ = true;
            break;

        case SIGHUP:
            // mark logrotate
            ++logRotateEvent_;
            break;
        }
        start();
    }

    asio::io_service ios_;
    asio::signal_set signals_;
    bi::mapped_region mem_;
    std::atomic_bool &terminated_;
    std::atomic<std::uint64_t> &logRotateEvent_;
    std::uint64_t lastLogRotateEvent_;
    dbglog::module &log_;
    const program &owner_;
};

service::service(const std::string &name, const std::string &version
                 , int flags)
    : program(name, version, flags)
{}

service::~service()
{}

namespace {

void switchPersona(dbglog::module &log
                   , const std::string &username
                   , const std::string &groupname)
{
    bool switchUid(false);
    bool switchGid(false);
    uid_t uid(-1);
    gid_t gid(-1);

    LOG(info3, log)
        << "Trying to run under " << username << ":" << groupname << ".";
    if (!username.empty()) {
        auto pwd(::getpwnam(username.c_str()));
        if (!pwd) {
            LOGTHROW(err3, std::runtime_error)
                << "There is no user <" << username
                << "> present on the system.";
            throw;
        }

        // get uid and gid
        uid = pwd->pw_uid;
        gid = pwd->pw_gid;
        switchUid = switchGid = true;
    }

    if (!groupname.empty()) {
        auto gr(::getgrnam(groupname.c_str()));
        if (!gr) {
            LOGTHROW(err3, std::runtime_error)
                << "There is no group <" << groupname
                << "> present on the system.";
            throw;
        }
        gid = gr->gr_gid;
        switchGid = true;
    }

    // TODO: check whether we do not run under root!

    if (switchGid) {
        LOG(info3, log) << "Switching to gid <" << gid << ">.";
        if (-1 == ::setgid(gid)) {
            std::system_error e(errno, std::system_category());
            LOG(fatal, log)
                << "Cannot switch to gid <" << gid << ">: "
                << "<" << e.code() << ", " << e.what() << ">.";
            throw e;
        }
    }

    if (switchUid) {
        LOG(info3, log)
            << "Setting supplementary groups for user <"
            << username << ">.";
        if (-1 == ::initgroups(username.c_str(), gid)) {
            std::system_error e(errno, std::system_category());
            LOG(fatal, log)
                << "Cannot initialize supplementary groups for user <"
                << username << ">: <" << e.code()
                << ", " << e.what() << ">.";
            throw e;
        }

        LOG(info3, log) << "Switching to uid <" << uid << ">.";
        if (-1 == ::setuid(uid)) {
            std::system_error e(errno, std::system_category());
            LOG(fatal, log)
                << "Cannot switch to uid <" << uid << ">: "
                << "<" << e.code() << ", " << e.what() << ">.";
            throw e;
        }
    }
}

int sendSignal(dbglog::module &log, const fs::path &pidFile
               , const std::string &signal)
{
    int signo = 0;
    if (signal == "stop") {
        signo = SIGTERM;
    } else if (signal == "logrotate") {
        signo = SIGHUP;
    } else if (signal == "test") {
        signo = 0;
    } else {
        LOG(fatal, log) << "Unrecongized signal: <" << signal << ">.";
        return EXIT_FAILURE;
    }

    try {
        if (!pidfile::signal(pidFile, signo)) {
            // not running -> return 1
            return 1;
        }
    } catch (const std::exception &e) {
        LOG(fatal, log)
            << "Cannot signal running instance: <" << e.what() << ">.";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

} // namespace

void service::preNotifyHook(const po::variables_map &vars)
{
    if (!vars.count("signal")) { return; }
    if (!vars.count("pidfile")) {
        LOG(fatal, log_) << "Pid file must be specified to send signal.";
        throw immediate_exit(EXIT_FAILURE);
    }

    // send signal and terminate
    immediateExit(sendSignal(log_, vars["pidfile"].as<fs::path>()
                             , vars["signal"].as<std::string>()));
}

int service::operator()(int argc, char *argv[])
{
    dbglog::thread_id("main");

    bool daemonize(false);
    bool daemonizeNochdir(false);
    bool daemonizeNoclose(false);

    std::string username;
    std::string groupname;
    fs::path pidFilePath;

    try {
        po::options_description genericCmdline("command line options");

        po::options_description genericConfig
            ("configuration file options (all options can be overridden "
             "on command line)");

        genericCmdline.add_options()
            ("daemonize,d"
             , "Run in daemon mode (otherwise run in foreground).")
            ("daemonize-nochdir"
             , "Do not leave current directory after forking to background.")
            ("daemonize-noclose"
             , "Do not close STDIN/OUT/ERR after forking to background.")
            ("pidfile", po::value(&pidFilePath)
             , "Path to pid file.")
            ("signal,s", po::value<std::string>()
             , "Signal to be sent to running instance: stop, logrotate, test.")
            ;

        genericConfig.add_options()
            ("service.user", po::value(&username)
             , "Switch process persona to given username.")
            ("service.group", po::value(&groupname)
             , "Switch process persona to given group name.")
            ;

        auto vm(program::configure(argc, argv, genericCmdline, genericConfig));
        daemonize = vm.count("daemonize");
        daemonizeNochdir = vm.count("daemonize-nochdir");
        daemonizeNoclose = vm.count("daemonize-noclose");

        if (!daemonize && (daemonizeNochdir || daemonizeNoclose)) {
            LOG(warn4, log_)
                << "Options --daemonize-nochdir and --daemonize-noclose "
                "make sense only together with --daemonize.";
        }
    } catch (const immediate_exit &e) {
        return e.code;
    }

    signalHandler_.reset(new SignalHandler(log_, *this));

    LOG(info4, log_) << "Service " << name << '-' << version << " starting.";

    // daemonize if asked to do so

    // TODO: wait for child to settle down, use some pipe to send status
    if (daemonize) {
        LOG(info4, log_) << "Forking to background.";
        if (-1 == daemon(true, true)) {
            LOG(fatal) << "Failed to daemonize: " << errno;
            return EXIT_FAILURE;
        }
        LOG(info4, log_) << "Running in background.";
    }

    if (!pidFilePath.empty()) {
        // handle pidfile
        try {
            pidfile::allocate(pidFilePath);
        } catch (const std::exception &e) {
            LOG(fatal, log_) << "Cannot allocate pid file: " << e.what();
            return EXIT_FAILURE;
        }
    }

    try {
        switchPersona(log_, username, groupname);
    } catch (const std::exception &e) {
        return EXIT_FAILURE;
    }

    int code = EXIT_SUCCESS;
    {
        SignalHandler::ScopedHandler signals(*signalHandler_);

        Cleanup cleanup;
        try {
            cleanup = start();
        } catch (const immediate_exit &e) {
            if (daemonize) {
                LOG(warn4, log_)
                    << "Startup exists with exist status: " << e.code << ".";
            }
            return e.code;
        }

        if (!isRunning()) {
            LOG(info4, log_) << "Terminated during startup.";
            return EXIT_FAILURE;
        }

        if (daemonize) {
            // replace stdin/out/err with /dev/null
            if (!daemonizeNoclose) {
                // TODO: check errors
                auto null(::open("/dev/null", O_RDWR));
                ::dup2(null, STDIN_FILENO);
                ::dup2(null, STDOUT_FILENO);
                ::dup2(null, STDERR_FILENO);
            }

            // go away!
            if (!daemonizeNochdir) {
                if (-1 == ::chdir("/")) {
                    std::system_error e(errno, std::system_category());
                    LOG(warn3, log_)
                        << "Cannot cd to /: "
                        << "<" << e.code() << ", " << e.what() << ">.";
                }
            }
            // disable log here
            dbglog::log_console(false);
        }

        code = run();
    }

    if (code) {
        LOG(err4, log_) << "Terminated with error " << code << '.';
    } else {
        LOG(info4, log_) << "Terminated normally.";
    }

    return code;
}

void service::stop()
{
    signalHandler_->terminate();
}

bool service::isRunning() {
    return !signalHandler_->process();
}

void service::configure(const std::vector<std::string> &)
{
    throw po::error
        ("Program asked to collect unrecognized options "
         "although it is not processing them. Go fix your program.");
}

inline bool service::help(std::ostream &, const std::string &)
{
    return false;
}

} // namespace service
