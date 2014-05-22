#include <Halide.h>
#include <stdio.h>

using namespace Halide;

int main(int argc, char **argv) {
    Func f, g;
    Var x;
    Param<void *> handle;

    f(x) = reinterpret<uint64_t>(handle);
    g(x) = reinterpret<uint64_t>(handle);

    int foo;
    handle.set(&foo);

    Image<uint64_t> out1 = f.realize(4);

    g.vectorize(x, 4);
    Image<uint64_t> out2 = g.realize(4);

    uint64_t correct = (uint64_t)(&foo);

    for (int x = 0; x < out1.width(); x++) {
        if (out1(x) != correct) {
            printf("out1(%d) = %llu instead of %llu\n",
                   x,
                   (long long unsigned)out1(x),
                   (long long unsigned)correct);
            return -1;
        }
        if (out2(x) != correct) {
            printf("out2(%d) = %llu instead of %llu\n",
                   x,
                   (long long unsigned)out2(x),
                   (long long unsigned)correct);
            return -1;
        }
    }

    printf("Success!\n");
    return 0;
}