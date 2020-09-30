// EVMC: Ethereum Client-VM Connector API.
// Copyright 2016-2020 The EVMC Authors.
// Licensed under the Apache License, Version 2.0.

/// @file
/// Example implementation of the EVMC VM interface.
///
/// This VM implements a subset of EMV instructions in simplistic, incorrect and unsafe way:
/// - memory bounds are not checked,
/// - stack bounds are not checked,
/// - most of the operations are done with 8-bit precision.
/// Yet, it is capable of coping with some example EVM bytecode inputs, what is very useful
/// in integration testing. The implementation is done in simple C++ for readability and uses
/// pure C API and some C helpers.

#include "example_vm.h"
#include <evmc/evmc.h>
#include <evmc/helpers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace example
{
/// The example VM instance struct extending the evmc_vm.
struct ExampleVM : evmc_vm
{
    int verbose = 0;  ///< The verbosity level.
    ExampleVM();      ///< Constructor to initialize the evmc_vm struct.
};

/// The implementation of the evmc_vm::destroy() method.
static void destroy(evmc_vm* instance)
{
    delete static_cast<ExampleVM*>(instance);
}

/// The example implementation of the evmc_vm::get_capabilities() method.
static evmc_capabilities_flagset get_capabilities(evmc_vm* /*instance*/)
{
    return EVMC_CAPABILITY_EVM1;
}

/// Example VM options.
///
/// The implementation of the evmc_vm::set_option() method.
/// VMs are allowed to omit this method implementation.
static enum evmc_set_option_result set_option(evmc_vm* instance,
                                              const char* name,
                                              const char* value)
{
    ExampleVM* vm = static_cast<ExampleVM*>(instance);
    if (std::strcmp(name, "verbose") == 0)
    {
        if (value == nullptr)
            return EVMC_SET_OPTION_INVALID_VALUE;

        char* end = nullptr;
        long int v = std::strtol(value, &end, 0);
        if (end == value)  // Parsing the value failed.
            return EVMC_SET_OPTION_INVALID_VALUE;
        if (v > 9 || v < -1)  // Not in the valid range.
            return EVMC_SET_OPTION_INVALID_VALUE;
        vm->verbose = (int)v;
        return EVMC_SET_OPTION_SUCCESS;
    }

    return EVMC_SET_OPTION_INVALID_NAME;
}

/// The Example VM stack representation.
struct Stack
{
    evmc_uint256be items[1024];       ///< The array of stack items;
    evmc_uint256be* pointer = items;  ///< The pointer to the currently first empty stack slot.

    /// Pops an item from the top of the stack.
    evmc_uint256be pop() { return *--pointer; }

    /// Pushes an item to the top of the stack.
    void push(evmc_uint256be value) { *pointer++ = value; }
};

/// The Example VM memory representation.
struct Memory
{
    size_t size = 0;          ///< The current size of the memory.
    uint8_t data[1024] = {};  ///< The fixed-size memory buffer.

    /// Stores the given value bytes in the memory at given index.
    /// The Memory::size is updated accordingly.
    /// The memory buffer bounds are not checked.
    void set(size_t index, const uint8_t* value_data, size_t value_size)
    {
        std::memcpy(&data[index], value_data, value_size);
        size_t new_size = index + value_size;
        if (new_size > size)
            size = new_size;
    }
};

/// The example implementation of the evmc_vm::execute() method.
static evmc_result execute(evmc_vm* instance,
                           const evmc_host_interface* host,
                           evmc_host_context* context,
                           enum evmc_revision rev,
                           const evmc_message* msg,
                           const uint8_t* code,
                           size_t code_size)
{
    ExampleVM* vm = static_cast<ExampleVM*>(instance);

    if (vm->verbose > 0)
        std::puts("execution started\n");

    int64_t gas_left = msg->gas;
    Stack stack;
    Memory memory;

    for (size_t pc = 0; pc < code_size; pc++)
    {
        // Check remaining gas, assume each instruction costs 1.
        gas_left -= 1;
        if (gas_left < 0)
            return evmc_make_result(EVMC_OUT_OF_GAS, 0, nullptr, 0);

        switch (code[pc])
        {
        default:
            return evmc_make_result(EVMC_UNDEFINED_INSTRUCTION, 0, nullptr, 0);

        case 0x00:  // STOP
            return evmc_make_result(EVMC_SUCCESS, gas_left, nullptr, 0);

        case 0x01:  // ADD
        {
            uint8_t a = stack.pop().bytes[31];
            uint8_t b = stack.pop().bytes[31];
            uint8_t sum = static_cast<uint8_t>(a + b);
            evmc_uint256be value = {};
            value.bytes[31] = sum;
            stack.push(value);
            break;
        }

        case 0x30:  // ADDRESS
        {
            evmc_address address = msg->destination;
            evmc_uint256be value = {};
            std::memcpy(&value.bytes[12], address.bytes, sizeof(address.bytes));
            stack.push(value);
            break;
        }

        case 0x35:  // CALLDATALOAD
        {
            size_t offset = stack.pop().bytes[31];
            evmc_uint256be value = {};

            size_t copy_size = 0;
            if (offset < msg->input_size)
                copy_size = msg->input_size - offset;  // Partial copy.
            if (copy_size > sizeof(value))
                copy_size = sizeof(value);  // Full copy.
            std::memcpy(value.bytes, &msg->input_data[offset], copy_size);

            stack.push(value);
            break;
        }

        case 0x43:  // NUMBER
        {
            evmc_uint256be value = {};
            value.bytes[31] = static_cast<uint8_t>(host->get_tx_context(context).block_number);
            stack.push(value);
            break;
        }

        case 0x52:  // MSTORE
        {
            uint8_t index = stack.pop().bytes[31];
            evmc_uint256be value = stack.pop();
            memory.set(index, value.bytes, sizeof(value));
            break;
        }

        case 0x54:  // SLOAD
        {
            evmc_uint256be index = stack.pop();
            evmc_uint256be value = host->get_storage(context, &msg->destination, &index);
            stack.push(value);
            break;
        }

        case 0x55:  // SSTORE
        {
            evmc_uint256be index = stack.pop();
            evmc_uint256be value = stack.pop();
            host->set_storage(context, &msg->destination, &index, &value);
            break;
        }

        case 0x59:  // MSIZE
        {
            uint8_t size_as_byte = static_cast<uint8_t>(memory.size);
            evmc_uint256be value = {};
            value.bytes[31] = size_as_byte;
            stack.push(value);
            break;
        }

        case 0x60:  // PUSH1
        {
            uint8_t byte = code[pc + 1];
            pc++;
            evmc_uint256be value = {};
            value.bytes[31] = byte;
            stack.push(value);
            break;
        }

        case 0x80:  // DUP1
        {
            evmc_uint256be value = stack.pop();
            stack.push(value);
            stack.push(value);
            break;
        }

        case 0xf1:  // CALL
        {
            evmc_message call_msg = {};
            call_msg.gas = stack.pop().bytes[31];
            evmc_uint256be a = stack.pop();
            std::memcpy(call_msg.destination.bytes, &a.bytes[12], sizeof(call_msg.destination));
            call_msg.value = stack.pop();
            uint8_t call_input_offset = stack.pop().bytes[31];
            call_msg.input_data = &memory.data[call_input_offset];
            call_msg.input_size = stack.pop().bytes[31];
            uint8_t call_output_offset = stack.pop().bytes[31];
            size_t call_output_size = stack.pop().bytes[31];

            evmc_result call_result = host->call(context, &call_msg);

            evmc_uint256be value = {};
            value.bytes[31] = (call_result.status_code == EVMC_SUCCESS);
            stack.push(value);

            if (call_output_size > call_result.output_size)
                call_output_size = call_result.output_size;
            memory.set(call_output_offset, call_result.output_data, call_output_size);

            if (call_result.release != nullptr)
                call_result.release(&call_result);
            break;
        }

        case 0xf3:  // RETURN
        {
            uint8_t index = stack.pop().bytes[31];
            uint8_t size = stack.pop().bytes[31];
            return evmc_make_result(EVMC_SUCCESS, gas_left, &memory.data[index], size);
        }

        case 0xfd:  // REVERT
        {
            if (rev < EVMC_BYZANTIUM)
                return evmc_make_result(EVMC_UNDEFINED_INSTRUCTION, 0, nullptr, 0);

            uint8_t index = stack.pop().bytes[31];
            uint8_t size = stack.pop().bytes[31];
            return evmc_make_result(EVMC_REVERT, gas_left, &memory.data[index], size);
        }
        }
    }

    return evmc_make_result(EVMC_SUCCESS, gas_left, nullptr, 0);
}


/// @cond internal
#if !defined(PROJECT_VERSION)
/// The dummy project version if not provided by the build system.
#define PROJECT_VERSION "0.0.0"
#endif
/// @endcond

ExampleVM::ExampleVM()
  : evmc_vm{EVMC_ABI_VERSION,   "example_vm",     PROJECT_VERSION,
            example::destroy,   example::execute, example::get_capabilities,
            example::set_option}
{}

}  // namespace example

extern "C" evmc_vm* evmc_create_example_vm()
{
    return new example::ExampleVM;
}
