#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: rag2 <smoke|ingest|query> [args]\n";
        return 1;
    }
    std::string cmd = argv[1];
    std::cout << "command: " << cmd << " (not implemented yet)\n";
    return 0;
}
