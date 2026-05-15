#ifndef DIFFMONGER_UTIL_PARSER_HPP
#define DIFFMONGER_UTIL_PARSER_HPP

#include <diffmonger/util/misc.hpp>

#include <variant>
#include <map>
#include <string>
#include <string_view>
#include <memory>
#include <optional>
#include <vector>
#include <charconv>
#include <span>
#include <cassert>
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <ranges>
#include <iomanip>
#include <utility>
#include <filesystem>

// Command line argument parser with source-of-truth generated documentation.
// Unlikely most AST parsers, which use a stateless visitor pattern,
// the current framework uses a stateful visitor pattern
// (i.e., nodes hold state that evolves during parsing).
// The object graph is ephemeral: the client generates it for a single operation
// (parsing or documentation output).
// If the same graph is needed in different contexts, then the client should
// implement a graph factory for producing multiple graph instances.

namespace diffmonger
{
class argument_error : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void intersperse_quoted(auto const &values,
                        auto const &separator,
                        auto &out,
                        char quote = '"',
                        char escape = '\\')
{
    bool first = true;
    for (auto const &val : values) {
        if (!first)
            out << separator;
        else
            first = false;

        // Use std::quoted for proper quoting and escaping
        out << std::quoted(val, quote, escape);
    }
}

namespace parser {
static auto constexpr keyValueSeparator = std::string_view("=");
static auto constexpr appendToken = std::string_view("+");
static auto constexpr assignToken = std::string_view(":");
}

class Parameter
{
public:

    struct Required{};
    struct Defaulted { std::vector<std::string> defaultValue; };
    struct Optional{};

    using VariantT = std::variant<Required, Defaulted, Optional>;

    Parameter(std::string name,
              VariantT type = Required{})
        : name(std::move(name)),
          type(std::move(type))
    {}

    // The implementation extensively assumes that Parameters are memory-stationary;
    // do not re-enable assignment or move.
    Parameter(Parameter const &) = delete;
    Parameter(Parameter &&) = delete;
    Parameter &operator=(Parameter const &) = delete;
    Parameter &operator=(Parameter &&) = delete;

    virtual std::string_view getDocumentation() const = 0;
    virtual std::vector<std::string> getConstraints() const = 0;
    virtual void check() const = 0;

    virtual void set(std::vector<std::string_view>) = 0;

    virtual ~Parameter() = default;

    // Must never stop being a const member;
    // allowing names to change would break lookup.
    std::string const name;
    VariantT const type;
};

template <typename>
class SubCommand;

class Command
{
    class Parent
    {
        friend class Command;
        friend class RootCommand;
        Parent(Command *value) : value(value) {}
        Command *value;
    };

    template <typename Map>
    static auto _lookup(std::string_view const key, Map &&map)
    {
        auto const it = map.find(key);
        return
            it == map.end()
            ? decltype(propagate_const_ptr(it->second)){}
            : propagate_const_ptr(it->second);
    }

    template <typename Map>
    static auto mapToValueRange(Map &&map)
    {
        return map |
            std::views::transform(
                [] (auto &pair) -> auto &
                { return *propagate_const_ptr(pair.second); });
    }

    static auto &_root(auto &&self)
    {
        auto *parent = &self.parent;
        while (parent->parent)
            parent = parent->parent;
        return parent;
    }
public:
    Command(Parent const &parent, std::string name)
        : name(std::move(name)), parent(parent.value)
    {
        using namespace parser;
        if (name.contains(keyValueSeparator)
            || name.contains(assignToken)
            || name.contains(appendToken))
            throw argument_error("Invalid name: " + name);
    }

    // The implementation extensively assumes that Commands are memory-stationary;
    // do not re-enable assignment or move.
    Command(Command const &) = delete;
    Command(Command &&) = delete;
    Command &operator=(Command const &) = delete;
    Command &operator=(Command &&) = delete;

    template <typename T, typename Self, typename ...Params>
    static T &emplaceSubCommand(Self &self, Params && ...params)
    {
        auto uniqueptr = std::make_unique<T>(Parent{ &self },
                                             std::forward<Params>(params)...);

        assert(uniqueptr->parent == &self);

        std::string_view const key = uniqueptr->name;

        T *ptr = uniqueptr.get();

        auto [it, success] = self.subcommands.try_emplace(key, std::move(uniqueptr));

        if (!success)
            throw std::runtime_error(
                (std::stringstream{}
                 << "Duplicate subcommand: "
                 << std::quoted(key)).str());

        return *ptr;
    }

