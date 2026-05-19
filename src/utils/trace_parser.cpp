#include "utils/trace_parser.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace cas::utils {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

// Parses a hex address token, tolerating an optional "0x"/"0X" prefix.
[[nodiscard]] bool parse_hex_address(std::string_view token, std::uint64_t& out) {
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        token.remove_prefix(2);
    }
    if (token.empty()) return false;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, 16);
    return ec == std::errc{} && ptr == end;
}

[[nodiscard]] bool parse_decimal_size(std::string_view token, std::size_t& out) {
    if (token.empty()) return false;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    auto [ptr, ec] = std::from_chars(begin, end, out, 10);
    return ec == std::errc{} && ptr == end;
}

} // namespace

std::optional<TraceEntry> TraceParser::parse_line(std::string_view line) {
    const std::string_view trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
        return std::nullopt; // blank / comment line — not an error
    }

    // Tokenize on whitespace: expect exactly <op> <addr> <size>.
    std::size_t p1 = trimmed.find_first_of(" \t");
    if (p1 == std::string_view::npos) return std::nullopt;
    std::string_view op_tok = trimmed.substr(0, p1);

    std::size_t rest_start = trimmed.find_first_not_of(" \t", p1);
    if (rest_start == std::string_view::npos) return std::nullopt;
    std::string_view rest = trimmed.substr(rest_start);

    std::size_t p2 = rest.find_first_of(" \t");
    if (p2 == std::string_view::npos) return std::nullopt;
    std::string_view addr_tok = rest.substr(0, p2);

    std::size_t size_start = rest.find_first_not_of(" \t", p2);
    if (size_start == std::string_view::npos) return std::nullopt;
    std::string_view size_tok = trim(rest.substr(size_start));
    // A size token might be followed by trailing garbage; only take the
    // first whitespace-delimited chunk.
    if (auto sp = size_tok.find_first_of(" \t"); sp != std::string_view::npos) {
        size_tok = size_tok.substr(0, sp);
    }

    if (op_tok.size() != 1) return std::nullopt;

    cache::AccessType type;
    switch (op_tok[0]) {
        case 'L': case 'l': type = cache::AccessType::Read; break;
        case 'S': case 's': type = cache::AccessType::Write; break;
        case 'M': case 'm': type = cache::AccessType::Write; break; // modify collapsed to write (see header docs)
        case 'I': case 'i': type = cache::AccessType::InstructionFetch; break;
        default: return std::nullopt;
    }

    std::uint64_t address = 0;
    if (!parse_hex_address(addr_tok, address)) return std::nullopt;

    std::size_t size = 1;
    if (!parse_decimal_size(size_tok, size)) return std::nullopt;
    if (size == 0) return std::nullopt;

    return TraceEntry{.type = type, .address = address, .size = size};
}

TraceParseResult TraceParser::parse_lines(std::span<const std::string> lines) {
    TraceParseResult result;
    result.entries.reserve(lines.size());

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view raw = lines[i];
        const std::string_view trimmed = trim(raw);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (auto entry = parse_line(raw); entry.has_value()) {
            result.entries.push_back(*entry);
        } else {
            result.errors.push_back(TraceParseError{
                .line_number = i + 1, .raw_line = std::string(raw), .reason = "malformed trace line"});
        }
    }
    return result;
}

TraceParseResult TraceParser::parse_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("TraceParser::parse_file: could not open " + path.string());
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(std::move(line));
    }
    return parse_lines(lines);
}

} // namespace cas::utils
