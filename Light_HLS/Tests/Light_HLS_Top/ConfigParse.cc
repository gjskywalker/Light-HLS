#include "ConfigParse.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

extern double DSP_limit;
extern double FF_limit;
extern double LUT_limit;
extern double BRAM_limit;
extern int ClockNum_limit;
extern bool all_sub_function_inline;
extern bool dataflowApplied;

namespace
{

enum class ScopeKind
{
    Function,
    Loop
};

struct ScopeMarker
{
    std::size_t position;
    bool begin;
    ScopeKind kind;
    std::string name;
};

enum class SourceDirectiveKind
{
    Pipeline,
    Unroll,
    ArrayPartition,
    BindStorage,
    Dataflow,
    Stream
};

struct ParsedPragmaOptions
{
    std::map<std::string, std::string> assignments;
    std::set<std::string> flags;
};

struct SourceDirective
{
    SourceDirectiveKind kind;
    std::string label;
    std::string variable;
    std::string scope;
    std::string channel_type;
    std::string storage_type;
    std::string storage_implementation;
    int dim = -1;
    int depth = -1;
    int bitwidth = 0;
    int instances = 1;
    int initial_tokens = 0;
    bool disabled = false;
    std::string config_line;
};

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isIdentifierBegin(char value)
{
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalpha(character) || value == '_';
}

bool isIdentifierCharacter(char value)
{
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) || value == '_';
}

bool parsePragmaOptions(const std::string &raw_arguments,
                        ParsedPragmaOptions &options,
                        std::string &error_message)
{
    std::string arguments = raw_arguments;
    const std::size_t line_comment = arguments.find("//");
    if (line_comment != std::string::npos)
        arguments.erase(line_comment);
    arguments = std::regex_replace(arguments, std::regex(R"(/\*.*?\*/)"), " ");

    std::size_t position = 0;
    while (position < arguments.size())
    {
        while (position < arguments.size() &&
               (std::isspace(static_cast<unsigned char>(arguments[position])) ||
                arguments[position] == ',' || arguments[position] == ';'))
        {
            ++position;
        }
        if (position == arguments.size())
            break;
        if (!isIdentifierBegin(arguments[position]))
        {
            error_message = "unexpected token near '" + arguments.substr(position) + "'";
            return false;
        }

        const std::size_t name_begin = position++;
        while (position < arguments.size() &&
               isIdentifierCharacter(arguments[position]))
        {
            ++position;
        }
        const std::string name =
            lowerCopy(arguments.substr(name_begin, position - name_begin));

        while (position < arguments.size() &&
               std::isspace(static_cast<unsigned char>(arguments[position])))
        {
            ++position;
        }

        if (position < arguments.size() && arguments[position] == '=')
        {
            ++position;
            while (position < arguments.size() &&
                   std::isspace(static_cast<unsigned char>(arguments[position])))
            {
                ++position;
            }
            const std::size_t value_begin = position;
            while (position < arguments.size() &&
                   !std::isspace(static_cast<unsigned char>(arguments[position])) &&
                   arguments[position] != ',' && arguments[position] != ';')
            {
                ++position;
            }
            if (value_begin == position)
            {
                error_message = "option '" + name + "' requires a value";
                return false;
            }
            if (options.assignments.count(name) || options.flags.count(name))
            {
                error_message = "duplicate option '" + name + "'";
                return false;
            }
            options.assignments[name] =
                arguments.substr(value_begin, position - value_begin);
            continue;
        }

        if (options.assignments.count(name) || options.flags.count(name))
        {
            error_message = "duplicate option '" + name + "'";
            return false;
        }
        options.flags.insert(name);
    }
    return true;
}

bool validateOptions(const ParsedPragmaOptions &options,
                     const std::set<std::string> &allowed_assignments,
                     const std::set<std::string> &allowed_flags,
                     std::string &error_message)
{
    for (const auto &assignment : options.assignments)
    {
        if (!allowed_assignments.count(assignment.first))
        {
            error_message = "unknown option '" + assignment.first + "'";
            return false;
        }
    }
    for (const std::string &flag : options.flags)
    {
        if (!allowed_flags.count(flag))
        {
            error_message = "unknown option '" + flag + "'";
            return false;
        }
    }
    return true;
}

bool parseBooleanOption(const ParsedPragmaOptions &options,
                        const std::string &name, bool &value,
                        std::string &error_message)
{
    value = options.flags.count(name);
    auto assignment = options.assignments.find(name);
    if (assignment == options.assignments.end())
        return true;

    const std::string option_value = lowerCopy(assignment->second);
    if (option_value == "true")
    {
        value = true;
        return true;
    }
    if (option_value == "false")
    {
        value = false;
        return true;
    }
    error_message = "option '" + name + "' must be true or false";
    return false;
}

bool sameDirectiveTarget(const SourceDirective &lhs,
                         const SourceDirective &rhs)
{
    if (lhs.kind != rhs.kind)
        return false;
    if (lhs.kind == SourceDirectiveKind::ArrayPartition)
    {
        return lhs.variable == rhs.variable && lhs.scope == rhs.scope &&
               lhs.dim == rhs.dim;
    }
    if (lhs.kind == SourceDirectiveKind::BindStorage)
        return lhs.variable == rhs.variable && lhs.scope == rhs.scope;
    if (lhs.kind == SourceDirectiveKind::Dataflow)
        return lhs.scope == rhs.scope;
    if (lhs.kind == SourceDirectiveKind::Stream)
        return lhs.variable == rhs.variable && lhs.scope == rhs.scope;
    return lhs.label == rhs.label;
}

void replaceSourceDirective(std::vector<SourceDirective> &directives,
                            SourceDirective directive)
{
    directives.erase(
        std::remove_if(directives.begin(), directives.end(),
                       [&](const SourceDirective &existing) {
                           return sameDirectiveTarget(existing, directive);
                       }),
        directives.end());
    directives.push_back(std::move(directive));
}

