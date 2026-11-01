#include "../helpers.hpp"
#include "source/program.hpp"

#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace atelier;

using source::Program;

using namespace atelier::acceptance::helpers;

void publish(Program& program, std::string_view text, std::string_view entry = {}) {
    auto candidate = take(Program::prepare(take(program.request("types.source", text, entry))));
    take(program.try_publish(candidate));
}

std::uint32_t number(const std::optional<Scalar>& value) {
    return std::get<std::uint32_t>(*value);
}

constexpr std::string_view schema = R"(

    struct combat.DamageInfo { combat.Point point; combat.Kind kind; uint amount; };
    enum combat.Kind : uint { Physical, Fire = 7, Ice, Maximum = 4294967295 }
    struct combat.Point { float x; int count; };
    enum combat.Signed { Minimum = -2147483648, Zero = 0, Maximum = 2147483647 }
)";
constexpr std::string_view body = R"(

    uint process(combat.DamageInfo& damage) {
        combat.DamageInfo snapshot = damage;
        combat.Point origin;
        origin.x = 2.0;
        combat.DamageInfo copy;
        copy = snapshot;
        copy.point = origin;
        bump(damage.amount, 4);
        damage.point.x = damage.point.x + 1.0;
        damage.kind = combat.Kind::Ice;
        if (copy.point.count != 0) { return 999; }
        if (snapshot.kind != combat.Kind::Fire) { return 998; }
        if (copy.point.x != 2.0) { return 997; }
        return snapshot.amount + copy.amount + damage.amount;
    }
    void bump(uint& value, uint amount) { value = value + amount; }
)";
const FieldInfo& field(const TypeInfo& type, std::string_view name) {
    for (const auto& value : type.fields)
        if (value.name == name)
            return value;
    throw std::runtime_error("Missing reflected source field.");
}

template <class T>
void write(std::span<std::byte> storage, std::size_t offset, T value) {
    std::memcpy(storage.data() + offset, &value, sizeof(value));
}

template <class T>
T read(std::span<const std::byte> storage, std::size_t offset) {
    T value;
    std::memcpy(&value, storage.data() + offset, sizeof(value));
    return value;
}
}

