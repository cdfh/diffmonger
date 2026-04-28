#ifndef DIFFMONGER_CSTDIO_UTIL_HPP
#define DIFFMONGER_CSTDIO_UTIL_HPP

#include <filesystem>

namespace diffmonger {

void remove(std::filesystem::path const &path);
void rename(std::filesystem::path const &from, std::filesystem::path const &dest);

}

#endif
