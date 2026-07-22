#pragma once
// Streaming Markdown -> ANSI renderer for model output. Inline styles
// (**bold**, *italic*, `code`), headings, bullets and fenced code blocks
// render as tokens arrive — no full-message buffering, the stream stays
// live. Tables are the one construct held back until complete, because
// column alignment needs every row; headerless-pipe tables ("A | B" over
// "---|---") are caught retroactively by erasing the already-printed header
// line and re-rendering. TTY only (piped output stays byte-exact);
// UAGENT_MARKDOWN=0 disables.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

#include "util.hpp"

struct MdStream {
    bool on;

    // inline state
    bool bold = false, ital = false, code = false;
    int star = 0;  // 1: lone '*' unresolved · 2: "**" seen, opening needs a non-space
    // line state
    bool linestart = true;
    std::string pre;       // held line prefix while it may still be structural
    bool heading = false;
    bool fence = false;     // inside a ``` block
    bool fencehead = false; // on the ``` marker line itself (rendered dim)
    // table state
    bool intable = false;   // buffering the current line: it started with '|'
    bool tablemode = false; // buffering lines of a headerless-pipe table
    std::string row;
    std::vector<std::string> table;
    // previous plain line, for retroactive table capture
    std::string cur_raw, prev_raw;
    size_t vis_line = 0, prev_vis = 0;  // visible columns printed on the line

    MdStream() { on = g_tty && env_str("UAGENT_MARKDOWN", "1") != "0"; }

    void feed(const std::string& s) {
        std::string safe = terminal_safe(s);
        if (!on) { fputs(safe.c_str(), stdout); fflush(stdout); return; }
        for (char c : safe) step(c);
        fflush(stdout);
    }

    void flush() {  // stream end: resolve everything still held
        if (!on) return;
        if (star) { pv(star == 2 ? "**" : "*"); star = 0; }
        if (intable || tablemode) {
            if (!row.empty()) table.push_back(row);
            row.clear();
            intable = tablemode = false;
        }
        flush_table();
        if (!pre.empty()) pv(pre), pre.clear();
        if (bold || ital || code || heading || fence || fencehead) fputs(RST(), stdout);
        bold = ital = code = heading = fence = fencehead = false;
        linestart = true;
        fflush(stdout);
    }

private:
    static size_t ulen(const std::string& s) { return display_width(s); }
    static int term_width() {
        struct winsize w;
        return (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) ? w.ws_col : 80;
    }
    void pv(const std::string& s) { fputs(s.c_str(), stdout); vis_line += ulen(s); }
    void pc(char c) { putchar(c); if (((unsigned char)c & 0xC0) != 0x80) vis_line++; }
    void emit_pre() { pv(pre); pre.clear(); }
    std::string marker() const {  // pre minus its leading indentation
        size_t sp = pre.find_first_not_of(" \t");
        return sp == std::string::npos ? "" : pre.substr(sp);
    }

    void step(char c) {
        if (!intable && !tablemode && !fence && !fencehead && c != '\n') cur_raw += c;
        if (intable) {  // line started with '|': one table row
            if (c == '\n') {
                table.push_back(row);
                row.clear(); cur_raw.clear(); vis_line = 0;
                intable = false; linestart = true;
            } else row += c;
            return;
        }
        if (tablemode) {  // headerless-pipe table body: rows until a line without '|'
            if (c != '\n') { row += c; return; }
            if (row.find('|') != std::string::npos) { table.push_back(row); row.clear(); return; }
            std::string tail = row;
            row.clear(); tablemode = false;
            flush_table();
            linestart = true;
            for (char t : tail) step(t);
            step('\n');
            return;
        }
        if (fencehead) {  // rest of the ``` marker line, shown dim
            if (c == '\n') {
                fputs(RST(), stdout); putchar('\n');
                if (fence) fputs(CODE_BLK(), stdout);  // color the block body
                fencehead = false; linestart = true;
                prev_raw.clear(); prev_vis = 0;
            } else putchar(c);
            return;
        }
        if (linestart) { classify(c); return; }
        if (fence) {
            putchar(c);
            if (c == '\n') { linestart = true; prev_raw.clear(); prev_vis = 0; }
            return;
        }
        inline_char(c);
    }