void verify_source_types() {
    Runtime runtime;
    const auto baseline = runtime.types().size();
    Program program(runtime, {});
    auto request = take(program.request("damage.source", std::string(schema) + std::string(body), "process"));
    require(runtime.types().size() == baseline && !runtime.find_type("combat.DamageInfo"),
            "Declaration collection published provisional types.");
    auto candidate = take(Program::prepare(request));
    request.reset();
    require(runtime.types().size() == baseline, "JIT preparation published source types.");
    take(runtime.begin_collection());
    require(!program.try_publish(candidate) && candidate && runtime.types().size() == baseline,
            "A busy type publication consumed its candidate or exposed types.");
    while (take(runtime.collect_step({1})).phase != CollectionPhase::idle) {
    }
    take(program.try_publish(candidate));
    const auto damage = take(runtime.find_type("combat.DamageInfo"));
    const auto point = take(runtime.find_type("combat.Point"));
    const auto kind = take(runtime.find_type("combat.Kind"));
    const auto* damage_info = take(runtime.describe(damage));
    const auto* point_info = take(runtime.describe(point));
    require(!damage_info->native_storage && damage_info->size == 16 && damage_info->alignment == 4 &&
                damage_info->copy_constructible && damage_info->value_operations_preserve_borrows &&
                field(*damage_info, "point").value.type == point && field(*damage_info, "kind").value.type == kind,
            "Source declarations did not use the shared Runtime layout and identities.");
    const auto* enum_info = take(runtime.describe(kind));
    require(enum_info->enumeration->values[2].second == 8 && enum_info->enumeration->values[3].second == 4294967295u,
            "Source enum explicit, implicit or boundary values changed.");
    const auto* signed_info = take(runtime.describe(take(runtime.find_type("combat.Signed"))));
    require(signed_info->enumeration->values.front().second == 0x80000000u &&
                signed_info->enumeration->values.back().second == 0x7fffffffu,
            "Signed enum boundaries were not preserved.");
    std::vector<std::byte> storage(damage_info->size);
    std::vector<std::byte> copied(damage_info->size);
    take(runtime.initialize_value(damage, storage));
    const auto amount_offset = *field(*damage_info, "amount").offset;
    const auto kind_offset = *field(*damage_info, "kind").offset;
    const auto x_offset = *field(*damage_info, "point").offset + *field(*point_info, "x").offset;
    const auto reset = [&] {
        write<std::uint32_t>(storage, amount_offset, 3);
        write<std::uint32_t>(storage, kind_offset, 7);
        write<float>(storage, x_offset, 5.0f);
    };
    reset();
    const std::array arguments{Argument::borrow(damage, storage.data(), storage.data())};
    require(number(take(program.entry().invoke({}, arguments))) == 13 &&
                read<std::uint32_t>(storage, amount_offset) == 7 && read<float>(storage, x_offset) == 6.0f &&
                read<std::uint32_t>(storage, kind_offset) == 8,
            "Source/C++ shared storage lost reference writes, deep value copies or enum identity.");
    take(runtime.copy_construct_value(damage, copied, storage));
    write<std::uint32_t>(storage, amount_offset, 20);
    require(read<std::uint32_t>(copied, amount_offset) == 7, "Runtime copy aliased a source record.");
    take(runtime.assign_value(damage, storage, copied));
    require(read<std::uint32_t>(storage, amount_offset) == 7, "Runtime assignment lost source record fields.");
    const std::array readonly{Argument::borrow(damage, storage.data())};
    require(!program.entry().invoke({}, readonly), "Writable source struct accepted a readonly argument.");
    const std::array wrong{Argument::borrow(point, storage.data(), storage.data())};
    require(!program.entry().invoke({}, wrong), "Source struct reference accepted a different Runtime identity.");

    Program independent(runtime, {});
    publish(independent,
            "enum auxiliary.Mode { Default } struct auxiliary.Data { uint value; }; uint run() { return 9; }");
    require(number(take(independent.entry().invoke({}, std::array<Scalar, 0>{}))) == 9,
            "Independent source declarations did not publish into the same Runtime.");

    // A second producer consumes the published source type through the same native call contract.
    TypeDefinition host;
    host.name = "combat.NativeConsumer";
    host.methods.push_back(
        {"amount",
         {{{TypeReference{"combat.DamageInfo"}, ValueForm::inline_value, ParameterPassing::read_only}},
          ValueDefinition{take(runtime.native_type<std::uint32_t>())}},
         +[](CallContext& call) -> Result<void> {
             const auto* info = take(call.runtime.describe(call.arguments[0].type));
             const auto offset = *field(*info, "amount").offset;
             call.result = Scalar{
                 read<std::uint32_t>({static_cast<const std::byte*>(call.arguments[0].data), info->size}, offset)};
             return {};
         },
         true});
    const auto host_type = take(runtime.register_type(std::move(host)));
    Program consumer(runtime, host_type);
    publish(consumer, "uint inspect(const combat.DamageInfo& value) { return self.amount(value); }");
    const auto object = take(runtime.create(host_type));
    require(number(take(consumer.entry().invoke(object.id(), readonly))) == 7,
            "Native consumer could not borrow the published source record.");
    Program enum_consumer(runtime, {});
    publish(enum_consumer, "combat.Kind identity(combat.Kind value) { return value; }");
    const std::array enum_args{Scalar{EnumValue{kind, 8}}};
    require(std::get<EnumValue>(*take(enum_consumer.entry().invoke({}, enum_args))) == EnumValue{kind, 8},
            "Source enum parameters/results lost shared Runtime identity.");

    const auto old = take(program.pin());
    publish(program, R"(
        uint process(combat.DamageInfo& damage) { return damage.amount + 100; }
        enum combat.Signed { Minimum = -2147483648, Zero = 0, Maximum = 2147483647 }
        struct combat.Point { float x; int count; };
        enum combat.Kind : uint { Physical, Fire = 7, Ice, Maximum = 4294967295 }
        struct combat.DamageInfo { combat.Point point; combat.Kind kind; uint amount; };
    )");
    require(take(runtime.describe(damage)) == damage_info && number(take(program.entry().invoke({}, arguments))) == 107,
            "Compatible code update replaced source type identity or storage.");
    reset();
    require(number(take(old.invoke({}, arguments))) == 13,
            "Old module lease lost its source type metadata or helper code.");
    // Objects exist; a code update still appends new declarations beside the frozen ones.
    constexpr std::string_view extra = "struct combat.Extra { combat.Point origin; uint value; };";
    reset();
    publish(
        program,
        std::string(schema) + std::string(extra) +
            "uint process(combat.DamageInfo& damage) { combat.Extra added; added.value = 5; return damage.amount + added.value; }");
    const auto* extra_info = take(runtime.describe(take(runtime.find_type("combat.Extra"))));
    require(take(runtime.describe(damage)) == damage_info && field(*extra_info, "origin").value.type == point &&
                number(take(program.entry().invoke({}, arguments))) == 8,
            "Compatible update did not append a source type beside frozen ones.");
    require(!Program::prepare(take(program.request(
                "removed.source", std::string(schema) + "uint process(combat.DamageInfo& damage) { return 0; }"))),
            "A published source type was removed during code-only publication.");
    auto changed_layout = std::string(schema) + std::string(extra);
    changed_layout.replace(changed_layout.find("uint amount;"), 12, "float amount;");
    require(!Program::prepare(take(program.request(
                "layout.source", changed_layout + "uint process(combat.DamageInfo& damage) { return 0; }"))),
            "Source layout changed during code-only publication.");
    program.close();
    take(runtime.destroy_value(damage, copied));
    take(runtime.destroy_value(damage, storage));
    require(take(runtime.find_type("combat.DamageInfo")) == damage, "Closing code unregistered a shared Runtime type.");

    Runtime rejected_runtime;
    Program rejected(rejected_runtime, {});
    const auto empty_count = rejected_runtime.types().size();
    constexpr std::array invalid{"struct A { B value; }; struct B { A value; }; uint run() { return 0; }",
                                 "struct A { uint value; uint value; }; uint run() { return 0; }",
                                 "struct A { uint value; }; struct A {}; uint run() { return 0; }",
                                 "struct A { Missing value; }; uint run() { return 0; }",
                                 "struct A { String text; }; uint run() { return 0; }",
                                 "struct A { uint value; }; uint run() { A local; return missing(); }",
                                 "struct run {}; uint run() { return 0; }",
                                 "struct A { uint value; }; uint run(const A& value) { value.value = 1; return 0; }",
                                 "struct A { uint value; }; async uint run() { A local; yield; return 0; }",
                                 "enum A : uint { Negative = -1 } uint run() { return 0; }",
                                 "enum A { TooBig = 2147483648 } uint run() { return 0; }",
                                 "enum A { Value, Value } uint run() { return 0; }",
                                 "enum A : uint { Last = 4294967295, Overflow } uint run() { return 0; }"};
    for (const auto* source : invalid) {
        const auto prepared = Program::prepare(take(rejected.request("invalid-types.source", source, "run")));
        require(!prepared && prepared.error().message.find("invalid-types.source") != std::string::npos &&
                    rejected_runtime.types().size() == empty_count && !rejected_runtime.find_type("A"),
                "Invalid declaration/body leaked types or lost its source diagnostic.");
    }
    auto stale = take(Program::prepare(
        take(rejected.request("old-types.source", "struct A { uint before; }; uint run() { return 1; }"))));
    publish(rejected, "struct A { float after; }; uint run() { return 2; }");
    require(!rejected.try_publish(stale) && number(take(rejected.entry().invoke({}, std::array<Scalar, 0>{}))) == 2 &&
                take(rejected_runtime.describe(take(rejected_runtime.find_type("A"))))->fields.front().name == "after",
            "Stale type candidate replaced the latest published definitions.");
    Program collision(rejected_runtime, {});
    require(!Program::prepare(take(collision.request("collision.source", "struct A {}; uint run() { return 0; }"))),
            "Another module silently reused a conflicting type declaration.");

    Runtime changed_runtime;
    Program pending(changed_runtime, {});
    auto pending_candidate =
        take(Program::prepare(take(pending.request("pending.source", "struct Pending {}; uint run() { return 0; }"))));
    TypeDefinition external;
    external.name = "External";
    external.kind = TypeKind::object;
    take(changed_runtime.register_type(std::move(external)));
    require(!pending.try_publish(pending_candidate) && !changed_runtime.find_type("Pending"),
            "A type candidate published after its captured identity allocation changed.");
    std::puts(
        "Source types: PASS (shared enum/struct layouts, staged publication, value lifetime, C++ borrows, failures, frozen schemas, old leases)");
}