bool parseConfigDirectiveTarget(const std::string &line,
                                SourceDirective &directive)
{
    std::istringstream input(line);
    std::string directive_name;
    input >> directive_name;
    const std::string normalized_name = lowerCopy(directive_name);
    if (normalized_name != "loop_pipeline" &&
        normalized_name != "loop_unroll" &&
        normalized_name != "array_partition" &&
        normalized_name != "bind_storage" &&
        normalized_name != "func_dataflow" &&
        normalized_name != "stream")
    {
        return false;
    }

    std::string arguments((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
    ParsedPragmaOptions options;
    std::string ignored_error;
    if (!parsePragmaOptions(arguments, options, ignored_error))
        return false;

    if (normalized_name == "loop_pipeline" ||
        normalized_name == "loop_unroll")
    {
        auto label = options.assignments.find("label");
        if (label == options.assignments.end())
            return false;
        directive.kind = normalized_name == "loop_pipeline"
                             ? SourceDirectiveKind::Pipeline
                             : SourceDirectiveKind::Unroll;
        directive.label = label->second;
        return true;
    }

    auto variable = options.assignments.find("variable");
    auto scope = options.assignments.find("scope");
    if (normalized_name == "func_dataflow")
    {
        if (scope == options.assignments.end())
            return false;
        directive.kind = SourceDirectiveKind::Dataflow;
        directive.scope = scope->second;
        return true;
    }
    if (normalized_name == "stream")
    {
        if (variable == options.assignments.end() ||
            scope == options.assignments.end())
        {
            return false;
        }
        directive.kind = SourceDirectiveKind::Stream;
        directive.variable = variable->second;
        directive.scope = scope->second;
        return true;
    }

    if (normalized_name == "bind_storage")
    {
        if (variable == options.assignments.end() ||
            scope == options.assignments.end())
        {
            return false;
        }
        directive.kind = SourceDirectiveKind::BindStorage;
        directive.variable = variable->second;
        directive.scope = scope->second;
        return true;
    }

    auto dim = options.assignments.find("dim");
    if (variable == options.assignments.end() ||
        scope == options.assignments.end() || dim == options.assignments.end())
    {
        return false;
    }
    try
    {
        std::size_t parsed = 0;
        directive.dim = std::stoi(dim->second, &parsed);
        if (parsed != dim->second.size())
            return false;
    }
    catch (const std::exception &)
    {
        return false;
    }
    directive.kind = SourceDirectiveKind::ArrayPartition;
    directive.variable = variable->second;
    directive.scope = scope->second;
    return true;
}

bool parsePositiveInteger(const std::map<std::string, std::string> &options,
                          const std::string &name, int default_value, bool required,
                          int &result, std::string &error_message)
{
    auto option = options.find(name);
    if (option == options.end())
    {
        if (required)
        {
            error_message = "missing required option '" + name + "'";
            return false;
        }
        result = default_value;
        return true;
    }

    try
    {
        std::size_t parsed = 0;
        result = std::stoi(option->second, &parsed);
        if (parsed != option->second.size() || result <= 0)
            throw std::invalid_argument("not a positive integer");
    }
    catch (const std::exception &)
    {
        error_message = "option '" + name + "' must be a positive integer";
        return false;
    }
    return true;
}

int builtinTypeBitwidth(std::string type)
{
    type = std::regex_replace(type, std::regex(R"(\s+)"), " ");
    while (!type.empty() && type.front() == ' ')
        type.erase(type.begin());
    while (!type.empty() && type.back() == ' ')
        type.pop_back();
    const std::string normalized = lowerCopy(type);
    if (normalized == "_float16" || normalized == "__fp16")
        return 16;
    if (normalized == "bool")
        return 1;
    if (normalized == "char" || normalized == "signed char" ||
        normalized == "unsigned char" || normalized == "int8_t" ||
        normalized == "uint8_t")
        return 8;
    if (normalized == "short" || normalized == "short int" ||
        normalized == "unsigned short" ||
        normalized == "unsigned short int" || normalized == "int16_t" ||
        normalized == "uint16_t")
        return 16;
    if (normalized == "int" || normalized == "unsigned" ||
        normalized == "unsigned int" || normalized == "float" ||
        normalized == "int32_t" || normalized == "uint32_t")
        return 32;
    if (normalized == "long" || normalized == "unsigned long" ||
        normalized == "long long" || normalized == "unsigned long long" ||
        normalized == "double" || normalized == "int64_t" ||
        normalized == "uint64_t")
        return 64;
    return 0;
}

std::string discoverHLSLibraryPath(const std::string &source_file)
{
    if (const char *path = std::getenv("HI_HLS_LIB_PATH"))
    {
        if (*path != '\0')
            return path;
    }

    std::filesystem::path current =
        std::filesystem::absolute(source_file).parent_path();
    while (!current.empty())
    {
        std::filesystem::path candidate = current / "HLS_Data_Lib";
        if (std::filesystem::is_directory(candidate))
            return candidate.string() + "/";
        if (current == current.parent_path())
            break;
        current = current.parent_path();
    }
    return "";
}

std::string configValue(const std::string &config, const std::string &name)
{
    std::istringstream input(config);
    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream tokens(line);
        std::string parameter;
        std::string equal;
        std::string value;
        tokens >> parameter >> equal >> value;
        if (parameter == name && equal == "=")
            return value;
    }
    return "";
}

bool isInstructionLibrary(const std::filesystem::path &path)
{
    return std::filesystem::is_regular_file(path / "add" / "add") &&
           std::filesystem::is_regular_file(path / "mul" / "mul") &&
           std::filesystem::is_regular_file(path / "fadd" / "fadd");
}

std::string availableBoards(const std::filesystem::path &boards_path)
{
    std::vector<std::string> boards;
    std::error_code error;
    for (std::filesystem::directory_iterator entry(boards_path, error), end;
         !error && entry != end; entry.increment(error))
    {
        if (entry->is_directory() || entry->is_symlink())
            boards.push_back(entry->path().filename().string());
    }
    std::sort(boards.begin(), boards.end());
    boards.erase(std::unique(boards.begin(), boards.end()), boards.end());
    std::ostringstream output;
    for (std::size_t index = 0; index < boards.size(); ++index)
    {
        if (index)
            output << ", ";
        output << boards[index];
    }
    return output.str();
}

bool resolveBoardLibraryPath(const std::string &configured_path,
                             const std::string &requested_board,
                             std::string &selected_board,
                             std::string &selected_library_path,
                             std::string &error_message)
{
    std::filesystem::path input =
        std::filesystem::absolute(std::filesystem::path(configured_path));
    std::filesystem::path boards_path;

    if (std::filesystem::is_directory(input / "boards"))
    {
        boards_path = input / "boards";
    }
    else if (input.parent_path().filename() == "boards")
    {
        boards_path = input.parent_path();
        if (requested_board.empty() && isInstructionLibrary(input))
        {
            selected_board = input.filename().string();
            selected_library_path = input.string() + "/";
            return true;
        }
    }
    else if (isInstructionLibrary(input))
    {
        if (!requested_board.empty() && requested_board != "default")
        {
            error_message =
                "HLS_lib_path points to a standalone instruction library, so "
                "board '" + requested_board + "' cannot be selected from it";
            return false;
        }
        selected_board = "legacy";
        selected_library_path = input.string() + "/";
        return true;
    }
    else
    {
        error_message = "HLS_lib_path '" + input.string() +
                        "' is neither an instruction library nor a board-library root";
        return false;
    }

    const std::string board =
        requested_board.empty() ? "default" : requested_board;
    const std::filesystem::path board_path = boards_path / board;
    if (!isInstructionLibrary(board_path))
    {
        error_message = "unknown or incomplete FPGA board library '" + board +
                        "' under '" + boards_path.string() + "'";
        const std::string available = availableBoards(boards_path);
        if (!available.empty())
            error_message += "; available boards: " + available;
        return false;
    }

    std::error_code canonical_error;
    const std::filesystem::path canonical_path =
        std::filesystem::weakly_canonical(board_path, canonical_error);
    const std::filesystem::path resolved_path =
        canonical_error ? board_path : canonical_path;
    selected_board = resolved_path.filename().string();
    selected_library_path = resolved_path.string() + "/";
    return true;
}

bool translateSourcePragmas(const std::string &source_file,
                            const std::string &top_function,
                            std::vector<SourceDirective> &directives,
                            unsigned &source_pragma_count,
                            std::string &error_message)
{
    (void)top_function;
    std::ifstream source(source_file);
    if (!source)
    {
        error_message = "cannot open rewritten source file '" + source_file + "'";
        return false;
    }

    static const std::regex pragma_re(
        R"(^\s*#\s*pragma\s+HLS\s+([A-Za-z_][A-Za-z0-9_]*)(.*)$)",
        std::regex::icase);
    static const std::regex loop_body_begin_re(
        R"(/\*\s*HI_LOOP_BODY_BEGIN\s+(Loop_[A-Za-z_][A-Za-z0-9_]*_[0-9]+)\s*\*/)");
    static const std::regex loop_body_end_re(
        R"(/\*\s*HI_LOOP_BODY_END\s+(Loop_[A-Za-z_][A-Za-z0-9_]*_[0-9]+)\s*\*/)");
    static const std::regex function_body_begin_re(
        R"(/\*\s*HI_FUNCTION_BODY_BEGIN\s+([A-Za-z_][A-Za-z0-9_]*)\s*\*/)");
    static const std::regex function_body_end_re(
        R"(/\*\s*HI_FUNCTION_BODY_END\s+([A-Za-z_][A-Za-z0-9_]*)\s*\*/)");
    static const std::regex stream_declaration_re(
        R"(\bhls\s*::\s*stream\s*<\s*([^,>]+)\s*(?:,\s*([0-9]+)\s*)?>\s*([A-Za-z_][A-Za-z0-9_]*)((?:\s*\[\s*[^\]]+\s*\])*))",
        std::regex::icase);
    static const std::regex stream_dimension_re(
        R"(\[\s*([^\]]+)\s*\])");

    std::vector<std::string> loop_scope;
    std::vector<std::string> function_scope;
    std::string line;
    unsigned line_number = 0;
    while (std::getline(source, line))
    {
        ++line_number;
        std::smatch match;
        const bool is_pragma = std::regex_match(line, match, pragma_re);

        std::vector<ScopeMarker> markers;
        for (std::sregex_iterator marker(line.begin(), line.end(),
                                         loop_body_begin_re), end;
             marker != end; ++marker)
        {
            markers.push_back(
                {static_cast<std::size_t>(marker->position()), true, ScopeKind::Loop,
                 (*marker)[1].str()});
        }
        for (std::sregex_iterator marker(line.begin(), line.end(),
                                         loop_body_end_re), end;
             marker != end; ++marker)
        {
            markers.push_back(
                {static_cast<std::size_t>(marker->position()), false, ScopeKind::Loop,
                 (*marker)[1].str()});
        }
        for (std::sregex_iterator marker(line.begin(), line.end(),
                                         function_body_begin_re), end;
             marker != end; ++marker)
        {
            markers.push_back(
                {static_cast<std::size_t>(marker->position()), true,
                 ScopeKind::Function, (*marker)[1].str()});
        }
        for (std::sregex_iterator marker(line.begin(), line.end(),
                                         function_body_end_re), end;
             marker != end; ++marker)
        {
            markers.push_back(
                {static_cast<std::size_t>(marker->position()), false,
                 ScopeKind::Function, (*marker)[1].str()});
        }
        std::sort(markers.begin(), markers.end(),
                  [](const ScopeMarker &lhs, const ScopeMarker &rhs) {
                      return lhs.position < rhs.position;
                  });

        for (const ScopeMarker &marker : markers)
        {
            std::vector<std::string> &scope =
                marker.kind == ScopeKind::Loop ? loop_scope : function_scope;
            if (marker.begin)
            {
                scope.push_back(marker.name);
                continue;
            }

            auto entry = std::find(scope.rbegin(), scope.rend(), marker.name);
            if (entry != scope.rend())
                scope.erase(std::next(entry).base());
        }
        if (!markers.empty())
            continue;

        if (!is_pragma && !function_scope.empty())
        {
            for (std::sregex_iterator stream(line.begin(), line.end(),
                                             stream_declaration_re),
                 end;
                 stream != end; ++stream)
            {
                SourceDirective directive;
                directive.kind = SourceDirectiveKind::Stream;
                directive.variable = (*stream)[3].str();
                directive.scope = function_scope.back();
                directive.channel_type = "fifo";
                directive.depth =
                    (*stream)[2].matched ? std::stoi((*stream)[2].str()) : 2;
                directive.bitwidth = builtinTypeBitwidth((*stream)[1].str());
                for (std::sregex_iterator dimension(
                         (*stream)[4].first, (*stream)[4].second,
                         stream_dimension_re),
                     dimension_end;
                     dimension != dimension_end; ++dimension)
                {
                    int extent = 0;
                    try
                    {
                        std::size_t parsed = 0;
                        std::string value = (*dimension)[1].str();
                        value.erase(
                            std::remove_if(
                                value.begin(), value.end(),
                                [](unsigned char character) {
                                    return std::isspace(character);
                                }),
                            value.end());
                        extent = std::stoi(value, &parsed);
                        if (parsed != value.size() || extent <= 0)
                            throw std::invalid_argument("invalid extent");
                    }
                    catch (const std::exception &)
                    {
                        error_message =
                            source_file + ":" + std::to_string(line_number) +
                            ": hls::stream array dimensions must be positive "
                            "integer literals";
                        return false;
                    }
                    if (directive.instances >
                        std::numeric_limits<int>::max() / extent)
                    {
                        error_message =
                            source_file + ":" + std::to_string(line_number) +
                            ": hls::stream array instance count is too large";
                        return false;
                    }
                    directive.instances *= extent;
                }
                directive.config_line =
                    "stream variable=" + directive.variable +
                    " scope=" + directive.scope +
                    " depth=" + std::to_string(directive.depth) +
                    " type=fifo bitwidth=" +
                    std::to_string(directive.bitwidth) +
                    " instances=" + std::to_string(directive.instances) +
                    " initial_tokens=0";
                replaceSourceDirective(directives, std::move(directive));
            }
        }

        if (is_pragma)
        {
            const std::string pragma_name = lowerCopy(match[1].str());
            if (pragma_name != "pipeline" && pragma_name != "unroll" &&
                pragma_name != "array_partition" &&
                pragma_name != "bind_storage" &&
                pragma_name != "dataflow" && pragma_name != "stream")
            {
                continue;
            }

            ParsedPragmaOptions options;
            std::string parse_error;
            if (!parsePragmaOptions(match[2].str(), options, parse_error))
            {
                error_message = source_file + ":" + std::to_string(line_number) +
                                ": invalid " + match[1].str() +
                                " pragma: " + parse_error;
                return false;
            }

            if (pragma_name == "pipeline")
            {
                if (!validateOptions(options, {"ii", "off", "rewind", "style"},
                                     {"off", "rewind"}, parse_error))
                {
                    error_message = source_file + ":" + std::to_string(line_number) +
                                    ": invalid PIPELINE pragma: " + parse_error;
                    return false;
                }

                if (loop_scope.empty())
                {
                    const std::string detail =
                        function_scope.empty()
                            ? "PIPELINE must be placed inside a loop or function"
                            : "function-level PIPELINE is valid in Vitis HLS but is "
                              "not modeled by LightHLS";
                    error_message = source_file + ":" +
                                    std::to_string(line_number) + ": " + detail;
                    return false;
                }

                bool off = false;
                if (!parseBooleanOption(options, "off", off, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid PIPELINE pragma: " + parse_error;
                    return false;
                }
                if (off && (options.assignments.count("ii") ||
                            options.assignments.count("rewind") ||
                            options.assignments.count("style") ||
                            options.flags.count("rewind")))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid PIPELINE pragma: off cannot be "
                                    "combined with II, rewind, or style";
                    return false;
                }
                if (options.assignments.count("rewind") ||
                    options.flags.count("rewind"))
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": PIPELINE option 'rewind' is valid in Vitis HLS but "
                        "is not modeled by LightHLS";
                    return false;
                }
                auto style = options.assignments.find("style");
                if (style != options.assignments.end() &&
                    lowerCopy(style->second) != "stp")
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": PIPELINE style must be stp; flp and frp are not "
                        "modeled by LightHLS";
                    return false;
                }

                int ii = 0;
                if (!parsePositiveInteger(options.assignments, "ii", 0, false,
                                          ii, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid PIPELINE pragma: " + parse_error;
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::Pipeline;
                directive.label = loop_scope.back();
                directive.disabled = off;
                if (!off)
                {
                    directive.config_line =
                        "loop_pipeline label=" + directive.label +
                        " II=" + std::to_string(ii);
                }
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            else if (pragma_name == "unroll")
            {
                if (!validateOptions(options, {"factor", "off"},
                                     {"off", "skip_exit_check", "region"},
                                     parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid UNROLL pragma: " + parse_error;
                    return false;
                }
                if (loop_scope.empty())
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": UNROLL must be placed inside the target loop";
                    return false;
                }

                bool off = false;
                if (!parseBooleanOption(options, "off", off, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid UNROLL pragma: " + parse_error;
                    return false;
                }
                if (off && (options.assignments.count("factor") ||
                            options.flags.count("skip_exit_check") ||
                            options.flags.count("region")))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid UNROLL pragma: off cannot be "
                                    "combined with factor, skip_exit_check, or region";
                    return false;
                }
                if (options.flags.count("skip_exit_check"))
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": UNROLL option 'skip_exit_check' is valid in Vitis HLS "
                        "but is not modeled by LightHLS";
                    return false;
                }
                if (options.flags.count("region"))
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": UNROLL option 'region' is not supported by LightHLS";
                    return false;
                }

                int factor = 0;
                if (!parsePositiveInteger(options.assignments, "factor", 0,
                                          false, factor, parse_error))
                {
                    error_message = source_file + ":" + std::to_string(line_number) +
                                    ": invalid UNROLL pragma: " + parse_error;
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::Unroll;
                directive.label = loop_scope.back();
                directive.disabled = off;
                if (!off)
                {
                    directive.config_line =
                        "loop_unroll label=" + directive.label +
                        " factor=" + std::to_string(factor);
                }
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            else if (pragma_name == "array_partition")
            {
                if (!validateOptions(options,
                                     {"variable", "type", "factor", "dim", "off"},
                                     {"off"}, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid ARRAY_PARTITION pragma: " +
                                    parse_error;
                    return false;
                }
                if (function_scope.empty())
                {
                    error_message = source_file + ":" + std::to_string(line_number) +
                                    ": ARRAY_PARTITION must be placed inside a function";
                    return false;
                }
                auto variable = options.assignments.find("variable");
                if (variable == options.assignments.end() ||
                    variable->second.empty())
                {
                    error_message = source_file + ":" + std::to_string(line_number) +
                                    ": ARRAY_PARTITION requires variable=<name>";
                    return false;
                }

                int dim = 1;
                auto dim_option = options.assignments.find("dim");
                if (dim_option != options.assignments.end())
                {
                    try
                    {
                        std::size_t parsed = 0;
                        dim = std::stoi(dim_option->second, &parsed);
                        if (parsed != dim_option->second.size() || dim < 0)
                            throw std::invalid_argument("invalid dimension");
                    }
                    catch (const std::exception &)
                    {
                        error_message = source_file + ":" + std::to_string(line_number) +
                                        ": ARRAY_PARTITION dim must be a non-negative integer";
                        return false;
                    }
                }

                bool off = false;
                if (!parseBooleanOption(options, "off", off, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid ARRAY_PARTITION pragma: " +
                                    parse_error;
                    return false;
                }
                if (off && (options.assignments.count("type") ||
                            options.assignments.count("factor")))
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": invalid ARRAY_PARTITION pragma: off cannot be combined "
                        "with type or factor";
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::ArrayPartition;
                directive.variable = variable->second;
                directive.scope = function_scope.back();
                directive.dim = dim;
                directive.disabled = off;
                if (off)
                {
                    replaceSourceDirective(directives, std::move(directive));
                    ++source_pragma_count;
                    continue;
                }

                std::string type = "complete";
                auto type_option = options.assignments.find("type");
                if (type_option != options.assignments.end())
                    type = lowerCopy(type_option->second);
                if (type != "complete" && type != "cyclic" && type != "block")
                {
                    error_message = source_file + ":" + std::to_string(line_number) +
                                    ": ARRAY_PARTITION type must be complete, cyclic, or block";
                    return false;
                }

                directive.config_line =
                    "array_partition variable=" + directive.variable +
                    " scope=" + directive.scope +
                    " dim=" + std::to_string(dim);
                if (type == "complete")
                {
                    if (options.assignments.find("factor") !=
                        options.assignments.end())
                    {
                        error_message = source_file + ":" + std::to_string(line_number) +
                                        ": ARRAY_PARTITION type=complete does not accept factor";
                        return false;
                    }
                    directive.config_line += " complete";
                }
                else
                {
                    int factor = 0;
                    if (!parsePositiveInteger(options.assignments, "factor", 0,
                                              true, factor, parse_error))
                    {
                        error_message = source_file + ":" + std::to_string(line_number) +
                                        ": invalid ARRAY_PARTITION pragma: " + parse_error;
                        return false;
                    }
                    directive.config_line +=
                        " factor=" + std::to_string(factor) + " " + type;
                }
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            else if (pragma_name == "bind_storage")
            {
                if (!validateOptions(options,
                                     {"variable", "type", "impl", "latency"},
                                     {}, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid BIND_STORAGE pragma: " +
                                    parse_error;
                    return false;
                }
                if (function_scope.empty())
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": BIND_STORAGE must be placed inside a function";
                    return false;
                }
                auto variable = options.assignments.find("variable");
                auto type = options.assignments.find("type");
                auto implementation = options.assignments.find("impl");
                if (variable == options.assignments.end() ||
                    variable->second.empty() || type == options.assignments.end() ||
                    implementation == options.assignments.end())
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": BIND_STORAGE requires variable, type, and impl";
                    return false;
                }
                const std::string normalized_type = lowerCopy(type->second);
                const std::string normalized_implementation =
                    lowerCopy(implementation->second);
                if (normalized_type != "ram_1p" &&
                    normalized_type != "ram_2p")
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": BIND_STORAGE type must be ram_1p or ram_2p";
                    return false;
                }
                if (normalized_implementation != "auto" &&
                    normalized_implementation != "bram" &&
                    normalized_implementation != "uram" &&
                    normalized_implementation != "lutram")
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": BIND_STORAGE impl must be auto, bram, uram, or lutram";
                    return false;
                }
                int latency = 1;
                if (!parsePositiveInteger(options.assignments, "latency", 1,
                                          false, latency, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid BIND_STORAGE pragma: " +
                                    parse_error;
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::BindStorage;
                directive.variable = variable->second;
                directive.scope = function_scope.back();
                directive.storage_type = normalized_type;
                directive.storage_implementation = normalized_implementation;
                directive.depth = latency;
                directive.config_line =
                    "bind_storage variable=" + directive.variable +
                    " scope=" + directive.scope + " type=" +
                    directive.storage_type + " impl=" +
                    directive.storage_implementation + " latency=" +
                    std::to_string(latency);
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            else if (pragma_name == "dataflow")
            {
                if (!validateOptions(options, {},
                                     {"disable_start_propagation"},
                                     parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid DATAFLOW pragma: " + parse_error;
                    return false;
                }
                if (function_scope.empty())
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": DATAFLOW must be placed inside a function or loop";
                    return false;
                }
                if (!loop_scope.empty())
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": loop-level DATAFLOW is valid in Vitis HLS but is "
                        "not modeled by LightHLS";
                    return false;
                }
                if (options.flags.count("disable_start_propagation"))
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": DATAFLOW option 'disable_start_propagation' is "
                        "valid in Vitis HLS but is not modeled by LightHLS";
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::Dataflow;
                directive.scope = function_scope.back();
                directive.config_line =
                    "func_dataflow scope=" + directive.scope + " enable";
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            else
            {
                if (!validateOptions(options,
                                     {"variable", "type", "depth",
                                      "initial_tokens", "bitwidth"}, {},
                                     parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid STREAM pragma: " + parse_error;
                    return false;
                }
                if (function_scope.empty())
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": STREAM must be placed inside a function";
                    return false;
                }
                auto variable = options.assignments.find("variable");
                if (variable == options.assignments.end() ||
                    variable->second.empty())
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": STREAM requires variable=<name>";
                    return false;
                }

                int depth = 2;
                if (!parsePositiveInteger(options.assignments, "depth", 2,
                                          false, depth, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid STREAM pragma: " + parse_error;
                    return false;
                }
                int initial_tokens = 0;
                auto initial_tokens_option =
                    options.assignments.find("initial_tokens");
                if (initial_tokens_option != options.assignments.end())
                {
                    try
                    {
                        std::size_t parsed = 0;
                        initial_tokens = std::stoi(
                            initial_tokens_option->second, &parsed);
                        if (parsed != initial_tokens_option->second.size() ||
                            initial_tokens < 0)
                        {
                            throw std::invalid_argument("invalid initial token count");
                        }
                    }
                    catch (const std::exception &)
                    {
                        error_message =
                            source_file + ":" + std::to_string(line_number) +
                            ": STREAM initial_tokens must be a non-negative integer";
                        return false;
                    }
                }
                if (initial_tokens > depth)
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": STREAM initial_tokens cannot exceed depth";
                    return false;
                }
                int bitwidth = 0;
                if (!parsePositiveInteger(options.assignments, "bitwidth", 0,
                                          false, bitwidth, parse_error))
                {
                    error_message = source_file + ":" +
                                    std::to_string(line_number) +
                                    ": invalid STREAM pragma: " + parse_error;
                    return false;
                }
                std::string type = "fifo";
                auto type_option = options.assignments.find("type");
                if (type_option != options.assignments.end())
                    type = lowerCopy(type_option->second);
                if (type != "fifo" && type != "pipo")
                {
                    error_message =
                        source_file + ":" + std::to_string(line_number) +
                        ": STREAM type must be fifo or pipo; shared and unsync "
                        "are not modeled by LightHLS";
                    return false;
                }

                SourceDirective directive;
                directive.kind = SourceDirectiveKind::Stream;
                directive.variable = variable->second;
                directive.scope = function_scope.back();
                directive.channel_type = type;
                directive.depth = depth;
                directive.initial_tokens = initial_tokens;
                directive.bitwidth = bitwidth;
                auto existing = std::find_if(
                    directives.begin(), directives.end(),
                    [&](const SourceDirective &candidate) {
                        return candidate.kind == SourceDirectiveKind::Stream &&
                               candidate.variable == directive.variable &&
                               candidate.scope == directive.scope;
                    });
                if (existing != directives.end())
                {
                    if (directive.bitwidth == 0)
                        directive.bitwidth = existing->bitwidth;
                    directive.instances = existing->instances;
                }
                directive.config_line =
                    "stream variable=" + directive.variable +
                    " scope=" + directive.scope +
                    " depth=" + std::to_string(directive.depth) +
                    " type=" + directive.channel_type +
                    " bitwidth=" + std::to_string(directive.bitwidth) +
                    " instances=" + std::to_string(directive.instances) +
                    " initial_tokens=" +
                    std::to_string(directive.initial_tokens);
                replaceSourceDirective(directives, std::move(directive));
                ++source_pragma_count;
            }
            continue;
        }
    }
    return true;
}

} // namespace

bool BuildEffectiveConfigFromSourcePragmas(const std::string &rewritten_source_file,
                                           const std::string &original_source_file,
                                           const std::string &top_function,
                                           const std::string &base_config_file,
                                           const std::string &requested_board,
                                           const std::string &requested_clock_period,
                                           const std::string &output_config_file,
                                           unsigned &source_pragma_count,
                                           std::string &selected_board,
                                           std::string &selected_library_path,
                                           std::string &error_message)
{
    source_pragma_count = 0;
    std::string base_config;
    if (!base_config_file.empty())
    {
        std::ifstream input(base_config_file);
        if (!input)
        {
            error_message = "cannot open base config file '" + base_config_file + "'";
            return false;
        }
        base_config.assign(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }
    else
    {
        const char *clock_environment = std::getenv("HI_CLOCK_PERIOD");
        const std::string clock =
            clock_environment && *clock_environment ? clock_environment : "10";
        base_config = "clock = " + clock + "\n";
    }

    std::string library_root = configValue(base_config, "HLS_lib_path");
    if (library_root.empty())
        library_root = discoverHLSLibraryPath(original_source_file);
    if (library_root.empty())
    {
        error_message =
            "cannot find HLS_Data_Lib from source path; set HI_HLS_LIB_PATH";
        return false;
    }

    std::string board = requested_board;
    if (board.empty())
    {
        if (const char *board_environment = std::getenv("HI_HLS_BOARD"))
        {
            if (*board_environment)
                board = board_environment;
        }
    }
    if (board.empty())
        board = configValue(base_config, "FPGA_board");
    if (!resolveBoardLibraryPath(library_root, board, selected_board,
                                 selected_library_path, error_message))
    {
        return false;
    }

    std::vector<SourceDirective> directives;
    if (!translateSourcePragmas(rewritten_source_file, top_function, directives,
                                source_pragma_count, error_message))
    {
        return false;
    }

    std::ofstream output(output_config_file);
    if (!output)
    {
        error_message = "cannot create temporary effective config '" +
                        output_config_file + "'";
        return false;
    }
    std::istringstream base_input(base_config);
    std::string base_line;
    while (std::getline(base_input, base_line))
    {
        std::istringstream base_tokens(base_line);
        std::string base_parameter;
        base_tokens >> base_parameter;
        if (base_parameter == "HLS_lib_path" ||
            base_parameter == "FPGA_board")
        {
            continue;
        }
        if (!requested_clock_period.empty() && base_parameter == "clock")
            continue;
        SourceDirective base_directive;
        const bool is_overridden =
            parseConfigDirectiveTarget(base_line, base_directive) &&
            std::any_of(directives.begin(), directives.end(),
                        [&](const SourceDirective &source_directive) {
                            return sameDirectiveTarget(base_directive,
                                                       source_directive);
                        });
        if (!is_overridden)
            output << base_line << '\n';
    }
    if (!requested_clock_period.empty())
        output << "clock = " << requested_clock_period << '\n';
    output << "FPGA_board = " << selected_board << '\n';
    output << "HLS_lib_path = " << selected_library_path << '\n';
    for (const SourceDirective &directive : directives)
    {
        if (!directive.disabled)
            output << directive.config_line << '\n';
    }
    return true;
}

bool ParseMemoryDepthConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, unsigned long long> &memory_depths,
    std::string &error_message)
{
    memory_depths.clear();
    std::ifstream input(config_file);
    if (!input)
    {
        error_message = "cannot open effective config file '" + config_file + "'";
        return false;
    }

    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        std::istringstream tokens(line);
        std::string directive_name;
        tokens >> directive_name;
        if (lowerCopy(directive_name) != "memory_depth")
            continue;

        std::string arguments((std::istreambuf_iterator<char>(tokens)),
                              std::istreambuf_iterator<char>());
        ParsedPragmaOptions options;
        std::string parse_error;
        if (!parsePragmaOptions(arguments, options, parse_error) ||
            !validateOptions(options, {"variable", "depth", "scope"}, {},
                             parse_error))
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": invalid memory_depth directive: " + parse_error;
            return false;
        }

        auto variable = options.assignments.find("variable");
        auto depth = options.assignments.find("depth");
        if (variable == options.assignments.end() || variable->second.empty() ||
            depth == options.assignments.end())
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": memory_depth requires variable=<name> and depth=<N>";
            return false;
        }

        auto scope = options.assignments.find("scope");
        if (scope != options.assignments.end() && scope->second != top_function)
            continue;

        unsigned long long parsed_depth = 0;
        try
        {
            std::size_t parsed = 0;
            parsed_depth = std::stoull(depth->second, &parsed);
            if (parsed != depth->second.size() || parsed_depth == 0)
                throw std::invalid_argument("not a positive integer");
        }
        catch (const std::exception &)
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": memory_depth depth must be a positive integer";
            return false;
        }

        if (!memory_depths.emplace(variable->second, parsed_depth).second)
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": duplicate memory_depth for variable '" +
                            variable->second + "'";
            return false;
        }
    }
    return true;
}

bool ParseMemoryInterfaceConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, MemoryInterfaceConfig> &memory_interfaces,
    std::string &error_message)
{
    memory_interfaces.clear();
    std::ifstream input(config_file);
    if (!input)
    {
        error_message = "cannot open effective config file '" + config_file + "'";
        return false;
    }

    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        std::istringstream tokens(line);
        std::string directive_name;
        tokens >> directive_name;
        if (lowerCopy(directive_name) != "memory_interface")
            continue;

        std::string arguments((std::istreambuf_iterator<char>(tokens)),
                              std::istreambuf_iterator<char>());
        ParsedPragmaOptions options;
        std::string parse_error;
        if (!parsePragmaOptions(arguments, options, parse_error) ||
            !validateOptions(options,
                             {"variable", "ports", "read_latency", "scope"},
                             {}, parse_error))
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": invalid memory_interface directive: " + parse_error;
            return false;
        }

        auto variable = options.assignments.find("variable");
        auto ports = options.assignments.find("ports");
        auto read_latency = options.assignments.find("read_latency");
        if (variable == options.assignments.end() || variable->second.empty() ||
            (ports == options.assignments.end() &&
             read_latency == options.assignments.end()))
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": memory_interface requires variable=<name> and at least one of ports or read_latency";
            return false;
        }

        auto scope = options.assignments.find("scope");
        if (scope != options.assignments.end() && scope->second != top_function)
            continue;

        MemoryInterfaceConfig interface_config;
        try
        {
            if (ports != options.assignments.end())
            {
                std::size_t parsed = 0;
                const unsigned long value = std::stoul(ports->second, &parsed);
                if (parsed != ports->second.size() || (value != 1 && value != 2))
                    throw std::invalid_argument("ports");
                interface_config.ports = static_cast<unsigned>(value);
            }
            if (read_latency != options.assignments.end())
            {
                std::size_t parsed = 0;
                const unsigned long value =
                    std::stoul(read_latency->second, &parsed);
                if (parsed != read_latency->second.size() || value == 0 ||
                    value > 64)
                {
                    throw std::invalid_argument("read_latency");
                }
                interface_config.readLatency = static_cast<unsigned>(value);
            }
        }
        catch (const std::exception &)
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": memory_interface ports must be 1 or 2 and read_latency must be in [1, 64]";
            return false;
        }

        if (!memory_interfaces.emplace(variable->second, interface_config).second)
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": duplicate memory_interface for variable '" +
                            variable->second + "'";
            return false;
        }
    }
    return true;
}

