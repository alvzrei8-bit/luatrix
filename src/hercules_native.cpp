/*
 * Hercules Native Amalgamation
 * ----------------------------
 * A single-file C++17 port of the public src/ pipeline from:
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
#include <iomanip>
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

namespace hercules {

struct Feature {
    const char* key;
    const char* name;
    const char* short_flag;
    const char* long_flag;
    const char* description;
    int pipeline_order;
    bool lua_only;
};

static const std::vector<Feature> FEATURES = {
    {"dynamic_code",       "Dynamic Code",        "-dc",  "--dynamic_code",       "Runtime reconstruction and load indirection", 10, false},
    {"opaque_predicates",  "Opaque Predicates",   "-opi", "--opaque_predicates",   "Semantically stable opaque branches",          20, false},
    {"string_encoding",    "String Encoding",     "-se",  "--string_encoding",    "Runtime string reconstruction",                30, false},
    {"string_expressions", "String To Expressions","-st", "--string_to_expressions","Arithmetic expressions for string bytes",       40, false},
    {"function_inlining",  "Function Inlining",   "-fi",  "--function_inlining",   "Inlining of safe constant-return helpers",     50, false},
    {"variable_renaming",  "Variable Renaming",   "-vr",  "--variable_renaming",   "Deterministic local-symbol renaming",          60, false},
    {"virtual_machine",    "Virtual Machine",     "-vm",  "--virtual_machine",     "Single-dispatch runtime execution envelope",  70, true},
    {"antitamper",         "Anti Tamper",         "-at",  "--antitamper",           "Runtime integrity checks for core functions",  80, false},
    {"control_flow",       "Control Flow",        "-cf",  "--control_flow",        "Opaque state guard around the program",       90, false},
    {"garbage_code",       "Garbage Code",        "-gci", "--garbage_code",         "Dead decoy locals and branches",              100, false},
    {"compressor",         "Compressor",          "-c",   "--compressor",          "Comment removal and safe whitespace packing",  110, false},
    {"wrap_in_function",   "Function Wrapping",   "-wif", "--wrap_in_function",    "Encapsulation in an immediately-called function",120, false},
    {"bytecode_encoding",  "Bytecode Encoding",   "-be",  "--bytecode_encoding",   "Encoded source payload loaded at runtime",    130, true},
    {"watermark",          "Watermark",           "",     "--watermark",           "Attribution header",                           140, false},
};

struct Options {
    std::string input;
    std::string output;
    std::string target = "auto";
    std::string watermark =
        "--[Obfuscated by Hercules Native | based on Hercules by zeusssz]\n";
    std::string preset;
    std::string custom_features;
    std::uint64_t seed = 0;
    bool seed_set = false;
    bool overwrite = false;
    bool folder = false;
    bool list_features = false;
    bool manifest_json = false;
    bool no_watermark = false;
    // Hercules' manifest enables its modules by default. Explicit selection
    // flags switch from that default to the requested subset.
    bool all = true;
    bool help = false;
    std::unordered_set<std::string> enabled;
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
    std::string result = "_h";
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
    return "__h_" + std::to_string(n);
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
    out << "do local __h_opaque=(" << salt << "*" << salt << "-" << salt * salt
        << ");if __h_opaque~=0 then error(\"opaque predicate failure\") end end\n";
    out << "if ((" << salt << "+" << salt << ")==" << salt * 2 << ") then\n";
    out << source << "\nend\n";
    return out.str();
}

static std::string control_flow(std::string source, std::uint64_t seed) {
    const unsigned state = static_cast<unsigned>(seed % 97 + 3);
    std::ostringstream out;
    out << "do local __h_state=" << state << ";if __h_state==" << state << " then\n";
    out << source << "\nend end\n";
    return out.str();
}

static std::string anti_tamper(std::string source) {
    return
        "do "
        "if type(string)~=\"table\" or type(string.char)~=\"function\" then "
        "error(\"Hercules integrity check failed\") end "
        "if type(table)~=\"table\" or type(table.concat)~=\"function\" then "
        "error(\"Hercules integrity check failed\") end "
        "end\n" + source;
}

static std::string inline_constant_functions(const std::string& source) {
    // Safe subset of Hercules' inliner: local functions with no parameters and
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
    out << "(function()local __h_s=\"\";";
    const std::size_t chunk_size = 48;
    for (std::size_t start = 0; start < source.size(); start += chunk_size) {
        const std::size_t end = std::min(source.size(), start + chunk_size);
        out << "__h_s=__h_s..string.char(";
        for (std::size_t i = start; i < end; ++i) {
            if (i != start) out << ",";
            out << static_cast<unsigned>(static_cast<unsigned char>(source[i]));
        }
        out << ");";
    }
    out << "return __h_s end)()";
    return out.str();
}

static std::string dynamic_code(const std::string& source, const std::string& loader) {
    return "local __h_dynamic_load=" + loader + ";local __h_dynamic_source=" +
           escaped_payload(source) +
           ";if __h_dynamic_load then __h_dynamic_load(__h_dynamic_source)() "
           "else error(\"dynamic loading is unavailable\") end";
}

static std::string bytecode_envelope(const std::string& source) {
    return "local __h_bytecode=load;" +
           std::string("if not __h_bytecode then error(\"bytecode loader unavailable\") end;") +
           "__h_bytecode(" + escaped_payload(source) + ")()";
}

static std::string virtual_machine_envelope(const std::string& source) {
    return "local __h_vm={};__h_vm[1]=function() return load(" +
           escaped_payload(source) +
           ")() end;return __h_vm[1]()";
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

static bool enabled(const Options& options, const std::string& key) {
    // Watermark is the one manifest output feature that remains enabled when
    // a user selects a subset of obfuscation passes, matching Hercules' CLI.
    if (key == "watermark") return !options.no_watermark;
    return options.all || options.enabled.count(key) != 0;
}

static std::string process(std::string source, const Options& options, const std::string& path) {
    const std::string target = options.target == "auto" ? detect_target(source, path) : options.target;
    const std::uint64_t seed = options.seed_set
        ? options.seed
        : static_cast<std::uint64_t>(std::random_device{}()) ^
          static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now()
              .time_since_epoch().count());
    std::mt19937_64 rng(seed);

    // Pipeline order mirrors manifest.lua. Target-incompatible passes are
    // skipped just as Hercules skips VM and bytecode encoding for Luau/GLua.
    if (enabled(options, "dynamic_code"))
        source = dynamic_code(source, (target == "luau" || target == "glua")
                                       ? "loadstring" : "load");
    if (enabled(options, "opaque_predicates")) source = opaque_predicates(source, seed);
    if (enabled(options, "string_encoding")) source = encode_strings(source, false);
    if (enabled(options, "string_expressions")) source = encode_strings(source, true);
    if (enabled(options, "function_inlining")) source = inline_constant_functions(source);
    if (enabled(options, "variable_renaming")) source = rename_variables(source, rng);
    if (enabled(options, "virtual_machine") && target == "lua")
        source = virtual_machine_envelope(source);
    if (enabled(options, "antitamper")) source = anti_tamper(source);
    if (enabled(options, "control_flow")) source = control_flow(source, seed);
    if (enabled(options, "garbage_code")) source = garbage_code(source, 20, seed);
    if (enabled(options, "compressor")) source = compress(source);
    if (enabled(options, "wrap_in_function")) source = wrap_function(source);
    if (enabled(options, "bytecode_encoding") && target == "lua")
        source = bytecode_envelope(source);
    if (enabled(options, "watermark") && !options.no_watermark)
        source = options.watermark + source;
    return source;
}

static std::string json_escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

static void print_features() {
    std::cout << "Hercules Native features (" << FEATURES.size() << "):\n";
    for (const auto& feature : FEATURES) {
        std::cout << "  " << std::left << std::setw(22) << feature.name
                  << feature.short_flag << "  " << feature.description;
        if (feature.lua_only) std::cout << " [Lua only]";
        std::cout << "\n";
    }
}

static void print_manifest() {
    std::cout << "{\"version\":2,\"features\":[";
    for (std::size_t i = 0; i < FEATURES.size(); ++i) {
        if (i) std::cout << ",";
        const auto& f = FEATURES[i];
        std::cout << "{\"key\":\"" << f.key << "\",\"name\":\""
                  << json_escape(f.name) << "\",\"short\":\""
                  << f.short_flag << "\",\"long\":\"" << f.long_flag
                  << "\",\"pipeline_order\":" << f.pipeline_order
                  << ",\"lua_only\":" << (f.lua_only ? "true" : "false")
                  << "}";
    }
    std::cout << "]}\n";
}

static void print_help(const char* program) {
    std::cout << "Usage: " << program << " INPUT.lua [options]\n\n"
              << "Selection:\n"
              << "  --all                 enable every compatible feature\n"
              << "  --preset NAME         light, balanced, heavy, maximum\n"
              << "  --features LIST       comma-separated feature keys\n"
              << "  --no-watermark        disable the attribution header\n"
              << "  --watermark TEXT      replace the attribution header\n\n"
              << "Runtime:\n"
              << "  --target lua|luau|glua  force target (default: auto)\n"
              << "  --seed NUMBER         deterministic output\n"
              << "  -o, --output PATH     output path\n"
              << "  --overwrite           write over the input\n"
              << "  --folder              process every .lua/.luau file in a folder\n\n"
              << "Introspection:\n"
              << "  --list-features       print all feature names and exit\n"
              << "  --manifest-json       print machine-readable feature metadata\n";
}

static void choose_preset(Options& options, const std::string& preset) {
    options.preset = preset;
    static const std::unordered_map<std::string, std::string> presets = {
        {"light", "variable_renaming,compressor"},
        {"balanced", "variable_renaming,control_flow,string_encoding,opaque_predicates,compressor"},
        {"heavy", "variable_renaming,control_flow,string_encoding,string_expressions,opaque_predicates,garbage_code,function_inlining,wrap_in_function,antitamper,dynamic_code,compressor"},
        {"maximum", "dynamic_code,opaque_predicates,string_encoding,string_expressions,function_inlining,variable_renaming,virtual_machine,antitamper,control_flow,garbage_code,compressor,wrap_in_function,bytecode_encoding,watermark"},
    };
    const auto it = presets.find(preset);
    if (it == presets.end()) throw std::runtime_error("unknown preset: " + preset);
    options.custom_features = it->second;
}

static void enable_list(Options& options, const std::string& csv) {
    std::size_t start = 0;
    while (start < csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::string key = csv.substr(start, comma == std::string::npos ? comma : comma - start);
        if (!key.empty()) options.enabled.insert(key);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

static Options parse_args(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") options.help = true;
        else if (arg == "--list-features") options.list_features = true;
        else if (arg == "--manifest-json") options.manifest_json = true;
        else if (arg == "--all") options.all = true;
        else if (arg == "--folder") options.folder = true;
        else if (arg == "--overwrite") options.overwrite = true;
        else if (arg == "--no-watermark") options.no_watermark = true;
        else if (arg == "--preset" && i + 1 < argc) {
            options.all = false;
            choose_preset(options, argv[++i]);
        }
        else if (arg == "--features" && i + 1 < argc) {
            options.all = false;
            enable_list(options, argv[++i]);
        }
        else if (arg == "--target" && i + 1 < argc) options.target = argv[++i];
        else if (arg == "--watermark" && i + 1 < argc) options.watermark = std::string(argv[++i]) + "\n";
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) options.output = argv[++i];
        else if (arg == "--seed" && i + 1 < argc) {
            options.seed = std::stoull(argv[++i]);
            options.seed_set = true;
        } else if (!arg.empty() && arg[0] != '-') {
            if (options.input.empty()) options.input = arg;
            else throw std::runtime_error("only one input path is supported");
        } else if (arg == "-c") {
            options.all = false;
            options.enabled.insert("compressor");
        }
        else {
            bool found = false;
            for (const auto& feature : FEATURES) {
                if (arg == feature.short_flag || arg == feature.long_flag) {
                    options.all = false;
                    options.enabled.insert(feature.key);
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("unknown option: " + arg);
        }
    }
    if (!options.custom_features.empty()) enable_list(options, options.custom_features);
    if (options.input.empty() && !options.list_features && !options.manifest_json && !options.help)
        throw std::runtime_error("no input file specified");
    if (options.target != "auto" && options.target != "lua" &&
        options.target != "luau" && options.target != "glua")
        throw std::runtime_error("target must be lua, luau, glua, or auto");
    for (const auto& feature : options.enabled) {
        bool known = false;
        for (const auto& spec : FEATURES) if (feature == spec.key) known = true;
        if (!known) throw std::runtime_error("unknown feature key: " + feature);
    }
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

static fs::path output_path(const fs::path& input, const Options& options) {
    if (options.overwrite) return input;
    if (!options.output.empty() && !options.folder) return fs::path(options.output);
    const std::string ext = input.extension().string();
    return input.parent_path() /
           (input.stem().string() + "_obfuscated" + (ext == ".luau" ? ".luau" : ".lua"));
}

static void process_one(const fs::path& input, const Options& options) {
    const std::string original = read_file(input);
    const fs::path output = output_path(input, options);
    write_file(output, process(original, options, input.string()));
    std::cout << input << " -> " << output << "\n";
}

} // namespace hercules

int main(int argc, char** argv) {
    try {
        hercules::Options options = hercules::parse_args(argc, argv);
        if (options.help) {
            hercules::print_help(argv[0]);
            return 0;
        }
        if (options.list_features) {
            hercules::print_features();
            return 0;
        }
        if (options.manifest_json) {
            hercules::print_manifest();
            return 0;
        }
        const fs::path input(options.input);
        if (options.folder) {
            if (!fs::is_directory(input)) throw std::runtime_error("not a directory: " + input.string());
            for (const auto& item : fs::recursive_directory_iterator(input)) {
                if (!item.is_regular_file()) continue;
                const auto ext = item.path().extension().string();
                if (ext == ".lua" || ext == ".luau") hercules::process_one(item.path(), options);
            }
        } else {
            hercules::process_one(input, options);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hercules: " << error.what() << "\n";
        return 1;
    }
}