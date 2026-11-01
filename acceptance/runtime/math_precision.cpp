#include "../helpers.hpp"
#include "runtime/foundation.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
using namespace atelier;

using namespace atelier::acceptance::helpers;

struct PrecisionOwner {
    double value{};

    float guard{3.0f};

    Array<double> samples;

    math::Vector3d position;

    math::Matrix4d matrix;

    double echo(double input) const noexcept { return input; }

    double accumulate(double& target, const double& amount) {
        target += amount;
        return target;
    }

    double sum(double a, double b, double c, double d, double e, double f, double g, double h,
               double i) const noexcept {
        return a + b + c + d + e + f + g + h + i;
    }

    double sample(Array<double>& values, const double& input) {
        values.add(input);
        return values[values.size() - 1];
    }

    math::Vector3d displace(math::Vector3d position, const math::Vector3d& delta, double scale) const noexcept {
        return position + delta * scale;
    }

    math::Matrix4d compose(math::Matrix4d first, const math::Matrix4d& second) const noexcept { return first * second; }

    math::Vector3d packed(math::Vector3 a, math::Vector3d b, math::Matrix4d matrix, math::Vector3 c, math::Vector3d d,
                          double weight) const noexcept {
        return {a.x + b.x + matrix.elements[15], c.y + d.y, weight};
    }
};
}