    Command *lookupCommand(std::string_view key) { return _lookup(key, subcommands); }
    Command const *lookupCommand(std::string_view key) const
    { return _lookup(key, subcommands); }
    Parameter *lookupParameter(std::string_view key) { return _lookup(key, parameters); }
    Parameter const *lookupParameter(std::string_view key) const
    { return _lookup(key, parameters); }

    virtual std::string_view getSelfDocumentation() const = 0;

    template <typename T>
    T &addParameter(std::unique_ptr<T> parameter)
    {
        std::string_view const key = parameter->name;
        auto *ptr = parameter.get();
        auto [it, success] = parameters.try_emplace(key, std::move(parameter));
        if (!success)
            throw std::runtime_error(
                (std::stringstream{}
                 << "Duplicate parameter: "
                 << std::quoted(key)).str());

        return *ptr;
    }

    template <typename T, typename ...Params>
    T &emplaceParameter(Params &&...params)
    {
        return addParameter(std::make_unique<T>(std::forward<Params>(params)...));
    }

    auto getSubCommands() { return mapToValueRange(subcommands); }
    auto getSubCommands() const { return mapToValueRange(subcommands); }

    auto getParameters() { return mapToValueRange(parameters); }
    auto getParameters() const { return mapToValueRange(parameters); }

    Command const *getMaybeParent() const { return parent; }

    // If called, then it's called before before_subcommand() or execute().
    virtual void unrecognisedParameters(
        std::map<std::string_view, std::vector<std::string_view>> unrecognised_params)
    {
        for (auto const &pair: unrecognised_params)
            throw argument_error(
                (std::stringstream{} <<
                 "Unrecognised parameter: " <<
                 std::quoted(pair.first)).str());
    }

    // Called upon starting a parsing traversal on the given command.
    // Then intention for this callback is that it affords the command the opportunity
    // to check it's parent command was given the options necessary for the current command.
    // This functionality could also be implemented with the finalise() callback,
    // but using initiate() has benefits if the finalisation logic is subcommand dependent.
    // While it's perfectly possible to switch() on the reference given to finalise(),
    // using initiate() eliminates the branching and allows for more deterministic
    // code paths. The reason finalise() is also supported is because it is more convenient
    // in those cases where the finalisation logic isn't dependent upon the subcommand.
    virtual void on_enter() {}
    // Finalise.
    virtual void before_subcommand(Command &subcommand) { (void) subcommand; }
    // Called in place of before_subcommand() if this is the final subcommand.
    virtual void execute()
    {
        throw argument_error("Subcommand expected");
    }

    virtual ~Command() = default;

    // Must never stop being a const member;
    // allowing names to change would break lookup.
    std::string const name;
private:
    template <typename>
    friend class SubCommand;

    friend class RootCommand;

    Command * const parent;

    // The keys are owned by their respective Command/Parameter objects,
    // which are memory stable (i.e., due to being owned as std::unique_ptrs),
    // and which have the key values as const members.
    // While there is an argument for changing to std::string to avoid risk of future
    // memory safety issues, there is no meaningful benefit:
    // the value type (VariantT) necessarily contains pointers to the respective
    // Command/Parameter objects, so if the string_view is unsafe, so is the value type.
    // Important: the map needs to be declared _after_ both subcommands and parameters
    // to ensure the map gets destroyed first, theyerby avoiding lifetime issues.
    std::map<std::string_view, std::unique_ptr<Command>> subcommands;
    std::map<std::string_view, std::unique_ptr<Parameter>> parameters;
};

template <typename Parent_>
class SubCommand : public Command
{
public:
    using Command::Command;

    Parent_ const &getParent() const { return *static_cast<Parent_ *>(Command::parent); }
    Parent_ &getParent() { return *static_cast<Parent_ *>(Command::parent); }
};

class RootCommand : public Command
{
public:
    RootCommand(std::string name)
        : Command(Command::Parent{nullptr}, std::move(name))
    {}
};


namespace parameter {

// Not worth having a base class; more fiddly than it's worth.
// Could also "if constexpr" on the Type within the definitions,
// but the return type of get() depends upon Type, and so the code
// would be mildly harder to interpret.
template <typename Type, typename T>
class Storage;

template <typename T>
class Storage<Parameter::Required, T>
{
public:
    using value_type = T;

