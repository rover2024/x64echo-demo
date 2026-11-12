// SPDX-License-Identifier: MIT
#ifndef X64ECHO_EXAMPLE_HELLO_H
#define X64ECHO_EXAMPLE_HELLO_H

class Calculator {
public:
    virtual ~Calculator();
    virtual int eval(int value) = 0;
};

class Sink {
public:
    virtual ~Sink();
    virtual int put(int value) = 0;
};

extern "C" Calculator *hello_new_calculator(int scale);
extern "C" int hello_evaluate(Calculator *calculator, int value);
extern "C" void hello_delete_calculator(Calculator *calculator);

extern "C" int hello_apply(int (*callback)(int), int value);
extern "C" int hello_drive(Sink *sink, int count);
extern "C" void hello_require_positive(int value);

#endif
