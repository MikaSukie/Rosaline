#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "codegen.hpp"
#include "import_resolver.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>

namespace fs = std::filesystem;

static void printUsage(const char* argv0) {
    std::cerr <<
R"(Rosaline compiler Usage: )" << argv0 << R"( [options] <input.rosi>

Options:
  -o <file>       Output file (default: stdout)
  --ast-dump      Print AST to stdout and exit
  --lex-dump      Print token stream to stdout and exit
  --no-sema       UNSAFE: skip all semantic analysis and type checking;
                  emit IR regardless of errors (for FFI/debugging only)
  -h, --help      Show this help message
)";
}

int main(int argc, char** argv) {
    std::string inputFile;
    std::string outputFile;
    bool dumpAst  = false;
    bool dumpLex  = false;
    bool noSema   = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--ast-dump") { dumpAst = true; }
        else if (arg == "--lex-dump") { dumpLex = true; }
        else if (arg == "--no-sema")  { noSema  = true; }
        else if (arg == "-o") {
            if (++i >= argc) {
                std::cerr << "error: -o requires an argument\n";
                return 1;
            }
            outputFile = argv[i];
        }
        else if (arg.starts_with("-o")) {
            outputFile = arg.substr(2);
        }
        else if (arg.starts_with("-")) {
            std::cerr << "error: unknown option '" << arg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
        else {
            if (!inputFile.empty()) {
                std::cerr << "error: multiple input files not supported\n";
                return 1;
            }
            inputFile = arg;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "error: no input file\n";
        printUsage(argv[0]);
        return 1;
    }

    fs::path inputPath = fs::weakly_canonical(inputFile);
    if (!fs::exists(inputPath)) {
        std::cerr << "error: file not found: " << inputFile << "\n";
        return 1;
    }

    auto rawSrc = readFile(inputPath);
    if (!rawSrc) {
        std::cerr << "error: cannot read file: " << inputFile << "\n";
        return 1;
    }

    DiagEngine diag;

    ImportResolver importer(inputPath.parent_path(), diag);
    std::shared_ptr<SourceFile> sf;
    std::string mergedSrc = importer.resolve(*rawSrc, inputPath, sf);

    if (diag.hasErrors) {
        diag.printAll(sf.get());
        return 1;
    }

    Lexer lexer(mergedSrc, inputPath.string(), diag);
    lexer.lex();

    if (diag.hasErrors) {
        diag.printAll(sf.get());
        return 1;
    }

    if (dumpLex) {
        lexer.dump();
        return 0;
    }

    Parser parser(lexer.tokens(), diag, sf);
    auto prog = parser.parse();

    if (diag.hasErrors) {
        diag.printAll(sf.get());
        return 1;
    }

    if (dumpAst) {
        dumpProgram(*prog);
        return 0;
    }

    if (!noSema) {
        SemaPass sema(*prog, diag, sf);
        bool ok = sema.run();

        diag.printAll(sf.get());
        if (!ok) return 1;

        std::string tdl, ttriple;
        for (auto& d : prog->directives) {
            if (d.key == "tdl")     tdl     = d.value;
            if (d.key == "ttriple") ttriple = d.value;
        }

        CodeGen cg(*prog, sema, diag, sf, tdl, ttriple);
        std::string ir = cg.emit();

        if (diag.hasErrors) {
            diag.printAll(sf.get());
            return 1;
        }

        if (outputFile.empty() || outputFile == "-") {
            std::cout << ir;
        } else {
            std::ofstream out(outputFile);
            if (!out) {
                std::cerr << "error: cannot open output file: " << outputFile << "\n";
                return 1;
            }
            out << ir;
            if (!out) {
                std::cerr << "error: write failed: " << outputFile << "\n";
                return 1;
            }
        }
    } else {
        std::cerr << "warning: --no-sema is active: all type checking, "
                     "undeclared identifier/function checks, and semantic "
                     "validation are DISABLED. Output IR may be invalid.\n";

        SemaPass sema(*prog, diag, sf);
        sema.run();

        for (auto& d2 : diag.diags) {
            if (d2.level != DiagLevel::Error) {
                DiagEngine tmp;
                tmp.emit(d2.level, d2.loc, d2.message, d2.lineText);
                tmp.printAll(sf.get());
            }
        }

        std::string tdl, ttriple;
        for (auto& d : prog->directives) {
            if (d.key == "tdl")     tdl     = d.value;
            if (d.key == "ttriple") ttriple = d.value;
        }

        CodeGen cg(*prog, sema, diag, sf, tdl, ttriple);
        std::string ir = cg.emit();

        if (outputFile.empty() || outputFile == "-")
            std::cout << ir;
        else {
            std::ofstream out(outputFile);
            out << ir;
        }
    }

    return 0;
}