    template <typename U>
    void set(U &&value) { this->value.emplace(std::forward<U>(value)); }

    T const &get() const { return _get(*this); }
    T &get() { return _get(*this); }
private:
    static auto &_get(auto &self)
    {
        if (self.value)
            return self.value.value();
        else
            throw argument_error("Required value not given");
    }

    std::optional<T> value;
};

template <typename T>
class Storage<Parameter::Defaulted, T>
{
public:
    using value_type = T;

    template <typename U>
    Storage(U &&initial_value)
        : value(std::forward<U>(initial_value))
    {}

    template <typename U>
    void set(U &&value) { this->value.emplace(std::forward<U>(value)); }

    T const &get() const { return _get(*this); }
    T &get() { return _get(*this); }
private:
    static auto &_get(auto &self)
    {
        assert(self.value);
        return self.value.value();
    }
    std::optional<T> value;
};

template <typename T>
class Storage<Parameter::Optional, T>
{
public:
    using value_type = T;

    template <typename U>
    void set(U &&value) { this->value.emplace(std::forward<U>(value)); }
    // These methods exist just to provide type-polymorphic access to value.
    // Otherwise, value could just be made public.
    std::optional<T> const &get() const { return value; }
    std::optional<T> &get() { return value; }
private:
    std::optional<T> value;
};

class Scalar
{
public:
    template <typename T>
    auto process(T &&args) const
    {
        if (std::as_const(args).size() != 1)
            throw argument_error("Wrong number of args");
        else
            return front(std::forward<T>(args));
    }

    auto describe() const
    {
        return "Scalar";
    }

private:
    template <typename T>
    static auto front(T const &x)
    { return x.front(); }

    template <typename T>
    static auto front(T &&x)
    { return std::move(std::forward<T>(x).front()); }
};

class Vector
{
public:
    // Note: {in} may be both vector<string_view> and vector<string>.
    // This is because it may come from the command line arguments (string_view case),
    // or it may come from a defaulted_value (string case).
    std::vector<std::string> process(auto &&in) const
    {
        std::vector<std::string> out;
        for (auto const &x: in)
            out.emplace_back(x);
        return out;
    }

    auto describe() const
    {
        return "Vector";
    }
};

template <typename T=int64_t>
struct Integer
{
    T process(std::string_view const arg) const
    {
        T out{};
        if (std::from_chars(arg.begin(), arg.end(), out).ec != std::errc{})
            throw argument_error(std::string{"invalid number: "}.append(arg));

        return out;
    }

    auto describe() const
    {
        return "Integer";
    }
};

struct NonNegative
{
    auto process(auto const x) const
    {
        if (x < 0)
            throw argument_error(
                std::string("negative value: ").append(std::to_string(x)));
        else
            return std::make_unsigned_t<decltype(x)>(x);
    }

    auto describe() const
    {
        return "Non-negative";
    }
};

template <typename T>
struct Choice
{
    using result_type = std::pair<std::string const &, T const &>;
    using MapT = std::map<std::string, T, std::less<>>;

    MapT map;

    Choice(MapT map)
        : map(std::move(map))
    {}

    result_type process(std::string_view const arg) const
    {
        auto const it = map.find(arg);

        if (it == map.end())
            throw argument_error(
                (std::stringstream{} << "invalid value for choice: " <<
                 std::quoted(arg)).str());
        else
            return {it->first, it->second};
    }

    auto describe() const
    {
        std::ostringstream ss;
        ss << "Choice of: ";
        intersperse_quoted(
            map |
            std::views::transform([] (auto &pair) { return pair.first; }),
            ", ",
            ss);
        ss << ".";
        return std::move(ss).str();
    }
};

struct ExistingFile
{
    auto process(std::filesystem::path x) const
    {
        if (std::filesystem::exists(x))
            throw std::runtime_error(
                std::string("File does not exist: ").append(x.native()));
        else
            return x;
    }

