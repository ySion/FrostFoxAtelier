#include "language/material.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>

namespace atelier::language {
namespace {
[[noreturn]] void reject(SourcePosition position, std::string message) { throw Diagnostic{position, std::move(message)}; }
std::string number(std::uint32_t bits) { return "asfloat(" + std::to_string(bits) + "u)"; }
class Hlsl {
    const MaterialModule& material_;
    const Program& program_;
    std::vector<bool> emitted_;
    std::string text_;
    std::string type(ValueType value) const {
        switch (value.kind) {
        case ValueType::f32: return "float";
        case ValueType::boolean: return "bool";
        case ValueType::u32: return "uint";
        case ValueType::void_value: return "void";
        case ValueType::record: {
            if (value.type_index == material_.texture_input) return "uint";
            const auto& layout = program_.value_layouts[value.type_index];
            if (layout.math) return "float" + std::to_string(layout.math.components());
            if (value.type_index == material_.context) return "ms_Context";
            if (value.type_index == material_.output) return "ms_Output";
            if (value.type_index == material_.parameters) return "ms_Parameters";
            return "ms_Type" + std::to_string(value.type_index);
        }
        default: throw std::logic_error("Unvalidated material type.");
        }
    }
    std::string initial(ValueType value, const std::byte* bytes) const {
        if (value == ValueType{ValueType::record, material_.texture_input}) return "0u";
        if (value.kind == ValueType::boolean) return bytes && *bytes != std::byte{} ? "true" : "false";
        if (value.kind == ValueType::f32) {
            std::uint32_t bits{};
            if (bytes) std::memcpy(&bits, bytes, sizeof(bits));
            return number(bits);
        }
        const auto& layout = program_.value_layouts[value.type_index];
        if (!layout.math) return "ms_initial" + std::to_string(value.type_index) + "()";
        std::string result = type(value) + "(";
        for (std::size_t i = 0; i < layout.math.components(); ++i) {
            if (i) result += ",";
            result += initial(ValueType::f32, bytes ? bytes + i * sizeof(float) : nullptr);
        }
        return result + ")";
    }
    void initialize_fields(ValueType value, const std::string& path, const std::byte* bytes) {
        if (value.kind != ValueType::record || value.type_index == material_.texture_input || program_.value_layouts[value.type_index].math) {
            text_ += path + " = " + initial(value, bytes) + ";\n";
            return;
        }
        for (const auto& field : program_.value_layouts[value.type_index].fields)
            initialize_fields(field.type, path + ".ms_" + field.name, bytes ? bytes + field.offset : nullptr);
    }
    void declare_type(ValueType value) {
        if (value.kind != ValueType::record || emitted_[value.type_index]) return;
        emitted_[value.type_index] = true;
        const auto& layout = program_.value_layouts[value.type_index];
        if (layout.math || value.type_index == material_.texture_input) return;
        for (const auto& field : layout.fields) declare_type(field.type);
        text_ += "struct " + type(value) + " {\n";
        for (const auto& field : layout.fields) text_ += type(field.type) + " ms_" + field.name + ";\n";
        text_ += "};\n" + type(value) + " ms_initial" + std::to_string(value.type_index) + "() {\n";
        text_ += type(value) + " v = (" + type(value) + ")0;\n";
        initialize_fields(value, "v", layout.initial_value.data());
        text_ += "return v;\n}\n";
    }
    static std::string operation(Operation operation) {
        switch (operation) {
        case Operation::add: case Operation::positive: return "+";
        case Operation::subtract: case Operation::negate: return "-";
        case Operation::multiply: return "*";
        case Operation::divide: return "/";
        case Operation::equal: return "==";
        case Operation::not_equal: return "!=";
        case Operation::less: return "<";
        case Operation::less_equal: return "<=";
        case Operation::greater: return ">";
        case Operation::greater_equal: return ">=";
        case Operation::logical_not: return "!";
        case Operation::logical_and: return "&&";
        case Operation::logical_or: return "||";
        default: throw std::logic_error("Unvalidated material operation.");
        }
    }
    static std::string intrinsic(MathOperation operation) {
        switch (operation) {
        case MathOperation::dot: return "dot";
        case MathOperation::cross: return "cross";
        case MathOperation::length: return "length";
        case MathOperation::square_root: return "sqrt";
        case MathOperation::floor: return "floor";
        case MathOperation::ceil: return "ceil";
        case MathOperation::trunc: return "trunc";
        case MathOperation::fract: return "ms_fract";
        case MathOperation::abs: return "abs";
        case MathOperation::min: return "min";
        case MathOperation::max: return "max";
        case MathOperation::clamp: return "ms_clamp";
        case MathOperation::lerp: return "ms_lerp";
        case MathOperation::step: return "step";
        default: return {};
        }
    }
    std::string expression(const Function& function, NodeId id) const {
        const auto& node = function.expressions[id];
        const auto operand = [&](NodeId value) { return expression(function, value); };
        using Kind = Expression::Kind;
        switch (node.kind) {
        case Kind::literal:
            if (node.type == ValueType::f32) return number(static_cast<std::uint32_t>(node.bits));
            if (node.type == ValueType::boolean) return node.bits ? "true" : "false";
            return std::to_string(node.bits) + "u";
        case Kind::local: return "ms_v" + std::to_string(node.value);
        case Kind::value_field: {
            const auto owner = function.expressions[node.left].type;
            const auto& layout = program_.value_layouts[owner.type_index];
            const auto field = std::ranges::find_if(layout.fields, [&](const auto& value) { return value.offset == node.offset; });
            if (field == layout.fields.end()) throw std::logic_error("Missing typed material field.");
            return "(" + operand(node.left) + ")." + (layout.math ? "" : "ms_") + field->name;
        }
        case Kind::unary: return "(" + operation(node.operation) + operand(node.left) + ")";
        case Kind::binary: {
            auto result = "(" + operand(node.left) + operation(node.operation) + operand(node.right) + ")";
            const auto left = function.expressions[node.left].type;
            if (left.kind == ValueType::record && (node.operation == Operation::equal || node.operation == Operation::not_equal))
                result = (node.operation == Operation::equal ? "all" : "any") + result;
            return result;
        }
        case Kind::texture_sample: return "::atelier_material_sample(ms_environment," + operand(node.right) + "," + operand(node.left) + ")";
        case Kind::scalar_cast: return type(node.type) + "(" + operand(node.left) + ")";
        case Kind::math_construct: case Kind::math_intrinsic: case Kind::source_call: {
            std::string result = node.kind == Kind::source_call ? "ms_f" + std::to_string(node.value) :
                node.kind == Kind::math_construct ? type(node.type) : intrinsic(node.math_operation);
            result += "(";
            if (node.kind == Kind::source_call) result += "ms_environment";
            if (node.arguments.empty() && node.kind == Kind::math_construct) result += "0.0";
            for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                if (i || node.kind == Kind::source_call) result += ",";
                result += operand(node.arguments[i]);
            }
            return result + ")";
        }
        default: throw std::logic_error("Unvalidated material expression.");
        }
    }
    void statement(const Function& function, NodeId id) {
        const auto& node = function.statements[id];
        using Kind = Statement::Kind;
        switch (node.kind) {
        case Kind::block:
            text_ += "{\n";
            for (const auto child : node.children) statement(function, child);
            text_ += "}\n"; break;
        case Kind::declare: {
            const auto& local = function.locals[node.target];
            text_ += type(local.type) + " ms_v" + std::to_string(node.target) + " = ";
            text_ += node.expression == no_node ? initial(local.type, nullptr) : expression(function, node.expression);
            text_ += ";\n"; break;
        }
        case Kind::assign: text_ += expression(function, node.target) + " = " + expression(function, node.expression) + ";\n"; break;
        case Kind::call: text_ += expression(function, node.expression) + ";\n"; break;
        case Kind::return_value:
            text_ += "return" + (node.expression == no_node ? std::string{} : " " + expression(function, node.expression)) + ";\n"; break;
        case Kind::condition:
            text_ += "if (" + expression(function, node.expression) + ") {\n";
            statement(function, node.body); text_ += "}\n";
            if (node.alternative != no_node) { text_ += "else {\n"; statement(function, node.alternative); text_ += "}\n"; }
            break;
        default: throw std::logic_error("Unvalidated material statement.");
        }
    }
    std::string signature(std::size_t index) const {
        const auto& function = program_.functions[index];
        std::string result = type(function.result) + " ms_f" + std::to_string(index) + "(uint ms_environment";
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            result += ",";
            result += type(function.parameters[i].type) + " ms_v" + std::to_string(i);
        }
        return result + ")";
    }
    void load_parameters(ValueType value, const std::string& path, std::size_t offset) {
        if (value == ValueType::f32) {
            text_ += path + " = asfloat(data.Load(base + " + std::to_string(offset) + "u));\n";
            return;
        }
        const auto& layout = program_.value_layouts[value.type_index];
        if (layout.math) {
            text_ += path + " = asfloat(data.Load" + std::to_string(layout.math.components()) + "(base + " + std::to_string(offset) + "u));\n";
            return;
        }
        for (const auto& field : layout.fields) load_parameters(field.type, path + ".ms_" + field.name, offset + field.offset);
    }
public:
    explicit Hlsl(const MaterialModule& material) : material_(material), program_(material.program), emitted_(program_.value_layouts.size()) {}
    static bool supports(MathOperation operation) { return !intrinsic(operation).empty(); }
    std::string generate() {
        text_ = "// Generated from typed Source. Sampling is linear-float4, explicit LOD-0.\n";
        std::set<std::pair<MathOperation, std::string>> helpers;
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            if (!program_.reachable[i]) continue;
            const auto& function = program_.functions[i];
            declare_type(function.result);
            for (const auto& local : function.locals) declare_type(local.type);
            for (const auto& node : function.expressions) {
                const auto operation = node.math_operation;
                if (node.kind != Expression::Kind::math_intrinsic || (operation != MathOperation::clamp &&
                    operation != MathOperation::lerp && operation != MathOperation::fract)) continue;
                const auto value = type(node.type);
                if (!helpers.emplace(operation, value).second) continue;
                // Typed value arguments evaluate each operand once. These Source
                // contracts differ from HLSL's built-ins; precise prevents DXC
                // from reassociating lerp or folding subtraction back into frac.
                text_ += value + " " + intrinsic(operation) + "(" + value + " a";
                if (operation == MathOperation::fract) {
                    text_ += ") { precise " + value + " result = a - floor(a); return min(result,asfloat(0x3f7fffffu)); }\n";
                } else {
                    text_ += "," + value + " b," + value + " c) { ";
                    text_ += operation == MathOperation::clamp ? "return max(b,min(a,c)); }\n" :
                        "precise " + value + " result = a * (1.0 - c) + b * c; return result; }\n";
                }
            }
        }
        for (std::size_t i = 0; i < program_.functions.size(); ++i)
            if (program_.reachable[i]) text_ += signature(i) + ";\n";
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            if (!program_.reachable[i]) continue;
            text_ += signature(i) + "\n";
            statement(program_.functions[i], program_.functions[i].body);
        }
        text_ += "ms_Output ms_shade(uint environment, ms_Context context, ms_Parameters parameters) {\n";
        if (material_.resources != UINT32_MAX) {
            const auto resource_type = type({ValueType::record, material_.resources});
            text_ += resource_type + " resources = (" + resource_type + ")0;\n";
            for (const auto& input : material_.textures) {
                std::string path = "resources.ms_";
                for (const auto character : input.key) path += character == '.' ? ".ms_" : std::string(1, character);
                text_ += path + " = " + std::to_string(input.slot) + "u;\n";
            }
        }
        text_ += "return ms_f" + std::to_string(program_.exports[0]) + "(environment,context,parameters" +
            (material_.resources == UINT32_MAX ? "); }\n" : ",resources); }\n");
        text_ += "ms_Parameters ms_defaults() { return ms_f" + std::to_string(program_.exports[1]) + "(0u); }\n";
        text_ += "ms_Parameters ms_load_parameters(ByteAddressBuffer data, uint base) {\nms_Parameters p = (ms_Parameters)0;\n";
        load_parameters({ValueType::record, material_.parameters}, "p", 0);
        text_ += "return p;\n}\n";
        return std::move(text_);
    }
};
}

