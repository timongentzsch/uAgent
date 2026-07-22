#pragma once
// Terminal input and slash-command metadata. Commands, help, parsing, and
// completion all derive from one registry.

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "util.hpp"

enum class CommandCompletion { none, filenames, models, efforts };
enum class SlashCommandId {
    attach,
    compact,
    detach,
    effort,
    model,
    models,
    online,
    quit,
    reset,
    sessions,
    yolo,
};

struct SlashCommandSpec {
    SlashCommandId id;
    const char* name;
    const char* argument;
    CommandCompletion completion;
    bool visible;
};

inline constexpr SlashCommandSpec SLASH_COMMANDS[] = {
    {SlashCommandId::attach, "/attach", "PATH", CommandCompletion::filenames, true},
    {SlashCommandId::compact, "/compact", "", CommandCompletion::none, true},
    {SlashCommandId::detach, "/detach", "", CommandCompletion::none, true},
    {SlashCommandId::effort, "/effort", "LEVEL", CommandCompletion::efforts, true},
    {SlashCommandId::model, "/model", "NAME", CommandCompletion::models, true},
    {SlashCommandId::models, "/models", "[FILTER|all]", CommandCompletion::none, true},
    {SlashCommandId::online, "/online", "", CommandCompletion::none, true},
    {SlashCommandId::quit, "/quit", "", CommandCompletion::none, true},
    {SlashCommandId::reset, "/reset", "", CommandCompletion::none, true},
    {SlashCommandId::sessions, "/sessions", "", CommandCompletion::none, true},
    {SlashCommandId::yolo, "/yolo", "", CommandCompletion::none, true},
    {SlashCommandId::quit, "/exit", "", CommandCompletion::none, false},
    {SlashCommandId::quit, "/q", "", CommandCompletion::none, false},
    {SlashCommandId::reset, "/clear", "", CommandCompletion::none, false},
    {SlashCommandId::reset, "/new", "", CommandCompletion::none, false},
};

inline const SlashCommandSpec* slash_command(const std::string& name) {
    for (const SlashCommandSpec& command : SLASH_COMMANDS)
        if (name == command.name) return &command;
    return nullptr;
}

struct ParsedSlashCommand {
    const SlashCommandSpec* spec = nullptr;
    std::string argument;
};

inline ParsedSlashCommand parse_slash_command(const std::string& input) {
    for (const SlashCommandSpec& command : SLASH_COMMANDS) {
        if (input == command.name) return {&command, ""};
        std::string prefix = std::string(command.name) + " ";
        if (*command.argument && input.rfind(prefix, 0) == 0)
            return {&command, trim(input.substr(prefix.size()))};
    }
    return {};
}

inline void print_command_help() {
    printf("%scommands:", DIM());
    for (const SlashCommandSpec& command : SLASH_COMMANDS)
        if (command.visible)
            printf(" %s%s%s", command.name, *command.argument ? " " : "", command.argument);
    printf("%s\n", RST());
}

#if defined(HAVE_EDITLINE)
#include <editline/readline.h>

inline int esc_pending = 0;
inline std::string readline_initial;
inline size_t readline_initial_pos = 0;
inline bool steering_prompt = false, steering_cancelled = false;
inline std::vector<std::string> readline_commands, readline_models, readline_efforts;
inline const std::vector<std::string>* readline_candidates = nullptr;

inline char* completion_candidate(const char* text, int state) {
    static size_t index;
    if (!state) index = 0;
    while (readline_candidates && index < readline_candidates->size()) {
        const std::string& candidate = (*readline_candidates)[index++];
        if (candidate.rfind(text, 0) == 0) return strdup(candidate.c_str());
    }
    return nullptr;
}

inline char** uagent_completion(const char* text, int start, int) {
    rl_attempted_completion_over = 1;
    readline_candidates = nullptr;
    std::string before = rl_line_buffer ? std::string(rl_line_buffer, start) : "";
    if (start == 0 && *text == '/')
        readline_candidates = &readline_commands;
    else if (const SlashCommandSpec* command = slash_command(trim(before))) {
        if (command->completion == CommandCompletion::models)
            readline_candidates = &readline_models;
        else if (command->completion == CommandCompletion::efforts)
            readline_candidates = &readline_efforts;
        else if (command->completion == CommandCompletion::filenames) {
            rl_attempted_completion_over = 0;
            return nullptr;
        }
    } else if (before.empty() || before[0] != '/') {
        rl_attempted_completion_over = 0;
        return nullptr;
    }
    // libedit uses `char*` here on Linux and `const char*` on macOS.
    return readline_candidates
               ? completion_matches(const_cast<char*>(text), completion_candidate)
               : nullptr;
}