    auto describe() const
    {
        return "Path to an existing file";
    }
};

struct Boolean : Choice<bool>
{
    Boolean() : Choice<bool>(MapT{{"true", true}, {"false", false}})
    {}
    bool process(auto &&arg) const
    {
        return Choice::process(std::forward<decltype(arg)>(arg)).second;
    }
};

namespace detail {

template <int I, typename Arg, typename ...Classes>
inline auto process(Arg &&value, std::tuple<Classes...> const &chain)
{
    if constexpr (I == std::tuple_size_v<std::tuple<Classes...>>)
        return std::forward<Arg>(value);
    else
        return process<I + 1>(std::get<I>(chain).process(std::forward<Arg>(value)), chain);
}
}

template <typename Type, typename T, typename ...Classes>
struct ParameterHelper : Parameter
{
    using Chain = std::tuple<Classes...>;
    using Storage_ = Storage<Type, T>;
    using value_type = T;

    ParameterHelper(
        Type type,
        std::string name,
        std::string documentation)
        : ParameterHelper(std::move(type),
                          std::move(name),
                          std::move(documentation),
                          Chain{})
    {
    }

    ParameterHelper(
        std::string name,
        std::string documentation)
        : ParameterHelper(Type{},
                          std::move(name),
                          std::move(documentation),
                          Chain{})
    {
    }

    template <typename Type_, typename Chain_>
    ParameterHelper(
        Type_ &&type,
        std::string name,
        std::string documentation,
        Chain_ &&chain_)
        : Parameter(std::move(name), Parameter::VariantT{std::forward<Type_>(type)}),
          chain(std::forward<Chain_>(chain_)),
          documentation(std::move(documentation)),
          storage(
              [&]
              {
                  if constexpr (std::is_same_v<std::decay_t<Type_>, Parameter::Defaulted>)
                      return Storage<Type_, value_type>{
                          detail::process<0>(
                              std::get<Parameter::Defaulted>(Parameter::type).defaultValue,
                              this->chain)};
                  else
                      return Storage<Type_, value_type>{};
              }())
    {
    }

    std::string_view getDocumentation() const override
    {
        return documentation;
    }

    void set(std::vector<std::string_view> values) override
    {
        storage.set(detail::process<0>(std::move(values), chain));
    }

    std::vector<std::string> getConstraints() const override
    {
        std::vector<std::string> out;
        std::apply(
            [&](auto const&... funcs) { (..., out.push_back(funcs.describe())); },
            chain);
        return out;
    }

    void check() const override { get(); }

    auto &get() { return _get(*this); }
    auto &get() const { return _get(*this); }

private:
    Chain chain;
    std::string documentation;
    Storage_ storage;

    static auto &_get(auto &&self)
    {
        try
        {
            return self.storage.get();
        } catch (argument_error const &e)
        {
            throw argument_error(std::string("Regarding parameter ")
                                     .append(self.name).append(": ")
                                     .append(e.what()));
        }
    }
};

}

/* Syntax is (eBNF):
 *
 *   argv = subcommandargv { <$> subcommandargv }
 *   subcommandargv = subcommandname { <$> param }
 *   param = paramname paramvalue
 *   paramvalue = ":" [assignvalue] { <$> assignvalue }
 *              | "=" value { <$> assignvalue }
 *              | "+=" value
 *   assignvalue = "=" value
 *
 * where "argv" is the input arguments array (i.e., as represented by argc/argv in main()),
 * "<$>" represents advancing to the next element in the argv array,
 * and "value" represents arbitrary data (binary or otherwise).
 *
 * This syntax allows for things like:
 *   command param=value "${USERARGS[@]}"
 * where USERARGS can now choose to override param:
 *   USERARGS=("param=newvalue")
 * or append additional elements to the param vector:
 *   USERARGS=("param+=value1" "param+=value2" ...)
 *
 * Additionally, this syntax supports setting vectors to empty vectors:
 *   command param:  # param is set but empty
 *
 * This is just a special case of the more general syntax:
 *   command param:=value0 =value1 =value2 ...
 *
 * The special ":=" syntax is necessary to differentiate setting a parameter to the
 * empty vector from traversing into a new subcommand. It allows parameter names
 * and command names to occupy different namespaces.
 *
 *
 * In most shells, it's trivial to pass arrays of values in such a way that is completely
 * safe from the normal escaping nightmare:
 *
 *   command param:"${paramvalues[@]/#/=}"
 *
 * It's similarly trivial to pass arrays of values to append to existing initial values:
 *
 *   command param=initial0 =initial1 "${additionalvalues[@]/#/param+=}"
 */
class Parser
{
    static void walkup(Command const *node, auto const &f)
    {
        while (node)
        {
            f(*node);
            node = node->getMaybeParent();
        }
    }

