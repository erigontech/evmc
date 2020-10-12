// EVMC: Ethereum Client-VM Connector API.
// Copyright 2020 The EVMC Authors.
// Licensed under the Apache License, Version 2.0.

#include "../../examples/example_vm/example_vm.h"
#include <evmc/evmc.hpp>
#include <evmc/mocked_host.hpp>
#include <tools/utils/utils.hpp>
#include <gtest/gtest.h>
#include <cstring>

using namespace evmc::literals;

namespace
{
struct Output
{
    evmc::bytes bytes;

    explicit Output(const char* output_hex) noexcept : bytes{evmc::from_hex(output_hex)} {}

    friend bool operator==(const evmc::result& result, const Output& expected) noexcept
    {
        return expected.bytes.compare(0, evmc::bytes::npos, result.output_data,
                                      result.output_size) == 0;
    }
};

auto vm = evmc::VM{evmc_create_example_vm()};

class example_vm : public testing::Test
{
protected:
    evmc_revision rev = EVMC_MAX_REVISION;
    evmc::MockedHost host;
    evmc_message msg{};

    example_vm() noexcept
    {
        msg.sender = 0x5000000000000000000000000000000000000005_address;
        msg.destination = 0xd00000000000000000000000000000000000000d_address;
    }

    evmc::result execute_in_example_vm(int64_t gas,
                                       const char* code_hex,
                                       const char* input_hex = "") noexcept
    {
        const auto code = evmc::from_hex(code_hex);
        const auto input = evmc::from_hex(input_hex);

        msg.gas = gas;
        msg.input_data = input.data();
        msg.input_size = input.size();

        return vm.execute(host, rev, msg, code.data(), code.size());
    }
};


}  // namespace

TEST_F(example_vm, empty_code)
{
    const auto r = execute_in_example_vm(999, "");
    EXPECT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.gas_left, 999);
    EXPECT_EQ(r.output_size, 0);
}

TEST_F(example_vm, return_address)
{
    // Assembly: `{ mstore(0, address()) return(12, 20) }`.
    const auto r = execute_in_example_vm(6, "306000526014600cf3");
    EXPECT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.gas_left, 0);
    EXPECT_EQ(r, Output("d00000000000000000000000000000000000000d"));
}

TEST_F(example_vm, counter_in_storage)
{
    // Assembly: `{ sstore(0, add(sload(0), 1)) }`
    auto& storage_value = host.accounts[msg.destination].storage[{}].value;
    storage_value = 0x00000000000000000000000000000000000000000000000000000000000000bb_bytes32;
    const auto r = execute_in_example_vm(10, "60016000540160005500");
    EXPECT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.gas_left, 3);
    EXPECT_EQ(r, Output(""));
    EXPECT_EQ(storage_value,
              0x00000000000000000000000000000000000000000000000000000000000000bc_bytes32);
}

TEST_F(example_vm, revert_block_number)
{
    // Assembly: `{ mstore(0, number()) revert(0, 32) }`
    host.tx_context.block_number = 0xb4;
    const auto r = execute_in_example_vm(7, "4360005260206000fd");
    EXPECT_EQ(r.status_code, EVMC_REVERT);
    EXPECT_EQ(r.gas_left, 1);
    EXPECT_EQ(r, Output("00000000000000000000000000000000000000000000000000000000000000b4"));
}

TEST_F(example_vm, revert_undefined)
{
    rev = EVMC_FRONTIER;
    const auto r = execute_in_example_vm(100, "fd");
    EXPECT_EQ(r.status_code, EVMC_UNDEFINED_INSTRUCTION);
    EXPECT_EQ(r.gas_left, 0);
    EXPECT_EQ(r, Output(""));
}

TEST_F(example_vm, call)
{
    const auto expected_output = evmc::from_hex("aabbcc");
    host.call_result.output_data = expected_output.data();
    host.call_result.output_size = expected_output.size();
    const auto r = execute_in_example_vm(100, "6003808080808080f1596000f3");
    EXPECT_EQ(r.status_code, EVMC_SUCCESS);
    EXPECT_EQ(r.gas_left, 89);
    EXPECT_EQ(r, Output("000000aabbcc"));
    ASSERT_EQ(host.recorded_calls.size(), 1);
    EXPECT_EQ(host.recorded_calls[0].flags, 0);
    EXPECT_EQ(host.recorded_calls[0].gas, 3);
    EXPECT_EQ(host.recorded_calls[0].value,
              0x0000000000000000000000000000000000000000000000000000000000000003_bytes32);
    EXPECT_EQ(host.recorded_calls[0].destination,
              0x0000000000000000000000000000000000000003_address);
    EXPECT_EQ(host.recorded_calls[0].input_size, 3);
}