bool ParseStorageBindingConfig(
    const std::string &config_file, const std::string &top_function,
    std::map<std::string, StorageBindingConfig> &storage_bindings,
    std::string &error_message)
{
    storage_bindings.clear();
    std::ifstream input(config_file);
    if (!input)
    {
        error_message = "cannot open effective config file '" + config_file + "'";
        return false;
    }

    std::string line;
    unsigned line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        std::istringstream tokens(line);
        std::string directive_name;
        tokens >> directive_name;
        if (lowerCopy(directive_name) != "bind_storage")
            continue;

        std::string arguments((std::istreambuf_iterator<char>(tokens)),
                              std::istreambuf_iterator<char>());
        ParsedPragmaOptions options;
        std::string parse_error;
        if (!parsePragmaOptions(arguments, options, parse_error) ||
            !validateOptions(options,
                             {"variable", "scope", "type", "impl", "latency"},
                             {}, parse_error))
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": invalid bind_storage directive: " + parse_error;
            return false;
        }

        auto variable = options.assignments.find("variable");
        auto type = options.assignments.find("type");
        auto implementation = options.assignments.find("impl");
        if (variable == options.assignments.end() || variable->second.empty() ||
            type == options.assignments.end() ||
            implementation == options.assignments.end())
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": bind_storage requires variable, type, and impl";
            return false;
        }
        auto scope = options.assignments.find("scope");
        if (scope != options.assignments.end() && scope->second != top_function)
            continue;

        StorageBindingConfig binding;
        binding.type = lowerCopy(type->second);
        binding.implementation = lowerCopy(implementation->second);
        if (binding.type != "ram_1p" && binding.type != "ram_2p")
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": bind_storage type must be ram_1p or ram_2p";
            return false;
        }
        if (binding.implementation != "auto" &&
            binding.implementation != "bram" &&
            binding.implementation != "uram" &&
            binding.implementation != "lutram")
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": bind_storage impl must be auto, bram, uram, or lutram";
            return false;
        }
        auto latency = options.assignments.find("latency");
        if (latency != options.assignments.end())
        {
            try
            {
                std::size_t parsed = 0;
                const unsigned long value =
                    std::stoul(latency->second, &parsed);
                if (parsed != latency->second.size() || value == 0 ||
                    value > 64)
                {
                    throw std::invalid_argument("latency");
                }
                binding.readLatency = static_cast<unsigned>(value);
            }
            catch (const std::exception &)
            {
                error_message = config_file + ":" +
                                std::to_string(line_number) +
                                ": bind_storage latency must be in [1, 64]";
                return false;
            }
        }
        if (!storage_bindings.emplace(variable->second, binding).second)
        {
            error_message = config_file + ":" + std::to_string(line_number) +
                            ": duplicate bind_storage for variable '" +
                            variable->second + "'";
            return false;
        }
    }
    return true;
}