inline int esc_getc(FILE* file) {
    if (readline_initial_pos < readline_initial.size())
        return static_cast<unsigned char>(readline_initial[readline_initial_pos++]);
    if (esc_pending) {
        int next = esc_pending;
        esc_pending = 0;
        return next;
    }
    int fd = file ? fileno(file) : 0;
    unsigned char ch;
    ssize_t size;
    do size = read(fd, &ch, 1);
    while (size < 0 && errno == EINTR);
    if (size <= 0) return EOF;
    if (ch == 0x1b) {
        pollfd event{fd, POLLIN, 0};
        if (poll(&event, 1, 50) == 0) {
            if (steering_prompt) {
                steering_cancelled = true;
                return '\n';
            }
            esc_pending = 0x0b;
            return 0x01;
        }
    }
    return ch;
}

inline void register_completion(CommandCompletion source, const std::string& value) {
    std::vector<std::string>* values = nullptr;
    if (source == CommandCompletion::models) values = &readline_models;
    if (source == CommandCompletion::efforts) values = &readline_efforts;
    if (!values || value.empty() ||
        std::find(values->begin(), values->end(), value) != values->end())
        return;
    values->push_back(value);
}

inline void configure_completion(const std::vector<std::string>& models,
                                 const std::vector<std::string>& efforts) {
    readline_commands.clear();
    readline_models = models;
    readline_efforts = efforts;
    for (const SlashCommandSpec& command : SLASH_COMMANDS)
        if (command.visible) readline_commands.push_back(command.name);
    std::sort(readline_models.begin(), readline_models.end());
    std::sort(readline_efforts.begin(), readline_efforts.end());
    readline_models.erase(std::unique(readline_models.begin(), readline_models.end()),
                          readline_models.end());
    readline_efforts.erase(std::unique(readline_efforts.begin(), readline_efforts.end()),
                           readline_efforts.end());
    rl_attempted_completion_function = uagent_completion;
}

inline std::string rl_hide_escapes(const std::string& prompt) {
    std::string out;
    for (size_t i = 0; i < prompt.size();) {
        if (prompt[i] == '\033') {
            size_t end = i + 1;
            if (end < prompt.size() && prompt[end] == '[') {
                for (++end; end < prompt.size() && !isalpha((unsigned char)prompt[end]); ++end) {}
                if (end < prompt.size()) ++end;
            }
            out += '\001';
            out.append(prompt, i, end - i);
            out += '\002';
            i = end;
        } else {
            out += prompt[i++];
        }
    }
    return out;
}
#endif

inline std::string panel_prompt(const char* label = "") {
    if (!g_tty) return std::string(label) + "> ";
    return std::string(PANEL()) + (label && *label ? std::string(label) + "> " : "> ");
}

inline std::string read_input_line(const std::string& prompt, bool* eof,
                                   bool keep_history = true,
                                   const std::string& initial = "") {
    (void)keep_history;  // used only when libedit is compiled in
    *eof = false;
#if defined(HAVE_EDITLINE)
    if (g_tty) {
        readline_initial = initial;
        readline_initial_pos = 0;
        std::string hidden_prompt = rl_hide_escapes(prompt);
        char* line = readline(hidden_prompt.c_str());
        readline_initial.clear();
        readline_initial_pos = 0;
        fputs(RST(), stdout);
        fputs("\r\033[K", stdout);
        if (!line) {
            *eof = true;
            return "";
        }
        std::string input = line;
        free(line);
        if (keep_history && !trim(input).empty()) add_history(input.c_str());
        return input;
    }
#endif
    fputs(prompt.c_str(), stdout);
    fputs(initial.c_str(), stdout);
    fflush(stdout);
    std::string input;
    if (!std::getline(std::cin, input)) {
        fputs(RST(), stdout);
        *eof = true;
        return "";
    }
    fputs(RST(), stdout);
    return initial + input;
}

inline std::string steering_replacement(bool& cancelled) {
    std::string input;
    do {
        bool eof = false;
        panel_clear_line();
#if defined(HAVE_EDITLINE)
        steering_prompt = true;
        steering_cancelled = false;
#endif
        input = trim(read_input_line(panel_prompt("steer"), &eof, false));
#if defined(HAVE_EDITLINE)
        cancelled = steering_cancelled;
        steering_prompt = steering_cancelled = false;
        if (!cancelled && !input.empty()) add_history(input.c_str());
#else
        cancelled = input.find('\x1b') != std::string::npos;
#endif
        if (eof) cancelled = true;
    } while (!cancelled && input.empty());
    return cancelled ? "" : input;
}
