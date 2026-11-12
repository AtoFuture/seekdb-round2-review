#include "minivectordb/database.hpp"
#include "protocol/jsonl.hpp"
#include <charconv>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {
std::string escape(std::string_view input) {
    std::string out;
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\0': out += "\\0"; break;
        default: out += c;
        }
    }
    return out;
}
std::string format(const minivectordb::Cell& cell) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) return "s:" + escape(value);
        else if constexpr (std::is_same_v<T, std::int64_t>) return "i:" + std::to_string(value);
        else {
            std::ostringstream out;
            out.imbue(std::locale::classic());
            if constexpr (std::is_same_v<T, double>)
                out << "d:" << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
            else {
                out << "v:[" << std::setprecision(std::numeric_limits<float>::max_digits10);
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (i) out << ',';
                    out << value[i];
                }
                out << ']';
            }
            return out.str();
        }
    }, cell);
}
const char* error_name(minivectordb::ErrorCode code) {
    using minivectordb::ErrorCode;
    switch (code) {
    case ErrorCode::Syntax: return "SYNTAX";
    case ErrorCode::Schema: return "SCHEMA";
    case ErrorCode::Type: return "TYPE";
    case ErrorCode::Constraint: return "CONSTRAINT";
    case ErrorCode::Transaction: return "TRANSACTION";
    case ErrorCode::Corruption: return "CORRUPTION";
    }
    return "INTERNAL";
}
struct Arguments {
    std::filesystem::path directory = "/data";
    std::size_t sort_memory = 8 * 1024 * 1024;
    bool protocol = false; // true selects explicit dev-tsv
};
Arguments arguments(int argc, char** argv) {
    Arguments args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (i + 1 >= argc) throw std::invalid_argument("missing option value");
        const std::string_view value(argv[++i]);
        if (flag == "--protocol" && value == "dev-tsv") args.protocol = true;
        else if (flag == "--protocol" && value == "jsonl") args.protocol = false;
        else if (flag == "--data-dir") args.directory = std::string(value);
        else if (flag == "--sort-memory") {
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), args.sort_memory);
            if (error != std::errc{} || end != value.data() + value.size() || args.sort_memory < 4096)
                throw std::invalid_argument("--sort-memory must be an integer >=4096");
        } else throw std::invalid_argument("unknown option or protocol");
    }
    if (args.directory.empty()) throw std::invalid_argument("--data-dir cannot be empty");
    return args;
}
int serve(minivectordb::Database& db) {
    enum class State { normal, quoted, identifier, line_comment, block_comment };
    State state = State::normal;
    std::string statement;
    bool has_code = false;
    bool failed = false;
    constexpr std::size_t max_statement_bytes = 16 * 1024 * 1024;
    auto append = [&](char c) {
        if (statement.size() == max_statement_bytes)
            throw std::length_error("development SQL frame exceeds 16 MiB");
        statement += c;
    };
    auto execute = [&] {
        if (has_code) {
            bool result = false;
            std::uint64_t count = 0;
            minivectordb::QuerySink sink;
            sink.columns = [&](const std::vector<std::string>& columns) {
                result = true;
                std::cout << "RESULT";
                for (const auto& column : columns) std::cout << '\t' << escape(column);
                std::cout << '\n';
            };
            sink.row = [&](const minivectordb::Row& row) {
                std::cout << "ROW";
                for (const auto& cell : row) std::cout << '\t' << format(cell);
                std::cout << '\n';
                ++count;
            };
            try {
                const auto info = db.execute(statement, sink);
                if (result) std::cout << "END\t" << count << '\n';
                else std::cout << "OK\t" << info.affected_rows << '\n';
            } catch (const minivectordb::Error& error) {
                std::cout << "ERROR\t" << error_name(error.code()) << '\t' << escape(error.what()) << '\n';
                failed = true;
            } catch (const std::exception& error) {
                std::cout << "ERROR\tEXECUTION\t" << escape(error.what()) << '\n';
                failed = true;
            }
            std::cout.flush();
        }
        statement.clear();
        has_code = false;
    };
    char c;
    while (std::cin.get(c)) {
        if (state == State::normal) {
            if (c == ';') { execute(); continue; }
            append(c);
            if (c == '-' && std::cin.peek() == '-') {
                append(static_cast<char>(std::cin.get()));
                state = State::line_comment;
            } else if (c == '/' && std::cin.peek() == '*') {
                append(static_cast<char>(std::cin.get()));
                state = State::block_comment;
            } else if (c == '\'' || c == char(96)) {
                state = c == '\'' ? State::quoted : State::identifier;
                has_code = true;
            } else if (!std::isspace(static_cast<unsigned char>(c))) has_code = true;
        } else {
            append(c);
            if (state == State::line_comment && c == '\n') state = State::normal;
            else if (state == State::block_comment && c == '*' && std::cin.peek() == '/') {
                append(static_cast<char>(std::cin.get()));
                state = State::normal;
            } else if ((state == State::quoted && c == '\'') ||
                       (state == State::identifier && c == char(96))) {
                if (std::cin.peek() == c) append(static_cast<char>(std::cin.get()));
                else state = State::normal;
            }
        }
    }
    if (std::cin.bad()) throw std::runtime_error("SQL input read failed");
    if (state == State::quoted || state == State::identifier || state == State::block_comment) {
        std::cout << "ERROR\tSYNTAX\tunterminated SQL quote or comment\n" << std::flush;
        failed = true;
    } else execute();
    return failed ? 1 : 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto args = arguments(argc, argv);
        std::filesystem::create_directories(args.directory);
        std::cout.exceptions(std::ios::badbit | std::ios::failbit);
        auto db = std::make_unique<minivectordb::Database>(args.directory / "database.mvdb",
                                minivectordb::Options{args.directory / "tmp", args.sort_memory});
        return args.protocol ? serve(*db) : minivectordb::jsonl::serve(db, args.directory);
    } catch (const std::exception& error) {
        std::cerr << "MiniVectorDB: " << error.what()
                  << "\nUsage: minivectordb [--protocol jsonl|dev-tsv] [--data-dir /data] [--sort-memory bytes]\n";
        return 2;
    }
}