void Parse_Config(const char *config_file_name, std::map<std::string, int> &LoopLabel2UnrollFactor,
                  std::map<std::string, int> &LoopLabel2II)
{
    std::string tmp_s;
    std::string tmpStr_forParsing;
    std::ifstream *config_file = new std::ifstream(config_file_name);
    while (getline(*config_file, tmp_s))
    {
        tmp_s = removeExtraSpace(tmp_s);
        std::stringstream iss(tmp_s);
        std::string param_name;
        iss >> param_name; //  get the name of parameter

        switch (hash_(param_name.c_str()))
        {
        case hash_compile_time("clock"):
            consumeEqual(iss);
            iss >> clock_period_str;
            // clock_period = std::stod(clock_period_str);
            break;

        case hash_compile_time("HLS_lib_path"):
            consumeEqual(iss);
            iss >> HLS_lib_path;
            break;

        case hash_compile_time("array_partition"):
            // parseArrayPartition(iss);
            break;

        case hash_compile_time("loop_unroll"):
            parseLoopUnroll(iss, LoopLabel2UnrollFactor);
            break;

        case hash_compile_time("loop_pipeline"):
            parseLoopPipeline(iss, LoopLabel2II);
            break;

        case hash_compile_time("array_port"):
            parseArrayPortNum(iss);
            break;

        case hash_compile_time("func_dataflow"):
            parseFuncDataflow(iss);
            break;

        case hash_compile_time("local_array"):
            parseLocalArray(iss);
            break;

        case hash_compile_time("resource_limit"):
            parseResourceLimit(iss);
            break;

        case hash_compile_time("all_sub_function_inline"):
            all_sub_function_inline = 1;
            break;

        default:
            break;
        }
    }
    delete config_file;
}