std::expected<MaterialModule, Diagnostic> compile_material(std::string_view name, std::string_view source,
    std::span<const ValueLayout> values, std::uint32_t context, std::uint32_t output, std::uint32_t texture_input) {
    try {
        const ValueType token{ValueType::record, texture_input};
        const auto& token_layout = values[texture_input];
        if (!token_layout.plain_value || !token_layout.can_own || token_layout.math || token_layout.fields.size() != 1 ||
            token_layout.fields[0].name != "slot" || token_layout.fields[0].type != ValueType::u32)
            reject({}, "Material TextureInput requires its registered plain uint slot field.");
        const auto slot_offset = token_layout.fields[0].offset;
        auto tree = compile_source(name, source, {{}, {}, {}, {}, values, true, token}, "shade");
        if (!tree) return std::unexpected(tree.error());
        const std::string_view entries[]{"shade", "defaults"};
        auto exported = export_functions(*tree, entries, false);
        if (!exported) return std::unexpected(exported.error());
        const auto& shade = tree->functions[tree->exports[0]];
        const auto& defaults = tree->functions[tree->exports[1]];
        if ((shade.parameters.size() != 2 && shade.parameters.size() != 3) || shade.parameters[0].type != ValueType{ValueType::record, context} ||
            shade.result != ValueType{ValueType::record, output} || shade.parameters[1].type.kind != ValueType::record)
            reject(shade.position, "Material requires Output shade(Context, Params[, Resources]), with exact registered Context/Output types.");
        const auto params = shade.parameters[1].type;
        if (context == output || params.type_index == context || params.type_index == output || values[params.type_index].math)
            reject(shade.position, "Material Context, Output and Params must be distinct plain structs.");
        if (!defaults.parameters.empty() || defaults.result != params)
            reject(defaults.position, "Material requires Params defaults() with no arguments.");
        std::function<bool(ValueType)> contains_texture = [&](ValueType type) {
            if (type == token) return true;
            if (type.kind != ValueType::record) return false;
            return std::ranges::any_of(values[type.type_index].fields, [&](const auto& field) { return contains_texture(field.type); });
        };
        if (contains_texture({ValueType::record, context}) || contains_texture({ValueType::record, output}))
            reject(shade.position, "Material Context/Output cannot contain texture inputs.");
        std::vector<bool> checked(values.size());
        std::function<void(ValueType, SourcePosition)> check_type = [&](ValueType type, SourcePosition position) {
            if (type == ValueType::f32 || type == ValueType::boolean || type == ValueType::void_value || type == token) return;
            if (type.kind != ValueType::record) reject(position, "Material values require float, bool, float vectors or plain structs.");
            const auto& layout = values[type.type_index];
            if (checked[type.type_index]) return;
            checked[type.type_index] = true;
            if (!layout.plain_value || !layout.can_own || (layout.math && (!layout.math.vector() || layout.math.precision != math::Precision::f32)))
                reject(position, "Material values cannot contain Runtime resources, references or unsupported math types.");
            if (layout.fields.empty()) reject(position, "Material structs must expose their value fields.");
            for (const auto& field : layout.fields) check_type(field.type, position);
        };
        check_type(params, shade.position);
        std::set<std::string> keys;
        std::function<void(ValueType, std::string)> check_parameters = [&](ValueType type, std::string path) {
            if (type == ValueType::f32 || (type.kind == ValueType::record && values[type.type_index].math)) {
                keys.insert(std::move(path));
                return;
            }
            if (type.kind != ValueType::record || type == token)
                reject(shade.position, "Material parameters require float scalars, vectors or nested structs, not texture inputs.");
            for (const auto& field : values[type.type_index].fields)
                check_parameters(field.type, path.empty() ? field.name : path + "." + field.name);
        };
        check_parameters(params, {});
        std::uint32_t resources = UINT32_MAX;
        std::vector<MaterialTextureInput> textures;
        if (shade.parameters.size() == 3) {
            const auto type = shade.parameters[2].type;
            if (type.kind != ValueType::record || type == token || values[type.type_index].math)
                reject(shade.position, "Material Resources must be a struct of TextureInput leaves.");
            resources = type.type_index;
            check_type(type, shade.position);
            std::function<void(ValueType, std::string, std::size_t)> collect = [&](ValueType value, std::string path, std::size_t offset) {
                if (value == token) {
                    if (!keys.insert(path).second) reject(shade.position, "Material numeric/resource parameter key conflict: " + path);
                    textures.push_back({std::move(path), static_cast<std::uint32_t>(textures.size()), offset + slot_offset});
                    return;
                }
                if (value.kind != ValueType::record || values[value.type_index].math)
                    reject(shade.position, "Material Resources require TextureInput leaves, not numeric parameters.");
                for (const auto& field : values[value.type_index].fields)
                    collect(field.type, path.empty() ? field.name : path + "." + field.name, offset + field.offset);
            };
            collect(type, {}, 0);
        }
        std::vector<bool> visited(tree->functions.size());
        std::function<void(std::uint32_t)> check_defaults = [&](std::uint32_t index) {
            if (visited[index]) return;
            visited[index] = true;
            const auto& function = tree->functions[index];
            if (contains_texture(function.result) || std::ranges::any_of(function.locals,
                [&](const auto& local) { return contains_texture(local.type); }))
                reject(function.position, "Material defaults cannot contain texture inputs.");
            for (const auto& node : function.expressions) {
                if (node.kind == Expression::Kind::texture_sample) reject(node.position, "Material defaults cannot sample textures.");
                if (node.kind == Expression::Kind::source_call) check_defaults(node.value);
            }
        };
        check_defaults(tree->exports[1]);
        for (std::size_t index = 0; index < tree->functions.size(); ++index) {
            if (!tree->reachable[index]) continue;
            auto& function = tree->functions[index];
            if (function.is_coroutine || function.requires_receiver) reject(function.position, "Material functions cannot use objects or coroutines.");
            check_type(function.result, function.position);
            for (const auto& local : function.locals) check_type(local.type, function.position);
            for (const auto& parameter : function.parameters)
                if (parameter.passing != ParameterPassing::value) reject(function.position, "Material function parameters are values, not borrows.");
            for (const auto& node : function.statements) {
                if (node.kind == Statement::Kind::loop || node.kind == Statement::Kind::yield ||
                    node.kind == Statement::Kind::break_loop || node.kind == Statement::Kind::continue_loop)
                    reject(node.position, "Material does not support loops or suspension.");
                if (node.kind == Statement::Kind::declare && node.expression == no_node && contains_texture(function.locals[node.target].type))
                    reject(node.position, "Texture inputs cannot be default constructed; copy an injected value.");
            }
            for (auto& node : function.expressions) {
                using Kind = Expression::Kind;
                if (node.kind == Kind::value_field && function.expressions[node.left].type == token)
                    reject(node.position, "TextureInput members are compiler-owned, not accessible to material Source.");
                if (!(node.kind == Kind::literal && node.type == ValueType::u32)) check_type(node.type, node.position);
                switch (node.kind) {
                case Kind::literal: case Kind::local: case Kind::value_field: case Kind::math_construct: case Kind::source_call: break;
                case Kind::unary: case Kind::binary:
                    if (node.operation == Operation::remainder || (node.operation >= Operation::bitwise_and && node.operation <= Operation::shift_right))
                        reject(node.position, "Material integer arithmetic is not supported.");
                    break;
                case Kind::scalar_cast:
                    if (node.type != ValueType::f32) reject(node.position, "Material conversions only produce float.");
                    break;
                case Kind::math_intrinsic:
                    if (!Hlsl::supports(node.math_operation)) reject(node.position, "This math operation is not in the material subset.");
                    break;
                case Kind::texture_sample:
                    node.offset = slot_offset;
                    break;
                default: reject(node.position, "Operation is not part of the pure material subset.");
                }
            }
        }
        tree->material = true;
        return MaterialModule{std::move(*tree), context, params.type_index, output, texture_input, resources, std::move(textures)};
    } catch (Diagnostic& diagnostic) { return std::unexpected(std::move(diagnostic)); }
}
std::string material_hlsl(const MaterialModule& material) { return Hlsl(material).generate(); }
}
