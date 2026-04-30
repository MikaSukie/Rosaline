#pragma once
#include "common.hpp"
#include <unordered_set>
#include <filesystem>

class ImportResolver {
public:
    ImportResolver(const std::filesystem::path& baseDir, DiagEngine& diag)
        : baseDir_(baseDir), diag_(diag)
    {
        lineMap_.push_back({"", 0});
    }

    std::string resolve(const std::string& src,
                        const std::filesystem::path& srcPath,
                        std::shared_ptr<SourceFile>& outSF);

private:
    std::filesystem::path        baseDir_;
    DiagEngine&                  diag_;
    std::unordered_set<std::string> visited_;
    std::string                  merged_;

    std::vector<SourceFile::OriginalLoc> lineMap_;

    void processFile(const std::filesystem::path& path, int depth = 0);

    struct ImportSpec {
        std::string spec;
        bool        isExplicit;
        SourceLoc   loc;
    };
    std::vector<ImportSpec> scanImports(const std::string& src,
                                         const std::string& filePath);

    std::optional<std::filesystem::path>
    findImport(const std::string& spec, bool isExplicit,
               const std::filesystem::path& fromDir, const SourceLoc& loc);

    std::string stripImports(const std::string& src);

    void appendStripped(const std::string& stripped,
                        const std::string& originalFile);
};

inline std::string ImportResolver::resolve(
    const std::string& src,
    const std::filesystem::path& srcPath,
    std::shared_ptr<SourceFile>& outSF)
{
    std::filesystem::path canonical = std::filesystem::weakly_canonical(srcPath);
    visited_.insert(canonical.string());

    auto imports = scanImports(src, srcPath.string());
    auto fromDir = srcPath.parent_path();

    for (auto& imp : imports) {
        auto found = findImport(imp.spec, imp.isExplicit, fromDir, imp.loc);
        if (!found) continue;
        std::string cstr = std::filesystem::weakly_canonical(*found).string();
        if (visited_.count(cstr)) continue;
        processFile(*found, 1);
    }

    appendStripped(stripImports(src), srcPath.string());

    auto sf = std::make_shared<SourceFile>();
    sf->path = srcPath.string();
    sf->src  = merged_;

    sf->lines.push_back("");
    std::istringstream iss(merged_);
    std::string line;
    while (std::getline(iss, line))
        sf->lines.push_back(line);

    sf->lineMap = std::move(lineMap_);

    outSF = sf;
    return merged_;
}

inline void ImportResolver::processFile(const std::filesystem::path& path, int depth) {
    if (depth > 64) {
        diag_.error({"<import>", 0, 0, 1},
            "import recursion depth exceeded, possible circular import chain");
        return;
    }

    std::string canonical = std::filesystem::weakly_canonical(path).string();
    if (visited_.count(canonical)) return;
    visited_.insert(canonical);

    auto src = readFile(path);
    if (!src) {
        diag_.error({"<import>", 0, 0, 1},
            std::format("cannot open imported file '{}'", path.string()));
        return;
    }

    auto imports = scanImports(*src, path.string());
    auto fromDir = path.parent_path();

    for (auto& imp : imports) {
        auto found = findImport(imp.spec, imp.isExplicit, fromDir, imp.loc);
        if (!found) continue;
        std::string cstr = std::filesystem::weakly_canonical(*found).string();
        if (visited_.count(cstr)) continue;
        processFile(*found, depth + 1);
    }

    std::string header = "// <imported from: " + path.string() + ">\n";
    merged_ += header;
    lineMap_.push_back({path.string(), 0});

    appendStripped(stripImports(*src), path.string());

    merged_ += "\n";
    lineMap_.push_back({path.string(), 0});
}

inline void ImportResolver::appendStripped(const std::string& stripped,
                                           const std::string& originalFile)
{
    merged_ += stripped;

    std::istringstream ss(stripped);
    std::string line;
    int lineNum = 0;
    while (std::getline(ss, line)) {
        ++lineNum;
        lineMap_.push_back({originalFile, lineNum});
    }
}

inline std::vector<ImportResolver::ImportSpec>
ImportResolver::scanImports(const std::string& src, const std::string& filePath) {
    std::vector<ImportSpec> result;
    std::istringstream iss(src);
    std::string line;
    int lineNum = 0;

    while (std::getline(iss, line)) {
        ++lineNum;
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string_view sv(line.data() + start, line.size() - start);

        if (!sv.starts_with("import")) continue;
        sv.remove_prefix(6);
        if (!sv.empty() && (sv[0] == ' ' || sv[0] == '\t')) {
            size_t semi = sv.rfind(';');
            if (semi == std::string_view::npos) continue;
            sv = sv.substr(0, semi);

            std::string specs(sv);
            std::istringstream specss(specs);
            std::string tok;
            while (std::getline(specss, tok, ',')) {
                size_t s = tok.find_first_not_of(" \t");
                size_t e = tok.find_last_not_of(" \t");
                if (s == std::string::npos) continue;
                tok = tok.substr(s, e - s + 1);

                ImportSpec imp;
                imp.loc = {filePath, lineNum, (int)start + 1, (int)tok.size()};
                if (!tok.empty() && tok.front() == '"' && tok.back() == '"') {
                    imp.spec       = tok.substr(1, tok.size() - 2);
                    imp.isExplicit = true;
                } else {
                    imp.spec       = tok;
                    imp.isExplicit = false;
                }
                result.push_back(imp);
            }
        }
    }
    return result;
}

inline std::optional<std::filesystem::path>
ImportResolver::findImport(const std::string& spec, bool isExplicit,
                            const std::filesystem::path& fromDir,
                            const SourceLoc& loc) {
    namespace fs = std::filesystem;

    if (isExplicit) {
        fs::path p1 = fromDir / spec;
        if (fs::exists(p1)) return p1;
        fs::path p2 = baseDir_ / spec;
        if (fs::exists(p2)) return p2;
        diag_.error(loc, std::format("cannot find imported file '{}'", spec));
        return std::nullopt;
    }

    for (auto& dir : {fromDir, baseDir_}) {
        fs::path ros  = dir / (spec + ".ros");
        fs::path rosi = dir / (spec + ".rosi");
        bool hasRos  = fs::exists(ros);
        bool hasRosi = fs::exists(rosi);

        if (hasRos && hasRosi) {
            diag_.error(loc,
                "ambiguous import '" + spec + "': both '" + spec +
                ".ros' and '" + spec + ".rosi' exist. "
                "Use an explicit import: import \"" + spec + ".ros\" or \"" + spec + ".rosi\"");
            return std::nullopt;
        }
        if (hasRos)  return ros;
        if (hasRosi) return rosi;
    }

    diag_.error(loc,
        std::format("cannot find module '{}' (tried {}.ros, {}.rosi)", spec, spec, spec));
    return std::nullopt;
}

inline std::string ImportResolver::stripImports(const std::string& src) {
    std::ostringstream out;
    std::istringstream iss(src);
    std::string line;
    while (std::getline(iss, line)) {
        size_t s = line.find_first_not_of(" \t");
        if (s != std::string::npos && line.substr(s, 6) == "import")
            out << "// (import stripped)\n";
        else
            out << line << "\n";
    }
    return out.str();
}