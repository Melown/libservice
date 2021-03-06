/**
 * Copyright (c) 2017 Melown Technologies SE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * *  Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "cmdline.hpp"

/*
 * Defining SERVICE_PRINT_ALL_EXCEPTIONS will ensure that
 * any unhandled exception is first printed to standard
 * error output.
 * The exception is rethrown afterwards
 * and the program crashes as it would have anyway.
 * This behavior is especially useful on windows, where
 * post-mortem debugging is more difficult
 * and we would be left completely blind.
 * Elsewhere, this behavior is disabled by default
 * to not interfere with post-mortem debugging.
 * It is also disabled, by default, for debug configurations
 * so that the exception can be handled by debugger
 * at the original throwing site.
 */
#if !defined(SERVICE_PRINT_ALL_EXCEPTIONS) \
    && defined(_WIN32) && defined(NDEBUG)
    // implicitly define for this configuration
#   define SERVICE_PRINT_ALL_EXCEPTIONS
#endif

#ifdef SERVICE_PRINT_ALL_EXCEPTIONS
#   include <iostream>
#   include <iomanip>
#endif

namespace service {

#ifdef SERVICE_PRINT_ALL_EXCEPTIONS

namespace {

[[noreturn]] void handleException()
{
    try
    {
        throw; // rethrow the currently active exception
    }
    catch (const std::exception &e)
    {
        std::cerr << "Cmdline: Unhandled standard exception: <"
            << e.what() << ">." << std::endl;
        throw; // this handler may not return
    }
    catch (...)
    {
        std::cerr << "Cmdline: Unhandled exception of unknown type."
            << std::endl;
        throw; // this handler may not return
    }
}

} // namespace

Cmdline::Cmdline(const std::string &name, const std::string &version
    , int flags)
    try : Program(name, version, flags)
{}
catch (...)
{
    handleException();
}

#else

Cmdline::Cmdline(const std::string &name, const std::string &version
                 , int flags)
    : Program(name, version, flags)
{}

#endif // SERVICE_PRINT_ALL_EXCEPTIONS

Cmdline::~Cmdline()
{}

int Cmdline::operator()(int argc, char *argv[])
{
#ifdef SERVICE_PRINT_ALL_EXCEPTIONS

    try
    {
        return handleOperator(argc, argv);
    }
    catch (...)
    {
        handleException();
    }

#else

    return handleOperator(argc, argv);

#endif // SERVICE_PRINT_ALL_EXCEPTIONS
}

int Cmdline::handleOperator(int argc, char *argv[])
{
    dbglog::thread_id("main");

    try {
        Program::configure
            (argc, argv, po::options_description
             ("configuration file options (all options can be overridden "
              "on command line)"));
    } catch (const immediate_exit &e) {
        return e.code;
    }

    int code = 0;

    try {
        code = run();
    } catch (const immediate_exit &e) {
        code = e.code;
    }

    if (code && !noExcessiveLogging()) {
        LOG(err4, log_) << "Terminated with error " << code << '.';
    }

    return code;
}

void Cmdline::configure(const std::vector<std::string>&)
{
    throw po::error
        ("Program asked to collect unrecognized options "
         "although it is not processing them. Go fix your program.");
}

inline bool Cmdline::help(std::ostream &, const std::string &) const
{
    return false;
}

} // namespace cmdline