// parse the argument for array partitioning
void parseArrayPartition(std::stringstream &iss)
{
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.scopeStr = (tmp_val);
            break;

        case hash_compile_time("dim"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.dim = std::stoi(tmp_val)-1;  // count from dim="0" to match the storage
            // format
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.partition_factor = std::stoi(tmp_val);
            break;

        case hash_compile_time("cyclic"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.cyclic = 1;
            break;

        case hash_compile_time("block"):
            consumeEqual(iss);
            iss >> tmp_val;
            // ans_pragma.cyclic = 0;
            break;

        case hash_compile_time("complete"):
            break;

        default:
            print_error("Wrong argument for array partitioning.");
            break;
        }
    }
}

// parse the argument for loop unrolling
void parseLoopUnroll(std::stringstream &iss, std::map<std::string, int> &LoopLabel2UnrollFactor)
{
    int factor = -1;
    std::string loopLabel("");
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            loopLabel = (tmp_val);
            break;

        case hash_compile_time("factor"):
            consumeEqual(iss);
            iss >> tmp_val;
            factor = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for loop unrolling.");
            break;
        }
    }
    assert(loopLabel != "" && factor > -1);
    LoopLabel2UnrollFactor[loopLabel] = factor;
    llvm::errs() << "unroll loop: " << loopLabel << " and factor=" << factor << "\n";
}

