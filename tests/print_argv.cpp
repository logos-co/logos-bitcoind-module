// Prints the flags the module starts Core with for a network's settings, as JSON:
//   print_argv <network> '<settings json>'
#include <iostream>

#include "../src/bitcoind_argv.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: print_argv <network> '<settings json>'\n";
        return 2;
    }
    const nlohmann::json settings = nlohmann::json::parse(argv[2]);
    std::cout << bitcoind_argv::argvFor(argv[1], settings, 0).dump() << "\n";
    return 0;
}