void verify_runtime_math_precision() {
    Runtime runtime;
    const auto f64 = take(runtime.native_type<double>());
    const auto f32 = take(runtime.native_type<float>());
    const auto vector3d = take(runtime.native_type<math::Vector3d>());
    const auto matrix4d = take(runtime.native_type<math::Matrix4d>());
    require(take(runtime.describe(vector3d))->math == math::type_of<math::Vector3d> &&
                take(runtime.describe(matrix4d))->math == math::type_of<math::Matrix4d>,
            "Canonical mathematical records have no authoritative execution description.");
    const auto* double_info = take(runtime.describe(f64));
    require(double_info->name == "core.f64" && double_info->size == sizeof(double) &&
                double_info->alignment == alignof(double) && double_info->kind == TypeKind::scalar,
            "Double has no canonical native scalar identity or layout.");

    auto owner = NativeRegistration<PrecisionOwner>{runtime, "precision.Owner", TypeKind::object}
                     .field<&PrecisionOwner::value>("value")
                     .field<&PrecisionOwner::guard>("guard")
                     .field<&PrecisionOwner::samples>("samples")
                     .method<&PrecisionOwner::echo>("echo")
                     .field<&PrecisionOwner::position>("position")
                     .field<&PrecisionOwner::matrix>("matrix")
                     .method<&PrecisionOwner::accumulate>("accumulate")
                     .method<&PrecisionOwner::sum>("sum")
                     .method<&PrecisionOwner::sample>("sample")
                     .method<&PrecisionOwner::displace>("displace")
                     .method<&PrecisionOwner::compose>("compose")
                     .method<&PrecisionOwner::packed>("packed")
                     .build();
    SignatureDefinition wrong_result;
    wrong_result.result = ValueDefinition{f64};
    owner.methods.push_back({"wrong_result", std::move(wrong_result), [](CallContext& call) -> Result<void> {
                                 call.result = 1.0f;
                                 return {};
                             }});
    SignatureDefinition vector_result;
    vector_result.result = ValueDefinition{vector3d};
    owner.methods.push_back({"failed_math", vector_result, [](CallContext& call) -> Result<void> {
                                 *static_cast<math::Vector3d*>(call.result_value.data) = {101.0, 102.0, 103.0};
                                 call.result_written = true;
                                 return std::unexpected(Error{ErrorCode::invocation_failed,
                                                              "Deliberate failure after staging a math result."});
                             }});
    owner.methods.push_back({"missing_math", vector_result, [](CallContext&) -> Result<void> { return {}; }});
    const auto published = take(runtime.register_types(
        {array_definition<double>(runtime, "precision.DoubleArray"),
         std::move(owner),
         {"precision.Triple", TypeKind::record, {{"x", {f64}}, {"y", {f64}}, {"z", {f64}}}, {}, {}},
         {"precision.Described", TypeKind::object, {{"triple", {TypeReference{"precision.Triple"}}}}, {}, {}}}));
    const auto owner_type = published[1];
    const auto triple_type = published[2];
    const auto described_type = published[3];
    SignatureDefinition plain_signature;
    plain_signature.parameters = {{triple_type}};
    const auto* triple_info = take(runtime.describe(triple_type));
    require(!triple_info->math && triple_info->plain_value && runtime.resolve_signature(plain_signature).has_value(),
            "A plain record lost value passing or acquired builtin math intrinsic semantics.");
    SignatureDefinition async_math;
    async_math.parameters = {{vector3d}};
    async_math.execution = FunctionExecution::coroutine;
    require(!runtime.resolve_signature(async_math), "Unsupported coroutine math boundary was silently admitted.");
    auto object = take(runtime.create(owner_type));
    const auto value_field = take(runtime.find_field(owner_type, "value"));
    const auto guard_field = take(runtime.find_field(owner_type, "guard"));
    const auto echo = take(runtime.find_method(owner_type, "echo"));
    Scalar output;
    for (const std::uint64_t bits : {0x3ff0000000000001ull, 0x8000000000000000ull, 0x7ff8123456789abcull}) {
        const double value = std::bit_cast<double>(bits);
        take(runtime.write(object.id(), value_field, Scalar{value}));
        require(std::bit_cast<std::uint64_t>(std::get<double>(take(runtime.read(object.id(), value_field)))) == bits,
                "Double reflection lost precision, signed zero or NaN payload.");
        const std::array arguments{Scalar{value}};
        take(runtime.call(object.id(), echo, arguments, &output));
        require(std::bit_cast<std::uint64_t>(std::get<double>(output)) == bits,
                "Native double argument/result did not preserve all 64 bits.");
    }
    require(std::get<float>(take(runtime.read(object.id(), guard_field))) == 3.0f,
            "Double field write overwrote the adjacent float.");
    require(!runtime.write(object.id(), value_field, Scalar{1.0f}) &&
                !runtime.write(object.id(), value_field, Scalar{EnumValue{f64, 1}}),
            "Double field accepted a mismatched or fabricated scalar identity.");

    const double amount = 0x1p-40;
    take(runtime.write(object.id(), value_field, Scalar{1.0}));
    const std::array<FieldId, 1> path{value_field};
    const std::array borrowed{Argument::field(object.id(), path), Argument::borrow(amount)};
    take(runtime.call(object.id(), take(runtime.find_method(owner_type, "accumulate")), borrowed, &output));
    require(std::get<double>(output) == 1.0 + amount &&
                std::get<double>(take(runtime.read(object.id(), value_field))) == 1.0 + amount,
            "Writable/read-only double references changed precision or failed to update native storage.");
    const std::array many{Scalar{1.0}, Scalar{2.0}, Scalar{3.0}, Scalar{4.0},   Scalar{5.0},
                          Scalar{6.0}, Scalar{7.0}, Scalar{8.0}, Scalar{amount}};
    take(runtime.call(object.id(), take(runtime.find_method(owner_type, "sum")), many, &output));
    require(std::get<double>(output) == 36.0 + amount,
            "Expanded double argument storage truncated or misaligned a value.");

    Array<double> samples;
    const std::array sample_args{Argument::borrow(samples), Argument::borrow(amount)};
    take(runtime.call(object.id(), take(runtime.find_method(owner_type, "sample")), sample_args, &output));
    require(samples.size() == 1 && samples[0] == amount && std::get<double>(output) == amount,
            "Array<double> did not share scalar precision and native reference semantics.");
    output = 42.0;
    require(!runtime.call(object.id(), take(runtime.find_method(owner_type, "wrong_result")), {}, &output) &&
                std::get<double>(output) == 42.0,
            "Rejected native result overwrote the caller's double output.");

    SignatureDefinition signature;
    signature.parameters = {{f32},
                            {f64},
                            {f64, ValueForm::inline_value, ParameterPassing::read_only},
                            {f64, ValueForm::inline_value, ParameterPassing::reference}};
    signature.result = ValueDefinition{f64};
    const auto resolved = take(runtime.resolve_signature(signature));
    double mutable_value = 1.0 + amount;
    const std::array mixed{Argument{Scalar{1.0f}}, Argument::borrow(mutable_value), Argument::borrow(amount),
                           Argument::borrow(mutable_value)};
    PreparedArguments prepared;
    take(runtime.prepare_arguments(resolved, mixed, prepared));
    mutable_value = 2.0;
    const auto views = prepared.values();
    require(views[1].get<double>() == 1.0 + amount && views[2].data == &amount && views[3].writable == &mutable_value &&
                views[3].get<double&>() == 2.0,
            "Double value snapshots and borrowed arguments do not retain their distinct semantics.");

    math::Vector3d position{1.0 + amount, 2.0, 3.0};
    const math::Vector3d delta{amount, 4.0, 5.0};
    math::Vector3d displaced{91.0, 92.0, 93.0};
    const auto displace = take(runtime.find_method(owner_type, "displace"));
    const std::array math_arguments{Argument::borrow(position), Argument::borrow(delta), Argument{Scalar{2.0}}};
    take(runtime.call(object.id(), displace, math_arguments, ValueOutput{vector3d, &displaced}));
    require(displaced == position + delta * 2.0 && position.x == 1.0 + amount,
            "Native mathematical values did not copy inputs and return complete double precision storage.");
    const auto preserved = displaced;
    require(!runtime.call(object.id(), take(runtime.find_method(owner_type, "failed_math")), {},
                          ValueOutput{vector3d, &displaced}) &&
                displaced == preserved,
            "Failed native math call published its staged result.");
    require(!runtime.call(object.id(), take(runtime.find_method(owner_type, "missing_math")), {},
                          ValueOutput{vector3d, &displaced}) &&
                displaced == preserved,
            "Missing native math result overwrote caller storage.");
    require(!runtime.call(object.id(), displace, math_arguments, ValueOutput{f64, &displaced}) &&
                !runtime.call(object.id(), displace, math_arguments, &output),
            "Mathematical result accepted a different type or scalar output storage.");
    alignas(double) std::array<std::byte, sizeof(math::Vector3d) + 1> misaligned{};
    require(!runtime.call(object.id(), displace, math_arguments, ValueOutput{vector3d, misaligned.data() + 1}),
            "Mathematical result accepted misaligned output storage.");

    auto first = math::Matrix4d::identity();
    const auto second = math::Matrix4d{{1.0 + amount, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 4, 5, 6, 1}};
    math::Matrix4d composed;
    const std::array matrix_arguments{Argument::borrow(first), Argument::borrow(second)};
    take(runtime.call(object.id(), take(runtime.find_method(owner_type, "compose")), matrix_arguments,
                      ValueOutput{matrix4d, &composed}));
    require(composed == second, "Mathematical argument/result storage truncated a double matrix.");
    const auto position_field = take(runtime.find_field(owner_type, "position"));
    const auto matrix_field = take(runtime.find_field(owner_type, "matrix"));
    take(runtime.write(object.id(), position_field, ValueInput{vector3d, &position}));
    take(runtime.write(object.id(), matrix_field, ValueInput{matrix4d, &second}));
    math::Vector3d restored_position;
    math::Matrix4d restored_matrix;
    take(runtime.read(object.id(), position_field, ValueOutput{vector3d, &restored_position}));
    take(runtime.read(object.id(), matrix_field, ValueOutput{matrix4d, &restored_matrix}));
    require(restored_position == position && restored_matrix == second,
            "Whole mathematical field read/write lost components or precision.");
    require(!runtime.write(object.id(), position_field, ValueInput{f64, &amount}) &&
                !runtime.read(object.id(), position_field, ValueOutput{matrix4d, &restored_matrix}) &&
                restored_matrix == second,
            "Whole mathematical field access accepted a different Runtime type.");
    const std::array<FieldId, 1> position_path{position_field};
    const std::array field_arguments{Argument::field(object.id(), position_path), Argument::borrow(delta),
                                     Argument{Scalar{2.0}}};
    take(runtime.call(object.id(), displace, field_arguments, ValueOutput{vector3d, &displaced}));
    require(displaced == position + delta * 2.0, "Whole mathematical field did not enter the native by-value path.");
    SignatureDefinition snapshot;
    snapshot.parameters = {{matrix4d}, {vector3d, ValueForm::inline_value, ParameterPassing::read_only}};
    const auto snapshot_signature = take(runtime.resolve_signature(snapshot));
    const std::array snapshot_arguments{Argument::borrow(first), Argument::borrow(position)};
    take(runtime.prepare_arguments(snapshot_signature, snapshot_arguments, prepared));
    first.elements[15] = 7;
    require(prepared.values()[0].get<math::Matrix4d>().elements[15] == 1 && prepared.values()[1].data == &position,
            "Mathematical value arguments did not snapshot independently from borrowed references.");

    const auto vector3 = take(runtime.native_type<math::Vector3>());
    const math::Vector3 small_a{2, 3, 4};
    const math::Vector3 small_c{5, 6, 7};
    const math::Vector3d large_b{1.0 + amount, 2, 3};
    const math::Vector3d large_d{4, 5.0 + amount, 6};
    // Packed copies use 216 bytes: float vectors leave a four-byte alignment gap
    // before double vectors. This crosses the inline boundary without 128-byte
    // padding per vector, and exercises both native invocation and snapshots.
    const std::array packed_arguments{Argument::borrow(small_a), Argument::borrow(large_b), Argument::borrow(first),
                                      Argument::borrow(small_c), Argument::borrow(large_d), Argument{Scalar{amount}}};
    take(runtime.call(object.id(), take(runtime.find_method(owner_type, "packed")), packed_arguments,
                      ValueOutput{vector3d, &displaced}));
    require(displaced == math::Vector3d{small_a.x + large_b.x + first.elements[15], small_c.y + large_d.y, amount},
            "Expanded packed mathematical arguments lost value bits or alignment.");
    SignatureDefinition packed_signature;
    packed_signature.parameters = {{vector3}, {vector3d}, {matrix4d}, {vector3}, {vector3d}, {f64}};
    take(runtime.prepare_arguments(take(runtime.resolve_signature(packed_signature)), packed_arguments, prepared));
    first.elements[15] = 9;
    require(prepared.values()[2].get<math::Matrix4d>().elements[15] == 7 &&
                prepared.values()[1].get<math::Vector3d>() == large_b &&
                reinterpret_cast<std::uintptr_t>(prepared.values()[1].data) % alignof(double) == 0 &&
                reinterpret_cast<std::uintptr_t>(prepared.values()[4].data) % alignof(double) == 0,
            "Expanded mixed-precision scratch did not retain aligned independent snapshots.");

    auto described = take(runtime.create(described_type));
    const auto triple_field = take(runtime.find_field(described_type, "triple"));
    const auto z = take(runtime.find_field(triple_type, "z"));
    const std::array nested_path{triple_field, z};
    require(std::get<double>(take(runtime.read(described.id(), nested_path))) == 0.0,
            "Homogeneous described double record was not initialized correctly.");
    take(runtime.write(described.id(), nested_path, Scalar{1.0 + amount}));
    require(std::get<double>(take(runtime.read(described.id(), nested_path))) == 1.0 + amount,
            "Nested described double field did not preserve precision.");
    alignas(double) std::array<std::byte, 3 * sizeof(double)> source{};
    alignas(double) std::array<std::byte, 3 * sizeof(double)> destination{};
    take(runtime.initialize_value(triple_type, source));
    const std::array expected{1.0 + amount, 0x1p40, -0x1p-40};
    std::memcpy(source.data(), expected.data(), source.size());
    take(runtime.copy_construct_value(triple_type, destination, source));
    require(std::memcmp(destination.data(), expected.data(), destination.size()) == 0,
            "Homogeneous double record copy lost bytes.");
    take(runtime.destroy_value(triple_type, destination));
    take(runtime.destroy_value(triple_type, source));
    std::puts("[pass] Runtime double/math identity, native calls, borrows, staged outputs and described layouts");
}
