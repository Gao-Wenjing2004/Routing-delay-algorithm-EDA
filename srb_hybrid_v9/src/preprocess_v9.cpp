#include "srb_core_v9.hpp"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 7) {
            std::cerr << "usage: preprocess_v9 Inst.json Port.json Arc.json Net.json Gap.json graph.bin\n";
            return 2;
        }
        SRBSolver solver;
        solver.load(argv[1], argv[2], argv[3], argv[4], argv[5]);
        solver.save_binary(argv[6]);
        solver.print_stats();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
