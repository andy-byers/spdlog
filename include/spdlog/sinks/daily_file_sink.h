// Copyright(c) 2015-present, Gabi Melman & spdlog contributors.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)

#pragma once

#include <spdlog/common.h>
#include <spdlog/details/file_helper.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/chrono.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/details/os.h>
#include <spdlog/details/circular_q.h>
#include <spdlog/details/synchronous_factory.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <dirent.h>

namespace spdlog {
namespace sinks {

/*
 * Generator of daily log file names in format basename.YYYY-MM-DD.ext
 */
struct daily_filename_calculator
{
    // Create filename for the form basename.YYYY-MM-DD
    static filename_t calc_filename(const filename_t &filename, const tm &now_tm)
    {
        filename_t basename, ext;
        std::tie(basename, ext) = details::file_helper::split_by_extension(filename);
        return fmt_lib::format(SPDLOG_FMT_STRING(SPDLOG_FILENAME_T("{}_{:04d}-{:02d}-{:02d}{}")), basename, now_tm.tm_year + 1900,
            now_tm.tm_mon + 1, now_tm.tm_mday, ext);
    }
};

/*
 * Generator of daily log file names with strftime format.
 * Usages:
 *    auto sink =  std::make_shared<spdlog::sinks::daily_file_format_sink_mt>("myapp-%Y-%m-%d:%H:%M:%S.log", hour, minute);"
 *    auto logger = spdlog::daily_logger_format_mt("loggername, "myapp-%Y-%m-%d:%X.log", hour,  minute)"
 *
 */
struct daily_filename_format_calculator
{
    static filename_t calc_filename(const filename_t &filename, const tm &now_tm)
    {
#ifdef SPDLOG_USE_STD_FORMAT
        // adapted from fmtlib: https://github.com/fmtlib/fmt/blob/8.0.1/include/fmt/chrono.h#L522-L546

        filename_t tm_format;
        tm_format.append(filename);
        // By appending an extra space we can distinguish an empty result that
        // indicates insufficient buffer size from a guaranteed non-empty result
        // https://github.com/fmtlib/fmt/issues/2238
        tm_format.push_back(' ');

        const size_t MIN_SIZE = 10;
        filename_t buf;
        buf.resize(MIN_SIZE);
        for (;;)
        {
            size_t count = strftime(buf.data(), buf.size(), tm_format.c_str(), &now_tm);
            if (count != 0)
            {
                // Remove the extra space.
                buf.resize(count - 1);
                break;
            }
            buf.resize(buf.size() * 2);
        }

        return buf;
#else
        // generate fmt datetime format string, e.g. {:%Y-%m-%d}.
        filename_t fmt_filename = fmt::format(SPDLOG_FMT_STRING(SPDLOG_FILENAME_T("{{:{}}}")), filename);

        // MSVC doesn't allow fmt::runtime(..) with wchar, with fmtlib versions < 9.1.x
#    if defined(_MSC_VER) && defined(SPDLOG_WCHAR_FILENAMES) && FMT_VERSION < 90101
        return fmt::format(fmt_filename, now_tm);
#    else
        return fmt::format(SPDLOG_FMT_RUNTIME(fmt_filename), now_tm);
#    endif

#endif
    }

private:
#if defined __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif

    static size_t strftime(char *str, size_t count, const char *format, const std::tm *time)
    {
        return std::strftime(str, count, format, time);
    }

    static size_t strftime(wchar_t *str, size_t count, const wchar_t *format, const std::tm *time)
    {
        return std::wcsftime(str, count, format, time);
    }

#if defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif
};

/*
 * Rotating file sink based on date.
 * If truncate != false , the created file will be truncated.
 * If max_files > 0, retain only the last max_files and delete previous.
 */
template<typename Mutex, typename FileNameCalc = daily_filename_calculator>
class daily_file_sink final : public base_sink<Mutex>
{
public:
    // create daily file sink which rotates on given time
    daily_file_sink(filename_t base_filename, int rotation_hour, int rotation_minute, bool truncate = false, uint16_t max_files = 0,
        const file_event_handlers &event_handlers = {})
        : base_filename_(std::move(base_filename))
        , rotation_h_(rotation_hour)
        , rotation_m_(rotation_minute)
        , file_helper_{event_handlers}
        , truncate_(truncate)
        , max_files_(max_files)
    {
        if (rotation_hour < 0 || rotation_hour > 23 || rotation_minute < 0 || rotation_minute > 59)
        {
            throw_spdlog_ex("daily_file_sink: Invalid rotation time in ctor");
        }

        auto now = log_clock::now();
        auto filename = FileNameCalc::calc_filename(base_filename_, now_tm(now));
        file_helper_.open(filename, truncate_);
        rotation_tp_ = next_rotation_tp_();

        if (max_files_ > 0)
        {
            remove_obsolete_logs_();
        }
    }

    filename_t filename()
    {
        std::lock_guard<Mutex> lock(base_sink<Mutex>::mutex_);
        return file_helper_.filename();
    }

protected:
    void sink_it_(const details::log_msg &msg) override
    {
        auto time = msg.time;
        bool should_rotate = time >= rotation_tp_;
        if (should_rotate)
        {
            auto filename = FileNameCalc::calc_filename(base_filename_, now_tm(time));
            file_helper_.open(filename, truncate_);
            rotation_tp_ = next_rotation_tp_();
        }
        memory_buf_t formatted;
        base_sink<Mutex>::formatter_->format(msg, formatted);
        file_helper_.write(formatted);

        // Do the cleaning only at the end because it might throw on failure.
        if (should_rotate && max_files_ > 0)
        {
            delete_old_(time);
        }
    }