    // decide what kind of line this is from its first non-space characters
    void classify(char c) {
        if (fence) { fence_classify(c); return; }
        std::string mk = marker();
        if (mk.empty()) {  // only indentation so far
            if (c == ' ' || c == '\t') { pre += c; return; }
            if (!table.empty() && c != '|') flush_table();  // table ended on this line
            if (c == '#' || c == '`' || c == '-' || c == '*') { pre += c; return; }
            if (c == '|') { row = pre + c; pre.clear(); cur_raw.clear(); intable = true; return; }
            emit_pre();
            if (c == '\n') { end_line(); return; }  // blank line
            linestart = false;
            inline_char(c);
            return;
        }
        char m = mk[0];
        if (m == '#') {
            if (c == '#' && mk.size() < 6) { pre += c; return; }
            if (c == ' ') {  // "# ..." -> bold, hashes hidden
                pv(pre.substr(0, pre.size() - mk.size()));  // indentation
                fputs(BOLD(), stdout);
                heading = true; pre.clear(); linestart = false;
                return;
            }
            emit_pre();  // "#hashtag" etc: plain text
            linestart = false;
            inline_char(c);
            return;
        }
        if (m == '`') {
            if (c == '`' && mk.size() < 3) {
                pre += c;
                if (marker() == "```") {  // fence opens
                    fputs(DIM(), stdout); pv(pre); pre.clear();
                    fence = true; fencehead = true; linestart = false;
                }
                return;
            }
            // 1-2 backticks then something else: inline code span(s)
            std::string held = pre;
            pre.clear(); linestart = false;
            for (char h : held)
                if (h == '`') inline_char('`'); else pc(h);
            inline_char(c);
            return;
        }
        // m is '-' or '*' (held marker): bullet if a space follows the single char
        if (mk.size() == 1 && c == ' ') {
            pv(pre.substr(0, pre.size() - 1));
            pv("• ");
            pre.clear(); linestart = false;
            return;
        }
        if (m == '-') {
            // could still be a |-separator row ("---|---", "--- | ---")
            bool seppish = mk.find_first_not_of("-:| ") == std::string::npos;
            if (seppish && (c == '-' || c == ':' || c == '|' || (c == ' ' && mk.size() > 1))) {
                pre += c;
                return;
            }
            if (c == '\n') {
                if (seppish && mk.find('|') != std::string::npos &&
                    prev_raw.find('|') != std::string::npos) { retro_table(); return; }
                emit_pre();
                end_line();
                return;
            }
            emit_pre();
            linestart = false;
            inline_char(c);
            return;
        }
        pre.pop_back();  // '*' was an emphasis marker, not a bullet
        emit_pre();
        linestart = false;
        star = 1;
        inline_char(c);
    }

    void fence_classify(char c) {  // inside a fence only "```" is structural
        std::string mk = marker();
        if (mk.size() >= 3) {  // "```" held: it only closes if the line ends here
            if (c == ' ') { pre += c; return; }
            if (c == '\n') {
                fputs(DIM(), stdout); fputs(pre.c_str(), stdout); pre.clear();
                fputs(RST(), stdout); putchar('\n');
                fence = false;
                return;  // linestart stays true
            }
            fputs(pre.c_str(), stdout); pre.clear();  // "```something": content
            putchar(c);
            linestart = false;
            return;
        }
        if (c == ' ' && mk.empty()) { pre += c; return; }
        if (c == '`') { pre += c; return; }
        fputs(pre.c_str(), stdout); pre.clear();
        if (c == '\n') { putchar('\n'); prev_raw.clear(); return; }
        putchar(c);
        linestart = false;
    }

    void inline_char(char c) {
        if (star) {
            int n = star;
            star = 0;
            if (n == 1 && c == '*') {  // "**"
                if (bold) { bold = false; fputs(BOLD_OFF(), stdout); }
                else star = 2;  // opening bold only if a non-space follows
                return;
            }
            if (n == 2) {
                if (c == ' ' || c == '\n') pv("**");  // "a ** b": literal
                else { bold = true; fputs(BOLD(), stdout); }
            } else {
                if (ital) { ital = false; fputs(ITAL_OFF(), stdout); }
                else if (c != ' ' && c != '\n') { ital = true; fputs(ITAL(), stdout); }
                else pv("*");  // "2 * 3": literal
            }
        }
        if (c == '*' && !code) { star = 1; return; }
        if (c == '`') { code = !code; fputs(code ? CODE() : FG_DFL(), stdout); return; }
        if (c == '\n') { end_line(); return; }
        pc(c);
    }

    void end_line() {  // inline styles never span lines
        if (bold || ital || code || heading) {
            fputs(RST(), stdout);
            bold = ital = code = heading = false;
        }
        putchar('\n');
        prev_raw = cur_raw; prev_vis = vis_line;
        cur_raw.clear(); vis_line = 0;
        linestart = true;
    }