    static void contextstr(Command const &node, std::ostringstream &out)
    {
        std::vector<std::string_view> vec;

        walkup(&node, [&vec] (Command const &node) { vec.push_back(node.name); });

        std::reverse(vec.begin(), vec.end());

        assert(!vec.empty());

        auto it = vec.begin();

        for (out << *it++; it != vec.end(); ++it)
            out << ">>" << *it;
    }

    static void contextstr(Command const &node,
                           Parameter const &param,
                           std::ostringstream &out)
    {
        contextstr(node, out);
        out << ">>" << param.name;
    }

    template <typename F>
    static auto with_context(Command const &command, Parameter const *param, F &&f)
    {
        try {
            return f();
        } catch (argument_error const &e)
        {
            std::ostringstream ss;
            ss << "Error while processing ";
            param
                ? contextstr(command, *param, ss)
                : contextstr(command, ss);
            ss << ": " << e.what();
            throw argument_error(std::move(ss).str());
        }
    }

    static auto with_contextual_error(
        Command const &command,
        Parameter const * const param,
        std::string_view const str)
    {
        std::ostringstream ss;
        ss << "Error while processing ";
        param
            ? contextstr(command, *param, ss)
            : contextstr(command, ss);
        ss << ": " << str;
        throw argument_error(std::move(ss).str());
    }

    static void finalise(Command &command,
                         std::map<std::string_view, std::vector<std::string_view>> &map)
    {
        for (auto &parameter: command.getParameters())
            with_context(
                command, &parameter,
                [&]
                {
                    auto const nodehandle = map.extract(parameter.name);
                    if (!nodehandle.empty())
                        parameter.set(std::move(nodehandle.mapped()));
                    parameter.check();
                });

        with_context(
            command, nullptr,
            [&] { command.unrecognisedParameters(std::move(map)); });

        map = {};
    }
public:
    static void parse(
        Command *actionableCommand,
        // Excluding settableCommand->getName().
        std::span<std::string_view const> args)
    {
        using namespace parser;

        if (!actionableCommand)
            throw std::logic_error("parse() given nullptr");

        with_context(*actionableCommand, nullptr,
                     [&] { actionableCommand->on_enter(); });

        std::map<std::string_view, std::vector<std::string_view>> keyValues;

        auto cur = args.begin();
        while (cur != args.end())
        {
            assert(actionableCommand);

            auto const it = std::search(cur->begin(), cur->end(),
                                        keyValueSeparator.begin(),
                                        keyValueSeparator.end());

            // Possibly ends with "+" or ":"
            std::string_view const keyAndMaybeToken{cur->begin(), it};

            bool const endsWithAssignToken = keyAndMaybeToken.ends_with(assignToken);

            if ((it == cur->end()) && !endsWithAssignToken)
            { // cur is a command.
                Command *const nextActionableCommand =
                    actionableCommand->lookupCommand(keyAndMaybeToken);

                if (!nextActionableCommand)
                    with_contextual_error(
                        *actionableCommand, nullptr,
                        (std::stringstream{}
                        << "Unrecognised command: "
                        << std::quoted(keyAndMaybeToken)).str());

                finalise(*actionableCommand, keyValues);

                with_context(
                    *actionableCommand, nullptr,
                    [&]
                    {
                        actionableCommand->before_subcommand(*nextActionableCommand);
                    });

                actionableCommand = nextActionableCommand;

                with_context(*actionableCommand, nullptr,
                             [&] { actionableCommand->on_enter(); });

                ++cur;
            } else
            { // '=' present; cur is a parameter.
                // Either "paramxyz=...", "paramxyz:=...", or "paramxyz+=..."

                if (keyAndMaybeToken.ends_with(appendToken))
                { // Appending, not assigning; "paramxyz+=..."
                    std::string_view const key{keyAndMaybeToken.begin(),
                                               keyAndMaybeToken.end() - appendToken.length()};
                    auto [valuesit, _] = keyValues.try_emplace(key);
                    valuesit->second.emplace_back(it + keyValueSeparator.length(),
                                                  cur->end());
                    ++cur;
                } else
                { // Assigning, not appending.
                    std::string_view const key{
                        cur->begin(), it - (endsWithAssignToken ? assignToken.length() : 0)};
                    auto [valuesit, _] = keyValues.try_emplace(key);
                    auto &values = valuesit->second;

                    values.clear();

                    // Any values present?
                    if (it == cur->end())
                        ++cur;
                    else
                    { // Values present.
                        // Note: no need to check bounds when incrementing by
                        // keyValueSeparator.length():
                        // we already know keyValueSeparator present.
                        values.emplace_back(it + keyValueSeparator.length(), cur->end());

                        for (++cur; cur != args.end(); ++cur)
                        {
                            if (cur->starts_with(keyValueSeparator))
                                // Note: no need to check bounds when incrementing by
                                // keyValueSeparator.length():
                                // starts_with() guarantees we won't run off the end...
                                values.emplace_back(cur->begin() + keyValueSeparator.length(),
                                                    cur->end());
                            else
                                // cur is the next key.
                                break;
                        }
                    }
                }
            } // <-- else
            // Invariant: cur is the next key (or args.end()).
        } // <-- while

        finalise(*actionableCommand, keyValues);

        with_context(
            *actionableCommand, nullptr,
            [&] { actionableCommand->execute(); });
    }
};

inline void parse(Command &command,
                  // Ignores argc[0].
                  int argc, char const *const *const argv)
{
    std::vector<std::string_view> args;
    for (int i=1; i != argc; ++i)
        args.push_back(argv[i]);

    Parser::parse(&command, args);
}

namespace documenter {

class Indenter
{
public:
    Indenter(int level = 0, int nindents = 2)
        : level(level), nindents(nindents)
    {}

