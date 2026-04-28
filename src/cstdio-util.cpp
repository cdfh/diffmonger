#include "cstdio-util.hpp"

#include <cstdio>

namespace diffmonger {

void remove(std::filesystem::path const &path)
{
    int const result = std::remove(path.c_str());
    if (result)
        throw std::system_error(
            errno, std::generic_category(),
            std::string("Could not remove ").append(path.native()));
}

void rename(std::filesystem::path const &from,
            std::filesystem::path const &dest)
{
    int const result = std::rename(from.c_str(), dest.c_str());
    if (result)
        throw std::system_error(
            errno, std::generic_category(),
            std::string()
            .append("Could not rename ")
            .append(from.native())
            .append(" to ")
            .append(dest.native()));
}

}