    // --- tables --------------------------------------------------------------

    // "A | B" was already printed as prose and a |-separator just completed:
    // erase the printed header line and rebuild the table from raw text
    void retro_table() {
        int w = term_width();
        int rows = prev_vis ? (int)((prev_vis - 1) / (size_t)w) + 1 : 1;
        for (int i = 0; i < rows; i++) fputs("\033[A\033[2K", stdout);  // up + clear
        table.push_back(prev_raw);
        table.push_back(marker());
        pre.clear(); cur_raw.clear(); vis_line = 0;
        prev_raw.clear(); prev_vis = 0;
        tablemode = true;  // body rows follow until a line without '|'
    }

    // Render inline Markdown and measure real terminal columns for wide and
    // combining characters.
    static std::string render_cell(const std::string& s, size_t& vis) {
        std::string out;
        std::string visible;
        bool b = false, i = false, cd = false;
        for (size_t k = 0; k < s.size(); k++) {
            char c = s[k];
            if (c == '`') { cd = !cd; out += cd ? CODE() : FG_DFL(); continue; }
            if (c == '*' && !cd) {
                bool tight = k + 1 < s.size() && s[k + 1] != ' ';  // flanked: a marker
                if (k + 1 < s.size() && s[k + 1] == '*') { b = !b; out += b ? BOLD() : BOLD_OFF(); k++; }
                else if (i || tight) { i = !i; out += i ? ITAL() : ITAL_OFF(); }
                else { out += c; visible += c; }  // "2 * 3": literal
                continue;
            }
            out += c;
            visible += c;
        }
        vis = display_width(visible);
        if (b || i || cd) out += RST();
        return out;
    }

    void flush_table() {
        if (table.empty()) return;
        // no |---| separator row -> not actually a table (e.g. "| jq ."): pass through
        bool any_sep = false;
        for (auto& raw : table) {
            std::string t = trim(raw);
            if (t.size() > 1 && t.find_first_not_of("-:| ") == std::string::npos) any_sep = true;
        }
        if (!any_sep) {
            for (auto& raw : table) { fputs(raw.c_str(), stdout); putchar('\n'); }
            table.clear();
            return;
        }
        std::vector<std::vector<std::pair<std::string, size_t>>> rows;  // cell, visible width
        std::vector<bool> sep;
        std::vector<size_t> w;
        for (auto& raw : table) {
            std::string t = trim(raw);
            if (!t.empty() && t.front() == '|') t.erase(0, 1);
            if (!t.empty() && t.back() == '|') t.pop_back();
            std::vector<std::string> cells;
            std::string cur;
            for (char c : t) {
                if (c == '|') { cells.push_back(trim(cur)); cur.clear(); }
                else cur += c;
            }
            cells.push_back(trim(cur));
            bool is_sep = true;  // |---|:--:| alignment row
            for (auto& c : cells)
                if (c.find_first_not_of("-: ") != std::string::npos) is_sep = false;
            sep.push_back(is_sep);
            rows.emplace_back();
            for (size_t c = 0; c < cells.size(); c++) {
                size_t vis = 0;
                std::string out = is_sep ? "" : render_cell(cells[c], vis);
                rows.back().push_back({out, vis});
                if (c >= w.size()) w.resize(c + 1, 0);
                if (vis > w[c]) w[c] = vis;
            }
        }
        for (size_t r = 0; r < rows.size(); r++) {
            if (sep[r]) {
                std::string ln = "|";
                for (size_t c = 0; c < w.size(); c++) ln += std::string(w[c] + 2, '-') + "|";
                printf("%s%s%s\n", DIM(), ln.c_str(), RST());
                continue;
            }
            std::string ln;
            for (size_t c = 0; c < w.size(); c++) {
                auto cell = c < rows[r].size() ? rows[r][c] : std::make_pair(std::string(), (size_t)0);
                ln += std::string(DIM()) + "|" + RST() + " " + cell.first +
                      std::string(w[c] - cell.second + 1, ' ');
            }
            printf("%s%s|%s\n", ln.c_str(), DIM(), RST());
        }
        table.clear();
        prev_raw.clear(); prev_vis = 0;  // a table can't be a header row
    }
};

// one-shot render (for content that was held back during streaming)
inline void md_print(const std::string& s) {
    MdStream m;
    m.feed(s);
    m.flush();
}
