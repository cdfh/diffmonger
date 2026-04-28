#ifndef DIFFMONGER_COMMAND_LINE_INTERFACE_HPP
#define DIFFMONGER_COMMAND_LINE_INTERFACE_HPP

namespace diffmonger {

class RepositoryStructure;
struct RepositoryParams;

namespace cli_interface {

int run(int argc, char const * const argv[]);

}}

#endif