    Indenter(Indenter const &) = default;

    auto &append(std::string &out) const
    {
        for (int i=0, n = level*nindents; i != n; ++i)
            out.push_back(' ');
        return out;
    }

    void appendAndIndentNewlines(std::string_view str, std::string &out) const
    {
        auto const end = str.end();
        auto it = str.begin();
        while (it != end)
        {
            auto next = std::find(it, end, '\n');
            std::string_view postfix;
            if (next != end)
                ++next;
            else
                // Add a trailing \n only if documentation didn't already end in one.
                postfix = "\n";
            append(out).append(std::string_view(it, next)).append(postfix);
            it = next;
        }
    }

    Indenter next() const
    {
        return { level + 1, nindents };
    }

    auto withIndentation(auto &&f) const
    {
        return std::forward<decltype(f)>(f)(next());
    }
private:
    int level;
    int nindents;
};

inline void document(Parameter const &parameter, Indenter indenter, std::string &out)
{
    indenter.append(out).append(parameter.name).append(":\n");
    indenter.withIndentation([&] (auto indenter)
    {
        struct Visitor
        {
            std::string &out;
            void operator()(Parameter::Required) const
            {
                out.append("Required");
            }
            void operator()(Parameter::Optional) const
            {
                out.append("Optional");
            }
            void operator()(Parameter::Defaulted const &value) const
            {
                std::ostringstream ss{std::move(out), std::ios::ate};
                ss << "Defaulted with: ";
                intersperse_quoted(value.defaultValue, " ", ss);
                out = std::move(ss).str();
                out.append(".");
            }
        };
        std::visit(Visitor{indenter.append(out)}, parameter.type);
        indenter.append(out.append("\n")).append("Constraints:\n");
        indenter.withIndentation([&] (auto indenter)
        {
            auto const constraints = parameter.getConstraints();
            for (auto const &constraint: constraints)
                indenter.append(out).append("- ").append(constraint).push_back('\n');
        });
        indenter.appendAndIndentNewlines(parameter.getDocumentation(), out);
    });
}

inline void document(Command const &command, Indenter indenter, std::string &out)
{
    indenter.append(out).append(command.name).append(":\n");

    indenter.withIndentation([&] (auto indenter)
    {
        indenter.appendAndIndentNewlines(command.getSelfDocumentation(), out);

        if (!command.getParameters().empty())
        {
            indenter.append(out).append("Parameters:\n");

            for (auto const &parameter: command.getParameters())
                document(parameter, indenter.next(), out);
        }

        if (!command.getSubCommands().empty())
        {
            indenter.append(out).append("Sub-Commands:\n");
            for (auto const &subcommand: command.getSubCommands())
                document(subcommand, indenter.next(), out);
        }
    });
}

inline std::string document(Command &command)
{
    std::string out;
    document(command, Indenter{}, out);
    return out;
}

}

}

#endif
