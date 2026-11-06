#include "../helpers.hpp"
#include "runtime/native.hpp"
#include "source/program.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

struct alignas(64) Packet {
    math::Matrix4d first;

    std::uint32_t count;
};

struct alignas(64) ScratchValue {
    std::uint32_t count;

    bool enabled;
};

struct CopyValue {
    inline static std::uint32_t constructions{};

    inline static bool reject_construction{};

    std::uint32_t count;

    CopyValue() : count(0) { ++constructions; }
};

struct Host {
    Result<Packet> alias(Packet snapshot, Packet& value) const noexcept {
        value.count += 10;
        return snapshot;
    }

    Result<Packet> fail(Packet) const noexcept {
        return std::unexpected(Error{ErrorCode::invocation_failed, "Native plain record failure."});
    }
};

void publish(Program& program, std::string_view text) {
    auto candidate = take(Program::prepare(take(program.request("plain-records.source", text, "run"))));
    take(program.try_publish(candidate));
}
}

void verify_source_plain_records() {
    {
        Runtime owner;
        auto definition =
            NativeRegistration<CopyValue>{owner, "plain.CopyValue", TypeKind::record}
                .field<&CopyValue::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(CopyValue, count))
                .build();
        definition.native.value_operations_preserve_borrows = true;
        definition.native.construct = [](void* destination) noexcept -> Result<void> {
            auto* value = std::construct_at(static_cast<CopyValue*>(destination));
            if (CopyValue::reject_construction) {
                std::destroy_at(value);
                return std::unexpected(
                    Error{ErrorCode::construction_failed, "CopyValue default construction rejected."});
            }
            return {};
        };
        const auto type = take(owner.register_type(std::move(definition)));
        auto local = take(Program::create(owner));
        publish(*local, "plain.CopyValue run(plain.CopyValue input) { plain.CopyValue value; return input; }");
        CopyValue input;
        CopyValue output;
        input.count = 37;
        output.count = 91;
        CopyValue::reject_construction = true;
        const auto failed =
            local->entry().invoke({}, std::array{Argument::borrow(std::as_const(input))}, ValueOutput{type, &output});
        CopyValue::reject_construction = false;
        require(!failed && output.count == 91 && CopyValue::constructions == 3,
                "Local construction failure changed caller output.");
        take(local->entry().invoke({}, std::array{Argument::borrow(std::as_const(input))}, ValueOutput{type, &output}));
        require(output.count == 37 && CopyValue::constructions == 4,
                "A failed local construction left value invocation state behind.");
    }
    {
        PreparedArguments scratch;
        {
            Runtime owner;
            const auto type = take(
                owner.register_type(NativeRegistration<ScratchValue>{owner, "plain.ScratchValue", TypeKind::record}
                                        .field<&ScratchValue::count>("count", {TypeReference::native<std::uint32_t>()},
                                                                     offsetof(ScratchValue, count))
                                        .field<&ScratchValue::enabled>("enabled", {TypeReference::native<bool>()},
                                                                       offsetof(ScratchValue, enabled))
                                        .build()));
            ScratchValue value{37, true};
            FunctionSignature signature;
            signature.parameters.emplace_back(type);
            const auto argument = Argument::borrow(std::as_const(value));
            take(owner.prepare_arguments(signature, std::array{argument}, scratch));
            value.count = 99;
            require(scratch.values()[0].get<ScratchValue>().count == 37,
                    "Prepared value snapshot retained its input address.");
            auto multiple = signature;
            multiple.parameters.resize(3, signature.parameters[0]);
            take(owner.prepare_arguments(multiple, std::array{argument, argument, argument}, scratch));
            require(scratch.values().size() == 3 && scratch.values()[2].get<ScratchValue>().count == 99 &&
                        reinterpret_cast<std::uintptr_t>(scratch.values()[2].data) % alignof(ScratchValue) == 0,
                    "Expanded argument scratch lost record alignment or copied contents.");
            require(!owner.prepare_arguments(signature, std::array{Argument{1.0f}}, scratch) &&
                        scratch.values().empty(),
                    "Failed argument preparation exposed previous validated views.");
            value.count = 37;
            take(owner.prepare_arguments(signature, std::array{argument}, scratch));
        }
        require(scratch.values()[0].get<ScratchValue>().count == 37 &&
                    scratch.values()[0].get<ScratchValue>().enabled &&
                    reinterpret_cast<std::uintptr_t>(scratch.values()[0].data) % alignof(ScratchValue) == 0,
                "Prepared value scratch lost independent aligned storage after Runtime destruction.");
    }
    static_assert(sizeof(Packet) > 128);
    Runtime runtime;
    auto packet =
        NativeRegistration<Packet>{runtime, "plain.Packet", TypeKind::record}
            .field<&Packet::first>("first", {TypeReference::native<math::Matrix4d>()}, offsetof(Packet, first))
            .field<&Packet::count>("count", {TypeReference::native<std::uint32_t>()}, offsetof(Packet, count))
            .build();
    packet.native.value_operations_preserve_borrows = true;
    take(runtime.register_types({std::move(packet), NativeRegistration<Host>{runtime, "plain.Host", TypeKind::object}
                                                        .method<&Host::alias>("alias", true)
                                                        .method<&Host::fail>("fail", true)
                                                        .build()}));
    const auto packet_type = take(runtime.native_type<Packet>());
    const auto host_type = take(runtime.native_type<Host>());
    const auto host = take(runtime.create(host_type));
    auto alias = take(Program::create(runtime, host_type));
    publish(*alias, R"(
        plain.Packet snapshot(plain.Packet copy, plain.Packet& target) {
            target.count = target.count + 10;
            return copy;
        }
        plain.Packet run(plain.Packet& value) {
            plain.Packet result = snapshot(value, value);
            return self.alias(result, value);
        }
    )");
    Packet input{};
    input.count = 4;
    Packet output{};
    take(alias->entry().invoke(host.id(), std::array{Argument::borrow(input)}, ValueOutput{packet_type, &output}));
    require(output.count == 4 && input.count == 24,
            "Left-to-right record snapshot changed when a later writable argument aliases it.");
    auto failure = take(Program::create(runtime, host_type));
    publish(*failure, "plain.Packet run(plain.Packet value) { return self.fail(value); }");
    Packet preserved;
    std::memset(&preserved, 0x5a, sizeof(preserved));
    std::array<std::byte, sizeof(Packet)> before{};
    std::memcpy(before.data(), &preserved, sizeof(preserved));
    const auto failed = failure->entry().invoke(host.id(), std::array{Argument::borrow(std::as_const(input))},
                                                ValueOutput{packet_type, &preserved});
    require(!failed && std::memcmp(before.data(), &preserved, sizeof(preserved)) == 0,
            "Failed record call overwrote output larger than the inline result buffer.");
    std::puts("Source plain records: PASS (construction failure, snapshots, aligned scratch, large failed output)");
}
