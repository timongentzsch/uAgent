#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>

#include "agent.hpp"
#include "util.hpp"

namespace {

volatile size_t sink = 0;

template <class F>
double measure(size_t iterations, F&& work) {
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) sink += work();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start)
        .count();
}

void report(const char* name, size_t iterations, double milliseconds) {
    double operations_per_second =
        milliseconds > 0 ? static_cast<double>(iterations) * 1000.0 / milliseconds : 0;
    std::cout << std::left << std::setw(28) << name << std::right << std::fixed
              << std::setprecision(2) << std::setw(10) << milliseconds << " ms  "
              << std::setprecision(0) << std::setw(12) << operations_per_second
              << " ops/s\n";
}

}  // namespace

int main() {
    constexpr size_t iterations = 10000;
    const std::string call =
        "[uagent_tool_call]{\"name\":\"read_file\",\"arguments\":{\"path\":\"src/main.cpp\","
        "\"offset\":1,\"limit\":200}}[/uagent_tool_call]";
    const std::string hostile =
        "normal text\n\x1b]52;c;payload\x07\tmore text\n";
    const std::string large(24000, 'x');

    report("text tool-call parse", iterations,
           measure(iterations, [&] { return parse_text_tool_calls(call).size(); }));

    bool prior_tty = g_tty;
    g_tty = true;
    report("terminal sanitization", iterations,
           measure(iterations, [&] { return terminal_safe(hostile).size(); }));
    g_tty = prior_tty;

    report("tool-result cap", iterations,
           measure(iterations, [&] { return cap_result(large).size(); }));

    ProcessSupervisor processes;
    auto lean_tools = builtin_tools(processes, canonical_access_path("."), false);
    auto image_tools = builtin_tools(processes, canonical_access_path("."), true);
    auto base_tools = lean_tools;
    auto without = [](std::vector<Tool> tools, const std::string& name) {
        tools.erase(
            std::remove_if(tools.begin(), tools.end(),
                           [&](const Tool& tool) { return tool.name == name; }),
            tools.end());
        return tools;
    };
    auto no_python_tools = without(lean_tools, "run_python");
    base_tools = without(no_python_tools, "grep");
    size_t base_schema = tool_schemas(base_tools).dump().size();
    size_t grep_schema = tool_schemas(no_python_tools).dump().size();
    size_t lean_schema = tool_schemas(lean_tools).dump().size();
    size_t image_schema = tool_schemas(image_tools).dump().size();
    std::cout << "built-in schema              " << lean_schema << " bytes (~"
              << lean_schema / 4 << " tokens); grep adds "
              << grep_schema - base_schema << " bytes; Python adds "
              << lean_schema - grep_schema << " bytes; inline image adds "
              << image_schema - lean_schema << " bytes\n";
    std::cout << "sink " << sink << '\n';
    return 0;
}
