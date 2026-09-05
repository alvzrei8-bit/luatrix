/*
 * Luatrix Native Amalgamation
 * ----------------------------
 * A single-file C++17 Luatrix port of the public src/ pipeline from:
 * https://github.com/zeusssz/hercules-obfuscator
 *
 * The upstream project is Apache-2.0 licensed. This file keeps the upstream
 * attribution and is intended as a self-contained, dependency-free command
 * line obfuscator. It does not bundle or invoke the old lua_lexer.cpp VM.
 *
 * The implementation deliberately uses a lexical transformer instead of
 * regex-only rewrites. Strings, comments, long-bracket strings, identifiers,
 * and punctuation are tokenized before a pass changes source text.
 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace luatrix {

struct Feature {
    const char* key;
    const char* name;
    const char* description;
    int pipeline_order;
    bool lua_only;
};

static const std::vector<Feature> FEATURES = {
    {"dynamic_code",       "Dynamic Code",         "Runtime reconstruction and load indirection",          10, false},
    {"opaque_predicates",  "Opaque Predicates",    "Semantically stable opaque branches",                   20, false},
    {"string_encoding",    "String Encoding",      "Runtime string reconstruction",                         30, false},
    {"string_expressions", "String To Expressions", "Arithmetic expressions for string bytes",               40, false},
    {"function_inlining",  "Function Inlining",    "Inlining of safe constant-return helpers",              50, false},
    {"variable_renaming",  "Variable Renaming",    "Randomized local-symbol renaming",                       60, false},
    {"virtual_machine",    "Virtual Machine",      "Per-output randomized virtual opcode dispatcher",       70, true},
    {"antitamper",         "Anti Tamper",          "Runtime integrity checks for core functions",            80, false},
    {"control_flow",       "Control Flow",         "Opaque state guard around the program",                 90, false},
    {"garbage_code",       "Garbage Code",         "Dead decoy locals and branches",                        100, false},
    {"compressor",         "Compressor",           "Comment removal and safe whitespace packing",            110, false},
    {"wrap_in_function",   "Function Wrapping",    "Encapsulation in an immediately-called function",       120, false},
    {"bytecode_encoding",  "Bytecode Encoding",    "Encoded source payload loaded at runtime",               130, true},
    {"watermark",          "Watermark",            "LTRIX attribution header",                               140, false},
};

struct Options {
    std::string input;
    std::string output;
    std::string target = "auto";
    // Luatrix always runs every compatible pass. Its public interface is
    // intentionally only: luatrix <input> <out>.
};

struct Token {
    enum class Kind { Word, Number, String, Comment, Whitespace, Symbol };
    Kind kind;
    std::string text;
    std::size_t offset = 0;
};

static bool is_word_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static bool is_number_start(const std::string& s, std::size_t i) {
    return std::isdigit(static_cast<unsigned char>(s[i])) ||
           (s[i] == '.' && i + 1 < s.size() &&
            std::isdigit(static_cast<unsigned char>(s[i + 1])));
}

static std::size_t long_bracket_end(const std::string& s, std::size_t start) {
    if (start >= s.size() || s[start] != '[') return std::string::npos;
    std::size_t i = start + 1;
    while (i < s.size() && s[i] == '=') ++i;
    if (i >= s.size() || s[i] != '[') return std::string::npos;
    const std::string close = "]" + std::string(i - start - 1, '=') + "]";
    const std::size_t end = s.find(close, i + 1);
    return end == std::string::npos ? s.size() : end + close.size();
}

static std::vector<Token> lex(const std::string& source) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < source.size()) {
        const std::size_t begin = i;
        const char c = source[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            while (i < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[i]))) ++i;
            out.push_back({Token::Kind::Whitespace, source.substr(begin, i - begin), begin});
            continue;
        }
        if (c == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            const std::size_t block = long_bracket_end(source, i + 2);
            if (block != std::string::npos && i + 2 < source.size() &&
                source[i + 2] == '[') {
                i = block;
            } else {
                while (i < source.size() && source[i] != '\n') ++i;
            }
            out.push_back({Token::Kind::Comment, source.substr(begin, i - begin), begin});
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < source.size()) {
                if (source[i] == '\\') {
                    i += std::min<std::size_t>(2, source.size() - i);
                } else if (source[i++] == quote) {
                    break;
                }
            }
            out.push_back({Token::Kind::String, source.substr(begin, i - begin), begin});
            continue;
        }
        if (c == '[') {
            const std::size_t end = long_bracket_end(source, i);
            if (end != std::string::npos) {
                i = end;
                out.push_back({Token::Kind::String, source.substr(begin, i - begin), begin});
                continue;
            }
        }
        if (is_word_start(c)) {
            ++i;
            while (i < source.size() && is_word_char(source[i])) ++i;
            out.push_back({Token::Kind::Word, source.substr(begin, i - begin), begin});
            continue;
        }
        if (is_number_start(source, i)) {
            ++i;
            while (i < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[i])) ||
                    source[i] == '.' || source[i] == '_')) ++i;
            out.push_back({Token::Kind::Number, source.substr(begin, i - begin), begin});
            continue;
        }
        static const char* multi[] = {
            "...", "==", "~=", "<=", ">=", "//", "..", "<<", ">>",
            "+=", "-=", "*=", "/=", "%=", "^=", "::", "->"
        };
        bool matched = false;
        for (const char* op : multi) {
            const std::size_t n = std::char_traits<char>::length(op);
            if (source.compare(i, n, op) == 0) {
                i += n;
                out.push_back({Token::Kind::Symbol, source.substr(begin, n), begin});
                matched = true;
                break;
            }
        }
        if (!matched) {
            ++i;
            out.push_back({Token::Kind::Symbol, source.substr(begin, 1), begin});
        }
    }
    return out;
}

static std::string join(const std::vector<Token>& tokens) {
    std::string result;
    for (const auto& token : tokens) result += token.text;
    return result;
}

static bool significant(const Token& token) {
    return token.kind != Token::Kind::Whitespace && token.kind != Token::Kind::Comment;
}

static bool needs_separator(const Token& a, const Token& b) {
    const bool left = a.kind == Token::Kind::Word || a.kind == Token::Kind::Number;
    const bool right = b.kind == Token::Kind::Word || b.kind == Token::Kind::Number;
    return left && right;
}

static std::string compress(const std::string& source) {
    const auto tokens = lex(source);
    std::string result;
    const Token* previous = nullptr;
    for (const auto& token : tokens) {
        if (!significant(token)) continue;
        if (previous && needs_separator(*previous, token)) result.push_back(' ');
        result += token.text;
        previous = &token;
    }
    return result;
}

static std::string decode_short_string(const std::string& token, bool& safe) {
    safe = false;
    if (token.size() < 2 || (token.front() != '"' && token.front() != '\'') ||
        token.back() != token.front()) return {};
    std::string value;
    for (std::size_t i = 1; i + 1 < token.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(token[i]);
        if (c != '\\') {
            value.push_back(static_cast<char>(c));
            continue;
        }
        if (++i + 1 > token.size()) return {};
        const char e = token[i];
        switch (e) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'v': value.push_back('\v'); break;
            case '\\': value.push_back('\\'); break;
            case '"': value.push_back('"'); break;
            case '\'': value.push_back('\''); break;
            case '0': value.push_back('\0'); break;
            default:
                if (e >= '0' && e <= '9') {
                    int number = e - '0';
                    int digits = 1;
                    while (digits < 3 && i + 1 < token.size() - 1 &&
                           token[i + 1] >= '0' && token[i + 1] <= '9') {
                        number = number * 10 + (token[++i] - '0');
                        ++digits;
                    }
                    value.push_back(static_cast<char>(number & 255));
                } else {
                    value.push_back(e);
                }
        }
    }
    safe = true;
    return value;
}

static std::string byte_expression(unsigned char value, bool arithmetic) {
    if (!arithmetic) return std::to_string(static_cast<unsigned>(value));
    const unsigned salt = 17u + (value % 31u);
    return "((" + std::to_string(static_cast<unsigned>(value) + salt) + "-" +
           std::to_string(salt) + "))";
}

static std::string encode_strings(const std::string& source, bool arithmetic) {
    auto tokens = lex(source);
    for (auto& token : tokens) {
        if (token.kind != Token::Kind::String || token.text.empty() ||
            token.text.front() == '[') continue;
        bool safe = false;
        const std::string decoded = decode_short_string(token.text, safe);
        if (!safe) continue;
        std::ostringstream replacement;
        replacement << "string.char(";
        for (std::size_t i = 0; i < decoded.size(); ++i) {
            if (i) replacement << ",";
            replacement << byte_expression(static_cast<unsigned char>(decoded[i]), arithmetic);
        }
        replacement << ")";
        token.text = replacement.str();
    }
    return join(tokens);
}

static std::string random_identifier(std::mt19937_64& rng, std::size_t index) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<int> length(8, 12);
    std::uniform_int_distribution<int> pick(0, 25);
    const std::size_t count = std::max<std::size_t>(8, static_cast<std::size_t>(length(rng)));
    std::string result = "_lt";
    result.push_back(alphabet[index % 26]);
    while (result.size() < count) result.push_back(alphabet[pick(rng)]);
    return result;
}

static const std::unordered_set<std::string> LUA_KEYWORDS = {
    "and","break","do","else","elseif","end","false","for","function","goto",
    "if","in","local","nil","not","or","repeat","return","then","true","until","while",
    "continue","export","type"
};

static std::string rename_variables(const std::string& source, std::mt19937_64& rng) {
    auto tokens = lex(source);
    std::unordered_map<std::string, std::string> names;
    std::size_t sequence = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].kind != Token::Kind::Word || tokens[i].text != "local") continue;
        std::size_t j = i + 1;
        while (j < tokens.size() &&
               (tokens[j].kind == Token::Kind::Whitespace ||
                tokens[j].kind == Token::Kind::Comment)) ++j;
        if (j < tokens.size() && tokens[j].kind == Token::Kind::Word &&
            tokens[j].text == "function") ++j;
        while (j < tokens.size()) {
            while (j < tokens.size() &&
                   (tokens[j].kind == Token::Kind::Whitespace ||
                    tokens[j].kind == Token::Kind::Comment)) ++j;
            if (j >= tokens.size() || tokens[j].kind != Token::Kind::Word) break;
            if (LUA_KEYWORDS.count(tokens[j].text)) break;
            if (!names.count(tokens[j].text)) {
                names.emplace(tokens[j].text, random_identifier(rng, sequence++));
            }
            ++j;
            while (j < tokens.size() &&
                   (tokens[j].kind == Token::Kind::Whitespace ||
                    tokens[j].kind == Token::Kind::Comment)) ++j;
            if (j >= tokens.size() || tokens[j].text != ",") break;
            ++j;
        }
    }
    for (auto& token : tokens) {
        if (token.kind != Token::Kind::Word) continue;
        if (LUA_KEYWORDS.count(token.text)) continue;
        if (!names.count(token.text)) continue;
        // A field name in obj.name or obj:name is not a local variable.
        const std::size_t p = token.offset;
        if (p > 0 && (source[p - 1] == '.' || source[p - 1] == ':')) continue;
        token.text = names[token.text];
    }
    return join(tokens);
}

static std::string safe_name(std::size_t n) {
    return "__lt_" + std::to_string(n);
}

static std::string garbage_code(std::string source, std::size_t blocks, std::uint64_t seed) {
    std::mt19937_64 rng(seed ^ 0x9e3779b97f4a7c15ULL);
    std::ostringstream prefix;
    const std::size_t count = std::min<std::size_t>(blocks, 64);
    for (std::size_t i = 0; i < count; ++i) {
        const auto name = safe_name(i);
        const auto number = static_cast<unsigned>(rng() % 997 + 1);
        prefix << "do local " << name << "=" << number
               << ";if " << name << "<0 then error(\"dead\") end end\n";
    }
    return prefix.str() + source;
}

static std::string opaque_predicates(std::string source, std::uint64_t seed) {
    const unsigned salt = static_cast<unsigned>(seed % 43 + 7);
    std::ostringstream out;
    out << "do local __lt_opaque=(" << salt << "*" << salt << "-" << salt * salt
        << ");if __lt_opaque~=0 then error(\"opaque predicate failure\") end end\n";
    out << "if ((" << salt << "+" << salt << ")==" << salt * 2 << ") then\n";
    out << source << "\nend\n";
    return out.str();
}

static std::string control_flow(std::string source, std::uint64_t seed) {
    const unsigned state = static_cast<unsigned>(seed % 97 + 3);
    std::ostringstream out;
    out << "do local __lt_state=" << state << ";if __lt_state==" << state << " then\n";
    out << source << "\nend end\n";
    return out.str();
}

static std::string anti_tamper(std::string source) {
    return
        "do "
        "if type(string)~=\"table\" or type(string.char)~=\"function\" then "
        "error(\"Luatrix integrity check failed\") end "
        "if type(table)~=\"table\" or type(table.concat)~=\"function\" then "
        "error(\"Luatrix integrity check failed\") end "
        "end\n" + source;
}

static std::string inline_constant_functions(const std::string& source) {
    // Safe subset of the upstream inliner: local functions with no parameters and
    // a single literal return are replaced at call sites. Everything else is
    // left untouched rather than risking a semantic change.
    std::string result = source;
    const std::regex pattern(
        R"(local\s+function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)\s*return\s+((?:"(?:\\.|[^"])*"|'(?:\\.|[^'])*'|-?[0-9]+|true|false|nil))\s*end)");
    std::smatch match;
    std::string::const_iterator search = result.cbegin();
    std::vector<std::pair<std::string, std::string>> replacements;
    while (std::regex_search(search, result.cend(), match, pattern)) {
        replacements.emplace_back(match[1].str(), match[2].str());
        search = match.suffix().first;
    }
    for (const auto& item : replacements) {
        const std::regex call("\\b" + item.first + R"(\s*\(\s*\))");
        result = std::regex_replace(result, call, item.second);
    }
    return result;
}

static std::string escaped_payload(const std::string& source) {
    // Lua chunks have a small register limit. A single string.char(source...)
    // call works for tiny inputs but fails on real scripts, so construct the
    // payload in bounded slices inside an expression-local closure.
    std::ostringstream out;
    out << "(function()local __lt_s=\"\";";
    const std::size_t chunk_size = 48;
    for (std::size_t start = 0; start < source.size(); start += chunk_size) {
        const std::size_t end = std::min(source.size(), start + chunk_size);
        out << "__lt_s=__lt_s..string.char(";
        for (std::size_t i = start; i < end; ++i) {
            if (i != start) out << ",";
            out << static_cast<unsigned>(static_cast<unsigned char>(source[i]));
        }
        out << ");";
    }
    out << "return __lt_s end)()";
    return out.str();
}

static std::string dynamic_code(const std::string& source, const std::string& loader) {
    return "local __lt_dynamic_load=" + loader + ";local __lt_dynamic_source=" +
           escaped_payload(source) +
           ";if __lt_dynamic_load then __lt_dynamic_load(__lt_dynamic_source)() "
           "else error(\"dynamic loading is unavailable\") end";
}

static std::string bytecode_envelope(const std::string& source) {
    return "local __lt_bytecode=load;" +
           std::string("if not __lt_bytecode then error(\"bytecode loader unavailable\") end;") +
           "__lt_bytecode(" + escaped_payload(source) + ")()";
}

static std::string random_opcode(std::mt19937_64& rng,
                                 std::unordered_set<std::string>& used) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::uniform_int_distribution<int> pick(0, 25);
    std::string opcode;
    do {
        opcode.clear();
        for (int i = 0; i < 3; ++i) opcode.push_back(alphabet[pick(rng)]);
    } while (!used.insert(opcode).second);
    return opcode;
}

static std::string virtual_machine_envelope(const std::string& source,
                                            std::mt19937_64& rng) {
    // The VM instruction vocabulary is intentionally re-keyed per output.
    // A static "RET" opcode would make generated files easy to fingerprint.
    std::unordered_set<std::string> used;
    const std::string load_opcode = random_opcode(rng, used);
    const std::string ret_opcode = random_opcode(rng, used);
    const std::string noise_opcode = random_opcode(rng, used);
    std::ostringstream out;
    out << "local __lt_vm={};local __lt_opcode={"
        << "[\"" << load_opcode << "\"]=\"LOAD\","
        << "[\"" << ret_opcode << "\"]=\"RET\","
        << "[\"" << noise_opcode << "\"]=\"NOP\"};"
        << "local __lt_program={\"" << load_opcode << "\",\""
        << noise_opcode << "\",\"" << ret_opcode << "\"};"
        << "local __lt_chunk;local __lt_pc=1;"
        << "while __lt_pc<=#" << "__lt_program do "
        << "local __lt_op=__lt_opcode[__lt_program[__lt_pc]];"
        << "if __lt_op==\"LOAD\" then __lt_chunk=load("
        << escaped_payload(source) << ");"
        << "elseif __lt_op==\"RET\" then return __lt_chunk() end;"
        << "__lt_pc=__lt_pc+1 end";
    return out.str();
}

static std::string wrap_function(const std::string& source) {
    return "(function(...) " + source + " end)()";
}

static std::string detect_target(const std::string& source, const std::string& path) {
    int luau = 0;
    int glua = 0;
    if (std::regex_search(source, std::regex(R"(^#!.*\bluau\b|^--!)"))) luau += 3;
    if (std::regex_search(source, std::regex(R"(\bexport\s+type\b|\blocal\s+\w+\s*:)"))) luau += 2;
    if (std::regex_search(source, std::regex(R"(\bgame\s*:\s*(GetService|HttpGet)\s*\()"))) luau += 4;
    if (std::regex_search(source, std::regex(R"(\b(AddCSLuaFile|include|hook\.Add|SERVER|CLIENT)\b)"))) glua += 3;
    if (glua > luau && glua >= 2) return "glua";
    if (luau > glua && luau >= 2) return "luau";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".luau") return "luau";
    return "lua";
}

static std::string process(std::string source, const Options& options, const std::string& path) {
    const std::string target = options.target == "auto" ? detect_target(source, path) : options.target;
    const std::uint64_t seed = static_cast<std::uint64_t>(std::random_device{}()) ^
        static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now()
            .time_since_epoch().count());
    std::mt19937_64 rng(seed);

    // The full compatible pipeline is always active; VM and bytecode encoding
    // remain skipped for Luau/GLua just as they are in the upstream manifest.
    if (target == "luau" || target == "glua")
        source = dynamic_code(source, "loadstring");
    else
        source = dynamic_code(source, "load");
    source = opaque_predicates(source, seed);
    source = encode_strings(source, false);
    source = encode_strings(source, true);
    source = inline_constant_functions(source);
    source = rename_variables(source, rng);
    if (target == "lua") source = virtual_machine_envelope(source, rng);
    source = anti_tamper(source);
    source = control_flow(source, seed);
    source = garbage_code(source, 20, seed);
    source = compress(source);
    source = wrap_function(source);
    if (target == "lua") source = bytecode_envelope(source);
    source = "--[Luatrix | LTRIX]\n" + source;
    return source;
}

static Options parse_args(int argc, char** argv) {
    Options options;
    if (argc != 3) {
        throw std::runtime_error("usage: luatrix <input> <out>");
    }
    options.input = argv[1];
    options.output = argv[2];
    if (options.input == options.output)
        throw std::runtime_error("input and output must be different");
    return options;
}

static std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open input: " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

static void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output: " + path.string());
    output << contents;
}

static void process_one(const fs::path& input, const fs::path& output,
                        const Options& options) {
    const std::string original = read_file(input);
    write_file(output, process(original, options, input.string()));
    std::cout << input << " -> " << output << "\n";
}

} // namespace luatrix

int main(int argc, char** argv) {
    try {
        luatrix::Options options = luatrix::parse_args(argc, argv);
        luatrix::process_one(options.input, options.output, options);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "luatrix: " << error.what() << "\n";
        return 1;
    }
}