    void flush_() override
    {
        file_helper_.flush();
    }

private:
    void remove_obsolete_logs_()
    {
        using details::os::path_exists;
        using details::os::iterate_dir;
        using details::os::remove_if_exists;

        filename_t folder = details::os::dir_name(base_filename_);
        const filename_t sep {details::os::folder_seps_filename};
        filename_t basename = base_filename_.substr(folder.size());
        if (basename.find(sep) == 0)
        {
            folder += sep;
            basename = basename.substr(sep.size());
        }
        filename_t prefix, ext;
        std::tie(prefix, ext) = details::file_helper::split_by_extension(basename);

        prefix += '_';
        if (folder.empty())
        {
            folder = ".";
        }

        static constexpr std::size_t suffix_size = 11; // _YYYY-mm-dd
        const std::size_t target_size = basename.size() + suffix_size;

        const auto is_daily_log = [&](const filename_t &filename)
        {
            if (filename.size() == target_size && filename.find(prefix) == 0 && filename.rfind(ext) == filename.size() - ext.size())
            {
                // TODO: Check for valid day/month/year values, or better way to validate the date?
                static constexpr int is_separator[] {0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
                const auto *itr = std::begin(is_separator);

                return std::all_of(
                    begin(filename) + static_cast<filename_t::difference_type>(prefix.size()),
                    end(filename) - static_cast<filename_t::difference_type>(ext.size()),
                    [&itr](char chr)
                    {
                        if (*itr++ != 0)
                        {
                            return chr == '-';
                        }
                        return std::isdigit(chr) != 0;
                    });
            }
            return false;
        };

        const tm cutoff_tm = now_tm(cutoff_tp_(log_clock::now()));
        const filename_t cutoff_filename = FileNameCalc::calc_filename(basename, cutoff_tm);
        const filename_t cutoff_date = cutoff_filename.substr(prefix.size(), suffix_size - 1);

        iterate_dir(folder, [&](const filename_t &filename) {
            if (is_daily_log(filename))
            {
                const filename_t current_date = filename.substr(prefix.size(), suffix_size - 1);
                if (current_date <= cutoff_date)
                {
                    details::os::remove_if_exists(folder + filename);
                }
            }
        });
    }

    tm now_tm(log_clock::time_point tp)
    {
        time_t tnow = log_clock::to_time_t(tp);
        return spdlog::details::os::localtime(tnow);
    }

    log_clock::time_point next_rotation_tp_()
    {
        auto now = log_clock::now();
        tm date = now_tm(now);
        date.tm_hour = rotation_h_;
        date.tm_min = rotation_m_;
        date.tm_sec = 0;
        auto rotation_time = log_clock::from_time_t(std::mktime(&date));
        if (rotation_time > now)
        {
            return rotation_time;
        }
        return {rotation_time + std::chrono::hours(24)};
    }

    // Delete the file N rotations ago.
    // Throw spdlog_ex on failure to delete the old file.
    void delete_old_(log_clock::time_point current)
    {
        using details::os::filename_to_str;
        using details::os::remove_if_exists;

        const tm cutoff = now_tm(cutoff_tp_(current));
        const filename_t cutoff_filename = FileNameCalc::calc_filename(base_filename_, cutoff);
        bool ok = remove_if_exists(cutoff_filename) == 0;
        if (!ok)
        {
            throw_spdlog_ex("Failed removing daily file " + filename_to_str(cutoff_filename), errno);
        }
    }

    log_clock::time_point cutoff_tp_(log_clock::time_point current)
    {
        return current - std::chrono::hours(24)*max_files_;
    }

    filename_t base_filename_;
    int rotation_h_;
    int rotation_m_;
    log_clock::time_point rotation_tp_;
    details::file_helper file_helper_;
    bool truncate_;
    uint16_t max_files_;
};

using daily_file_sink_mt = daily_file_sink<std::mutex>;
using daily_file_sink_st = daily_file_sink<details::null_mutex>;
using daily_file_format_sink_mt = daily_file_sink<std::mutex, daily_filename_format_calculator>;
using daily_file_format_sink_st = daily_file_sink<details::null_mutex, daily_filename_format_calculator>;

} // namespace sinks

//
// factory functions
//
template<typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_mt(const std::string &logger_name, const filename_t &filename, int hour = 0, int minute = 0,
    bool truncate = false, uint16_t max_files = 0, const file_event_handlers &event_handlers = {})
{
    return Factory::template create<sinks::daily_file_sink_mt>(logger_name, filename, hour, minute, truncate, max_files, event_handlers);
}

template<typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_format_mt(const std::string &logger_name, const filename_t &filename, int hour = 0,
    int minute = 0, bool truncate = false, uint16_t max_files = 0, const file_event_handlers &event_handlers = {})
{
    return Factory::template create<sinks::daily_file_format_sink_mt>(
        logger_name, filename, hour, minute, truncate, max_files, event_handlers);
}

template<typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_st(const std::string &logger_name, const filename_t &filename, int hour = 0, int minute = 0,
    bool truncate = false, uint16_t max_files = 0, const file_event_handlers &event_handlers = {})
{
    return Factory::template create<sinks::daily_file_sink_st>(logger_name, filename, hour, minute, truncate, max_files, event_handlers);
}

template<typename Factory = spdlog::synchronous_factory>
inline std::shared_ptr<logger> daily_logger_format_st(const std::string &logger_name, const filename_t &filename, int hour = 0,
    int minute = 0, bool truncate = false, uint16_t max_files = 0, const file_event_handlers &event_handlers = {})
{
    return Factory::template create<sinks::daily_file_format_sink_st>(
        logger_name, filename, hour, minute, truncate, max_files, event_handlers);
}
} // namespace spdlog
