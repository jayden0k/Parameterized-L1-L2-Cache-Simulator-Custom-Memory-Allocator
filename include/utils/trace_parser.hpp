// ============================================================================
// trace_parser.hpp
//
// Parses memory access trace logs in the common Dinero/Valgrind `lackey`
// style, one access per line:
//
//     <op> <hex-address> <size-in-bytes>
//
// where <op> is one of:
//     L  - Load  (Read)
//     S  - Store (Write)
//     M  - Modify (Read followed by Write; recorded here as a single Write
//                  access for simplicity, matching common Dinero readers
//                  that only track the final effect on the line)
//     I  - Instruction fetch
//
// Examples:
//     S 0x1fffff50 4
//     L 0x7ffe4200 8
//     I 0x400120   4
//
// Blank lines and lines beginning with '#' are treated as comments and
// skipped. Malformed lines are reported (with line number) rather than
// silently ignored or thrown, so a single corrupt line does not abort an
// otherwise-valid multi-million-line trace.
// ============================================================================
#pragma once

#include "cache/cache.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cas::utils {

struct TraceEntry {
    cache::AccessType type = cache::AccessType::Read;
    std::uint64_t address = 0;
    std::size_t size = 1;
};

struct TraceParseError {
    std::size_t line_number = 0;
    std::string raw_line;
    std::string reason;
};

struct TraceParseResult {
    std::vector<TraceEntry> entries;
    std::vector<TraceParseError> errors; // non-fatal; parsing continues past bad lines
};

class TraceParser {
public:
    // Parses a single trace line. Returns std::nullopt (rather than
    // throwing) if the line is blank/comment-only or malformed — callers
    // that need the failure reason should use parse_file()/parse_lines()
    // which aggregate TraceParseError diagnostics.
    [[nodiscard]] static std::optional<TraceEntry> parse_line(std::string_view line);

    // Parses an in-memory range of lines (e.g. already read into a vector,
    // or streamed from anywhere other than a plain file).
    [[nodiscard]] static TraceParseResult parse_lines(std::span<const std::string> lines);

    // Parses an entire trace file from disk. Throws std::runtime_error if
    // the file cannot be opened; malformed individual lines are collected
    // in the returned TraceParseResult::errors rather than throwing.
    [[nodiscard]] static TraceParseResult parse_file(const std::filesystem::path& path);
};

} // namespace cas::utils
