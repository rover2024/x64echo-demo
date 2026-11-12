// SPDX-License-Identifier: MIT
#include "hello.h"

#include <stdexcept>

Calculator::~Calculator() = default;
Sink::~Sink() = default;

namespace {

class ScalingCalculator final : public Calculator {
public:
    explicit ScalingCalculator(int factor) : factor_(factor) {}

    int eval(int value) override
    {
        return value * factor_;
    }

private:
    int factor_;
};

} // namespace

extern "C" Calculator *hello_new_calculator(int scale)
{
    return new ScalingCalculator(scale);
}

extern "C" int hello_evaluate(Calculator *calculator, int value)
{
    return calculator->eval(value);
}

extern "C" void hello_delete_calculator(Calculator *calculator)
{
    delete calculator;
}

extern "C" int hello_apply(int (*callback)(int), int value)
{
    return callback(value);
}

extern "C" int hello_drive(Sink *sink, int count)
{
    int total = 0;
    for (int i = 0; i < count; ++i)
        total += sink->put(i);
    return total;
}

extern "C" void hello_require_positive(int value)
{
    if (value <= 0)
        throw std::invalid_argument("value must be positive");
}
