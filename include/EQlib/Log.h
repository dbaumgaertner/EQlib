#pragma once

#include "Define.h"

#include <pybind11/pybind11.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <stack>

namespace EQlib {

class Log
{
private:    // methods
    static std::shared_ptr<spdlog::logger>
    create()
    {
        auto logger = spdlog::stdout_color_mt("console");

        logger->set_pattern("%H:%M:%S.%e %^[%L]%$ %v");

        return logger;
    }

private:    // variables
    static const inline std::shared_ptr<spdlog::logger> s_console = create();

    static inline int s_info_level = 0;

public:     // methods
    static int info_level()
    {
        return s_info_level;
    }

    static void set_info_level(const int value)
    {
        s_info_level = value;
    }

    template<class... TArgs>
    static void debug(TArgs&&... args)
    {
        s_console->debug(std::forward<TArgs>(args)...);
    }

    template<class... TArgs>
    static void info(TArgs&&... args)
    {
        s_console->info(std::forward<TArgs>(args)...);
    }

    template<class... TArgs>
    static void info(const int level, TArgs&&... args)
    {
        if (level > info_level()) {
            return;
        }

        info(std::forward<TArgs>(args)...);
    }

    template<class... TArgs>
    static void error(TArgs&&... args)
    {
        s_console->error(std::forward<TArgs>(args)...);
    }

    template<class... TArgs>
    static void warn(TArgs&&... args)
    {
        s_console->warn(std::forward<TArgs>(args)...);
    }

    template<class... TArgs>
    static void critical(TArgs&&... args)
    {
        s_console->critical(std::forward<TArgs>(args)...);
    }
};

} // namespace EQlib