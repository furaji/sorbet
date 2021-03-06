#include <fstream>

constexpr int MAX_PROC_ARITY = 10;

using namespace std;

void emitProc(ofstream &out, int arity) {
    out << "class Proc" << arity << " < Proc" << '\n';
    out << "  Return = type_member(:out)" << '\n';
    for (int i = 0; i < arity; ++i) {
        out << "  Arg" << i << " = type_member(:in)" << '\n';
    }

    out << '\n';

    out << "  sig {params(" << '\n';
    for (int i = 0; i < arity; ++i) {
        out << "    arg" << i << ": "
            << "Arg" << i << "," << '\n';
    }
    out << "  )" << '\n';
    out << "  .returns(Return)}" << '\n';
    out << "  def call(";
    for (int i = 0; i < arity; ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "arg" << i;
    }
    out << ")" << '\n';
    out << "  end" << '\n';
    out << '\n';
    out << "  alias_method :[], :call" << '\n';
    out << '\n';
    out << "end" << '\n' << '\n';
}

int main(int argc, char **argv) {
    ofstream rb(argv[1], ios::trunc);
    for (int arity = 0; arity <= MAX_PROC_ARITY; ++arity) {
        emitProc(rb, arity);
    }
}
