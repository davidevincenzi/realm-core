#ifndef REALM_UTIL_CLI_ARGS_HPP
#define REALM_UTIL_CLI_ARGS_HPP

#include <vector>

#include "realm/util/string_view.hpp"

namespace realm::util {

struct CliParseResult {
    StringView program_name;
    std::vector<StringView> unmatched_arguments;
};

class CliFlag {
public:
    explicit CliFlag(StringView name, char short_name = '\0')
        : m_name(name)
        , m_short_name(short_name)
    {
    }

    operator bool() const noexcept
    {
        return m_found;
    }

    StringView name() const noexcept
    {
        return m_name;
    }
    char short_name() const noexcept
    {
        return m_short_name;
    }

protected:
    friend CliParseResult parse_arguments(int argc, char** argv, std::initializer_list<CliFlag*> to_parse);

    virtual void assign(StringView)
    {
        m_found = true;
    }

    virtual bool expects_value()
    {
        return false;
    }

private:
    bool m_found = false;
    StringView m_name;
    char m_short_name = '\0';
};

class CliArgument : public CliFlag {
public:
    using CliFlag::CliFlag;

    StringView value() const noexcept
    {
        return m_value;
    }

    template <typename T>
    T as() const;

protected:
    friend CliParseResult parse_arguments(int argc, char** argv, std::initializer_list<const CliFlag*> to_parse);

    void assign(StringView value) override
    {
        CliFlag::assign(value);
        m_value = value;
    }

    bool expects_value() override
    {
        return true;
    }

private:
    StringView m_value;
};

class CliParseException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

CliParseResult parse_arguments(int argc, char** argv, std::initializer_list<CliFlag*> to_parse);

} // namespace realm::util

#endif // REALM_UTIL_CLI_ARGS_HPP
