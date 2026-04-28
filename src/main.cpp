#include "CommandLineInterface.hpp"

#include <diffmonger/util/parser/Parser.hpp>

#include <iostream>
#include <cstdlib>

int main(int argc, char *argv[])
{
    try
    {
        diffmonger::cli_interface::run(argc, argv);
        return 0;
    } catch (std::runtime_error const &e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