// parse the argument for loop pipelining
void parseLoopPipeline(std::stringstream &iss, std::map<std::string, int> &LoopLabel2II)
{
    int factor = -1;
    std::string loopLabel("");
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("label"):
            consumeEqual(iss);
            iss >> tmp_val;
            loopLabel = (tmp_val);
            break;

        case hash_compile_time("II"):
            consumeEqual(iss);
            iss >> tmp_val;
            factor = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for loop unrolling.");
            break;
        }
    }
    assert(loopLabel != "" && factor > -1);
    LoopLabel2II[loopLabel] = factor;
}

// parse the argument for array port num setting
void parseArrayPortNum(std::stringstream &iss)
{
    std::string targetStr, scopeStr;
    int port_num;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("port_num"):
            consumeEqual(iss);
            iss >> tmp_val;
            port_num = std::stoi(tmp_val); // count from dim="0" to match the storage format
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array port num setting.");
            break;
        }
    }
}

// parse the argument for function dataflow setting
void parseFuncDataflow(std::stringstream &iss)
{
    std::string targetStr, scopeStr;
    int port_num;
    bool enable = false;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            enable = true;
            dataflowApplied = true;
            break;

        case hash_compile_time("disable"):
            enable = false;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for array port num setting.");
            break;
        }
    }
}

