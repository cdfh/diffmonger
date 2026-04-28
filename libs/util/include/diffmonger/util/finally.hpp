#ifndef DIFFMONGER_FINALLY_HPP
#define DIFFMONGER_FINALLY_HPP

#include <utility>
#include <type_traits>
#include <optional>

namespace diffmonger {

// Don't want to bring in GSL or Boost just for this...

template <typename F>
struct final_action
{
    final_action(final_action const &) = default;
    final_action(final_action &&) = default;
    final_action& operator=(final_action const &) = default;
    final_action& operator=(final_action &&) = default;

    template <typename F_>
    final_action(F_ &&f) : f{std::forward<F_>(f)}
    {}

    /**
     * Runs the associated action early and resets it so that it does
     * not run again at destruction.
     */
    void run()
    {
        if (f)
        {
            (*f)();
            f.reset();
        }
    }

    ~final_action()
    {
        run();
    }
private:
    std::optional<F> f;
};

template <typename F>
[[nodiscard]] auto finally(F &&f) noexcept
{
    return final_action<std::decay_t<F>>(std::forward<F>(f));
}

}

#endif
