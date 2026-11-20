#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>

namespace testing {

// CMake 按测试源码的相对目录指定报告路径，与启动程序时的工作目录无关。
inline std::filesystem::path report_path() {
    const std::filesystem::path path(IOMAN_TEST_REPORT_FILE);
    std::filesystem::create_directories(path.parent_path());
    return path;
}

// 同时转发到终端和报告。逐次刷新，强制退出时也保留已完成用例的输出。
class ReportBuffer final : public std::streambuf {
public:
    ReportBuffer(std::streambuf* terminal, std::ofstream& report, std::mutex& mutex)
        : _M_terminal(terminal), _M_report(report), _M_mutex(mutex) {}
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        _M_report.write(data, count);
        _M_report.flush();
        return _M_terminal->sputn(data, count);
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override {
        const std::lock_guard<std::mutex> lock(_M_mutex);
        _M_report.flush();
        return _M_terminal->pubsync();
    }
private:
    std::streambuf* _M_terminal;
    std::ofstream& _M_report;
    std::mutex& _M_mutex;
};

// 在 main 中最先创建，报告按源码的相对目录写入 tests/report，每次运行覆盖上次输出。
class Report {
public:
    explicit Report(const char* source)
        : _M_file(report_path()),
          _M_out(std::cout.rdbuf(), _M_file, _M_mutex),
          _M_err(std::cerr.rdbuf(), _M_file, _M_mutex) {
        if (!_M_file) {
            throw std::runtime_error("Cannot open test report");
        }
        _M_original_out = std::cout.rdbuf(&_M_out);
        _M_original_err = std::cerr.rdbuf(&_M_err);
        std::cout << "Source: " << source << "\n";
    }
    ~Report() {
        std::cout << "Duration_ms: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - _M_started).count() << "\n";
        std::cout.rdbuf(_M_original_out);
        std::cerr.rdbuf(_M_original_err);
    }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
private:
    std::mutex _M_mutex;
    std::ofstream _M_file;
    ReportBuffer _M_out;
    ReportBuffer _M_err;
    std::streambuf* _M_original_out;
    std::streambuf* _M_original_err;
    std::chrono::steady_clock::time_point _M_started = std::chrono::steady_clock::now();
};

inline void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

constexpr const char* default_xfail_message = "Expected exception was not thrown";

template <typename Exception, typename Function>
void throws(Function&& operation, const char* const message = default_xfail_message) {
    try {
        operation();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct Case {
    const char* name;
    void (*run)();
};

inline int run(const char* source, std::initializer_list<Case> cases) {
    Report report(source);
    std::promise<void> finished;
    auto completion = finished.get_future();
    std::thread watchdog([&] {
        if (completion.wait_for(std::chrono::seconds{50}) != std::future_status::ready) {
            std::cerr << "[FAIL] suite timed out, active case did not return\n";
            std::_Exit(EXIT_FAILURE);
        }
    });
    std::size_t failures = 0;
    for (const auto& entry : cases) {
        std::cout << "[RUN] " << entry.name << std::endl;
        try {
            entry.run();
            std::cout << "[PASS] " << entry.name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << entry.name << ": " << error.what() << std::endl;
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << entry.name << ": unknown exception" << std::endl;
        }
    }
    finished.set_value();
    watchdog.join();
    std::cout << "Total: " << cases.size() << ", passed: " << cases.size() - failures
              << ", failed: " << failures << std::endl;
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace testing
