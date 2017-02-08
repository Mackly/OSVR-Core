/** @file
    @brief Implementation

    @date 2016

    @author
    Sensics, Inc.
    <http://sensics.com>

*/

// Copyright 2016 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// 	http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Internal Includes
#include <osvr/Util/Logger.h>

#include "LogDefaults.h"
#include "LogLevelTranslate.h"

// Library/third-party includes
#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

// Standard includes
#include <iostream>
#include <memory>  // for std::shared_ptr
#include <string>  // for std::string
#include <utility> // for std::forward

namespace osvr {
namespace util {
    namespace log {

        inline LoggerPtr
        Logger::makeLogger(std::string const &name,
                           std::shared_ptr<spdlog::logger> const &logger) {

#ifdef OSVR_USE_UNIQUEPTR_FOR_LOGGER
            auto ret = LoggerPtr{new Logger{
                name, logger, static_cast<PrivateConstructor *>(nullptr)}};
#else
            auto ret = std::make_shared<Logger>(
                name, logger, static_cast<PrivateConstructor *>(nullptr));
#endif
            return ret;
        }

        Logger::Logger(std::string const &name,
                       std::shared_ptr<spdlog::logger> logger,
                       PrivateConstructor *)
            : name_(name), logger_(std::move(logger)) {}

        Logger::~Logger() {}

        LoggerPtr Logger::makeFallback(std::string const &name) {
            // First, we'll attempt to create a console logger to use as a
            // fallback. If that fails, we'll create a do-nothing logger
            // instead.
            std::cerr << "WARNING: Logger created for '" << name
                      << "' is a 'fallback' logger -- an internal error has "
                         "prevented a standard logger from being created. "
                         "Please report this issue in OSVR-Core on GitHub."
                      << std::endl;
            try {
                auto console_logger = spdlog::stderr_logger_mt(name);
                return makeLogger(name, console_logger);
            } catch (...) {
                std::cerr << "Failed to create a console logger to use as a "
                             "fallback. Logging will be disabled entirely."
                          << std::endl;
                auto null_sink = std::make_shared<spdlog::sinks::null_sink_st>();
                auto null_logger = std::make_shared<spdlog::logger>(name, null_sink);
                return makeLogger(name, null_logger);
            }
        }

        LoggerPtr Logger::makeFromExistingImplementation(
            std::string const &name, std::shared_ptr<spdlog::logger> logger) {
            if (!logger) {
                std::cerr
                    << "WARNING: Logger::makeFromExistingImplementation(\""
                    << name << "\", logger) called with a null logger pointer! "
                    << "Will result in a fallback logger!" << std::endl;
                return makeFallback(name);
            }
            return makeLogger(name, logger);
        }

        LoggerPtr Logger::makeWithSink(std::string const &name,
                                       spdlog::sink_ptr sink) {
            if (!sink) {
                // bad sink!
                std::cerr << "WARNING: Logger::makeWithSink(\"" << name
                          << "\", sink) called with a null sink! Will result "
                             "in a fallback logger!"
                          << std::endl;
                return makeFallback(name);
            }
            auto spd_logger = std::make_shared<spdlog::logger>(name, sink);
            spd_logger->set_pattern(DEFAULT_PATTERN);
            spd_logger->flush_on(convertToLevelEnum(DEFAULT_FLUSH_LEVEL));
            return makeLogger(name, spd_logger);
        }

        LoggerPtr Logger::makeWithSinks(std::string const &name,
                                        spdlog::sinks_init_list sinks) {
            for (auto &sink : sinks) {
                if (!sink) {
                    std::cerr << "WARNING: "
                                 "Logger::makeWithSinks(\""
                              << name << "\", sinks) called "
                                         "with at least one null sink! Will "
                                         "result in a fallback logger!"
                              << std::endl;
                    // got a bad sink
                    /// @todo should we be making a fallback logger here, just
                    /// hoping spdlog will deal with a bad sink pointer without
                    /// issue, or filtering the init list to a non-nullptr
                    /// vector?
                    return makeFallback(name);
                }
            }
            auto spd_logger = std::make_shared<spdlog::logger>(name, sinks);
            spd_logger->set_pattern(DEFAULT_PATTERN);
            spd_logger->flush_on(convertToLevelEnum(DEFAULT_FLUSH_LEVEL));
            return makeLogger(name, spd_logger);
        }

        LogLevel Logger::getLogLevel() const {
            return convertFromLevelEnum(logger_->level());
        }

        void Logger::setLogLevel(LogLevel level) {
            logger_->set_level(convertToLevelEnum(level));
        }

        void Logger::flushOn(LogLevel level) {
            logger_->flush_on(convertToLevelEnum(level));
        }

        Logger::StreamProxy Logger::trace(const char *msg) {
            return { *this, LogLevel::trace, msg };
        }

        Logger::StreamProxy Logger::debug(const char *msg) {
            return { *this, LogLevel::debug, msg };
        }

        Logger::StreamProxy Logger::info(const char *msg) {
            return { *this, LogLevel::info, msg };
        }

        Logger::StreamProxy Logger::notice(const char *msg) {
            return { *this, LogLevel::notice, msg };
        }

        Logger::StreamProxy Logger::warn(const char *msg) {
            return { *this, LogLevel::warn, msg };
        }

        Logger::StreamProxy Logger::error(const char *msg) {
            return { *this, LogLevel::error, msg };
        }

        Logger::StreamProxy Logger::critical(const char *msg) {
            return { *this, LogLevel::critical, msg };
        }

        // logger.info() << ".." call  style
        Logger::StreamProxy Logger::trace() {
            return { *this, LogLevel::trace };
        }

        Logger::StreamProxy Logger::debug() {
            return { *this, LogLevel::debug };
        }

        Logger::StreamProxy Logger::info() {
            return { *this, LogLevel::info };
        }

        Logger::StreamProxy Logger::notice() {
            return { *this, LogLevel::notice };
        }

        Logger::StreamProxy Logger::warn() {
            return { *this, LogLevel::warn };
        }

        Logger::StreamProxy Logger::error() {
            return { *this, LogLevel::error };
        }

        Logger::StreamProxy Logger::critical() {
            return { *this, LogLevel::critical };
        }

        // logger.log(log_level, msg) << ".." call style
        Logger::StreamProxy Logger::log(LogLevel level, const char *msg) {
            switch (level) {
            case LogLevel::trace:
                return trace(msg);
            case LogLevel::debug:
                return debug(msg);
            case LogLevel::info:
                return info(msg);
            case LogLevel::notice:
                return notice(msg);
            case LogLevel::warn:
                return warn(msg);
            case LogLevel::error:
                return error(msg);
            case LogLevel::critical:
                return critical(msg);
            }

            return info(msg);
        }

        // logger.log(log_level) << ".." call  style
        Logger::StreamProxy Logger::log(LogLevel level) {
            switch (level) {
            case LogLevel::trace:
                return trace();
            case LogLevel::debug:
                return debug();
            case LogLevel::info:
                return info();
            case LogLevel::notice:
                return notice();
            case LogLevel::warn:
                return warn();
            case LogLevel::error:
                return error();
            case LogLevel::critical:
                return critical();
            }

            return info();
        }

        void Logger::flush() {
            logger_->flush();
        }

        void Logger::write(LogLevel level, const char* msg)
        {
            logger_->log(convertToLevelEnum(level), msg);
        }

    } // namespace log
} // namespace util
} // namespace osvr
