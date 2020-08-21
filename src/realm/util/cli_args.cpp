#include "realm/util/cli_args.hpp"

namespace realm::util {

CliParseResult parse_arguments(int argc, char** argv, std::initializer_list<CliFlag*> to_parse)
{
    CliParseResult result;
    result.program_name = argv[0];
    for (int i = 1; i < argc; ++i) {
        StringView cur_arg(argv[i]);

        auto it = std::find_if(to_parse.begin(), to_parse.end(), [&](const CliFlag* flag) {
            if (cur_arg.starts_with('-')) {
                cur_arg.remove_prefix(1);
                if (cur_arg.starts_with('-')) {
                    if (cur_arg.substr(1) == flag->name()) {
                        return true;
                    }
                }
                if (cur_arg.size() == 2 && cur_arg[1] == flag->short_name()) {
                    return true;
                }
            }
            return false;
        });
        if (it == to_parse.end()) {
            result.unmatched_arguments.push_back(cur_arg);
            continue;
        }

        CliFlag* arg_holder = *it;
        if (!arg_holder->expects_value()) {
            arg_holder->assign(StringView{});
            continue;
        }

        if (auto eq_pos = cur_arg.find_first_of('='); eq_pos != StringView::npos && eq_pos < cur_arg.size()) {
            arg_holder->assign(cur_arg.substr(eq_pos + 1));
        }
        else {
            ++i;
            if (i == argc) {
                throw CliParseException("not enough arguments to parse argument with value");
            }
            arg_holder->assign(StringView(argv[i]));
        }
    }

    return result;
}

template <>
std::string CliArgument::as<std::string>() const
{
    return static_cast<std::string>(m_value);
}

template <>
int64_t CliArgument::as<int64_t>() const
{
    int64_t val = std::strtol(m_value.data(), nullptr, 10);
    if (errno == ERANGE) {
        throw CliParseException("parsing integer argument produced an integer out-of-range");
    }
    if (val == 0 && m_value.size() != 0) {
        throw CliParseException("integer argument parsed to zero, but argument was more than 1 character");
    }
    return val;
}

} // namespace realm::util
