// SPDX-License-Identifier: MIT
#include "hello.h"

#include <cstdio>
#include <stdexcept>

namespace {

int twice_plus_one(int value)
{
    return 2 * value + 1;
}

class Doubler final : public Sink {
public:
    int sum = 0;

    int put(int value) override
    {
        sum += value;
        return 2 * value;
    }
};

} // namespace

int main()
{
    Calculator *calculator = hello_new_calculator(3);
    std::printf("virtual call: %d\n", hello_evaluate(calculator, 7));
    hello_delete_calculator(calculator);

    Doubler doubler;
    int driven = hello_drive(&doubler, 4);
    std::printf("guest virtual callback: %d, guest sum: %d\n",
                driven, doubler.sum);

    std::printf("guest function callback: %d\n",
                hello_apply(twice_plus_one, 5));

    try {
        hello_require_positive(-1);
        std::printf("exception: missing\n");
    } catch (const std::invalid_argument &error) {
        std::printf("cross-boundary exception: %s\n", error.what());
    }

    return 0;
}
