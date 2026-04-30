#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <optional>
#include <variant>
#include <functional>
#include <cassert>
#include <format>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <ranges>
#include <span>
#include <algorithm>
#include <cstdint>

struct SourceLoc {
    std::string file;
    int         line = 0;
    int         col  = 0;
    int         len  = 1;
};

enum class DiagLevel { Error, Warning, Note };

struct Diagnostic {
    DiagLevel   level;
    SourceLoc   loc;
    std::string message;
    std::string lineText;
};

struct SourceFile {
    std::string              path;
    std::string              src;
    std::vector<std::string> lines;

    struct OriginalLoc {
        std::string file;
        int         line = 0;
    };
    std::vector<OriginalLoc> lineMap;

    OriginalLoc mapLine(int mergedLine) const {
        if (!lineMap.empty() &&
            mergedLine >= 1 &&
            mergedLine < static_cast<int>(lineMap.size()))
        {
            return lineMap[mergedLine];
        }
        return {path, mergedLine};
    }

    static std::shared_ptr<SourceFile> load(const std::string& path) {
        std::ifstream f(path);
        if (!f) return nullptr;
        std::ostringstream ss;
        ss << f.rdbuf();
        auto sf = std::make_shared<SourceFile>();
        sf->path = path;
        sf->src  = ss.str();
        sf->lines.push_back("");
        std::string line;
        std::istringstream in(sf->src);
        while (std::getline(in, line))
            sf->lines.push_back(line);
        return sf;
    }

    std::string getLine(int lineNum) const {
        if (lineNum < 1 || lineNum >= (int)lines.size()) return {};
        return lines[lineNum];
    }
};

class DiagEngine {
public:
    std::vector<Diagnostic> diags;
    bool hasErrors = false;

    void emit(DiagLevel lvl, const SourceLoc& loc,
              std::string msg, std::string lineText = {}) {
        diags.push_back({lvl, loc, std::move(msg), std::move(lineText)});
        if (lvl == DiagLevel::Error) hasErrors = true;
    }

    void error(const SourceLoc& loc, std::string msg,
               std::string lineText = {}) {
        emit(DiagLevel::Error, loc, std::move(msg), std::move(lineText));
    }

    void warning(const SourceLoc& loc, std::string msg,
                 std::string lineText = {}) {
        emit(DiagLevel::Warning, loc, std::move(msg), std::move(lineText));
    }

    void note(const SourceLoc& loc, std::string msg,
              std::string lineText = {}) {
        emit(DiagLevel::Note, loc, std::move(msg), std::move(lineText));
    }

    void printAll(const SourceFile* sf = nullptr) const {
        static const char* reset  = "\033[0m";
        static const char* bold   = "\033[1m";
        static const char* red    = "\033[1;31m";
        static const char* yellow = "\033[1;33m";
        static const char* cyan   = "\033[1;36m";
        static const char* green  = "\033[1;32m";
        static const char* dim    = "\033[2m";
        static const char* blue   = "\033[1;34m";

        for (auto& d : diags) {
            SourceLoc displayLoc = d.loc;
            if (sf && !sf->lineMap.empty()) {
                auto mapped = sf->mapLine(d.loc.line);
                if (!mapped.file.empty() && mapped.line > 0) {
                    displayLoc.file = mapped.file;
                    displayLoc.line = mapped.line;
                }
            }

            const char* lvlColor = (d.level == DiagLevel::Error)   ? red
                                 : (d.level == DiagLevel::Warning)  ? yellow
                                                                     : cyan;
            const char* lvlStr   = (d.level == DiagLevel::Error)   ? "error"
                                 : (d.level == DiagLevel::Warning)  ? "warning"
                                                                     : "note";

            std::cerr << lvlColor << bold << lvlStr << reset
                      << bold << ": " << d.message << reset << "\n";

            std::cerr << blue << "  --> " << reset
                      << displayLoc.file << ":" << displayLoc.line
                      << ":" << displayLoc.col << "\n";

            if (!d.lineText.empty()) {
                constexpr int kTabWidth = 4;

                std::string expanded;
                int displayCol = 0;
                int rawCol     = 0;
                for (char ch : d.lineText) {
                    ++rawCol;
                    if (ch == '\t') {
                        int spaces = kTabWidth - (int(expanded.size()) % kTabWidth);
                        expanded += std::string(spaces, ' ');
                        if (rawCol <= d.loc.col) displayCol = (int)expanded.size();
                    } else {
                        expanded += ch;
                        if (rawCol <= d.loc.col) displayCol = (int)expanded.size();
                    }
                }
                if (d.loc.col > rawCol) displayCol = (int)expanded.size();

                std::string lineNumStr = std::to_string(displayLoc.line);
                int gutterWidth = std::max(4, (int)lineNumStr.size());
                std::string numPad(gutterWidth - (int)lineNumStr.size(), ' ');
                std::string gutter      = "  " + numPad + lineNumStr + " | ";
                std::string blankGutter(gutter.size(), ' ');

                std::cerr << blue << dim << gutter << reset << expanded << "\n";

                int caretOffset = std::max(0, displayCol - 1);
                std::string caretLine = blankGutter + std::string(caretOffset, ' ');
                caretLine += '^';
                if (d.loc.len > 1)
                    caretLine += std::string(d.loc.len - 1, '~');

                std::cerr << green << bold << caretLine << reset << "\n";
            }

            std::cerr << "\n";
        }
    }

    int errorCount() const {
        return static_cast<int>(
            std::ranges::count_if(diags,
                [](auto& d){ return d.level == DiagLevel::Error; }));
    }
};

inline std::optional<std::string> readFile(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