// parse the argument for local array setting
void parseLocalArray(std::stringstream &iss)
{
    std::string targetStr, scopeStr;
    bool enable = false;
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;

        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("variable"):
            consumeEqual(iss);
            iss >> tmp_val;
            targetStr = (tmp_val);
            break;

        case hash_compile_time("scope"):
            consumeEqual(iss);
            iss >> tmp_val;
            scopeStr = (tmp_val);
            break;

        case hash_compile_time("enable"):
            enable = true;
            break;

        case hash_compile_time("disable"):
            enable = false;
            break;

        default:
            llvm::errs() << "wrong argument: " << arg_name << "\n";
            print_error("Wrong argument for local array setting.");
            break;
        }
    }
}

// parse the argument for resource limitation
void parseResourceLimit(std::stringstream &iss)
{
    int factor = -1;
    std::string loopLabel("");
    while (!iss.eof())
    {
        std::string arg_name;
        std::string tmp_val;
        iss >> arg_name; //  get the name of parameter
        switch (hash_(arg_name.c_str()))
        {
        case hash_compile_time("DSP"):
            consumeEqual(iss);
            iss >> tmp_val;
            DSP_limit = std::stoi(tmp_val);
            break;

        case hash_compile_time("FF"):
            consumeEqual(iss);
            iss >> tmp_val;
            FF_limit = std::stoi(tmp_val);
            break;

        case hash_compile_time("LUT"):
            consumeEqual(iss);
            iss >> tmp_val;
            LUT_limit = std::stoi(tmp_val);
            break;

        case hash_compile_time("BRAM"):
            consumeEqual(iss);
            iss >> tmp_val;
            BRAM_limit = std::stoi(tmp_val);
            break;

        case hash_compile_time("clock_num"):
            consumeEqual(iss);
            iss >> tmp_val;
            ClockNum_limit = std::stoi(tmp_val);
            break;

        default:
            print_error("Wrong argument for resource limitation.");
            break;
        }
    }
}
