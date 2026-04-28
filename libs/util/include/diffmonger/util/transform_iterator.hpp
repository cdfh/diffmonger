#ifndef DIFFMONGER_TRANSFORM_ITERATOR_HPP
#define DIFFMONGER_TRANSFORM_ITERATOR_HPP

#include <iterator>
#include <type_traits>
#include <utility>

// NOTE: This is currently unused!

// This is an abomination. I can't justify bringing in boost for make_transform_iterator,
// and I don't want to use ranges due to many compilers (e.g., LTS distributions)
// not yet having support.


namespace diffmonger {

template <typename Iterator, typename UnaryFunction>
class transform_iterator {
public:
    using iterator_type = Iterator;
    using function_type = UnaryFunction;

    using reference = decltype(std::declval<UnaryFunction>()(*std::declval<Iterator>()));
    using value_type = std::remove_cvref_t<reference>;
    using difference_type = typename std::iterator_traits<Iterator>::difference_type;
    using iterator_category = typename std::iterator_traits<Iterator>::iterator_category;
    using pointer = void;  // optional, define if necessary

    transform_iterator() = default;

    transform_iterator(Iterator it, UnaryFunction func)
        : iter_(it), func_(func) {}

    reference operator*() const {
        return func_(*iter_);
    }

    transform_iterator& operator++() {
        ++iter_;
        return *this;
    }

    transform_iterator operator++(int) {
        transform_iterator tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const transform_iterator& other) const {
        return iter_ == other.iter_;
    }

    bool operator!=(const transform_iterator& other) const {
        return !(*this == other);
    }

    Iterator base() const { return iter_; }

private:
    Iterator iter_;
    UnaryFunction func_;
};

template <typename Iterator, typename UnaryFunction>
transform_iterator<Iterator, UnaryFunction>
make_transform_iterator(Iterator it, UnaryFunction func) {
    return transform_iterator<Iterator, UnaryFunction>(it, func);
}
}
#endif
