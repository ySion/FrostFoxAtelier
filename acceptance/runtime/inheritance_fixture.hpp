#pragma once

#include "runtime/native.hpp"
#include <stdexcept>

namespace atelier::acceptance::inheritance {
inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class T>
T take(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
    return std::move(*value);
}

inline void take(Result<void> value) {
    if (!value)
        throw std::runtime_error(value.error().message);
}

template <class T>
void reject(Result<T> value, ErrorCode code) {
    require(!value && value.error().code == code, "Inheritance rejection used an unexpected result/code.");
}

inline void collect(Runtime& runtime) {
    if (runtime.collection_phase() == CollectionPhase::idle)
        take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        require(take(runtime.collect_step({1})).work_done <= 1, "Inheritance GC exceeded its budget.");
    take(runtime.begin_collection());
    while (runtime.collection_phase() != CollectionPhase::idle)
        take(runtime.collect_step({1}));
}

struct Base {
    inline static int live{};

    inline static int destroyed{};

    inline static Base* latest{};

    float value{7};

    ReferenceSlot hidden;

    Base() {
        ++live;
        latest = this;
    }

    ~Base() noexcept {
        --live;
        ++destroyed;
    }

    float bump(float amount) {
        value += amount;
        return value;
    }
};

// Unregistered C++ storage forces an adjusted address for the one Runtime base.
struct Prefix {
    virtual ~Prefix() = default;

    std::uint64_t sentinel{0x12345678};
};

struct Derived : Prefix, Base {
    inline static int live{};

    inline static int destroyed{};

    inline static Derived* latest{};

    float extra{11};

    ReferenceSlot edge;

    Derived() {
        ++live;
        latest = this;
    }

    ~Derived() noexcept override {
        --live;
        ++destroyed;
    }

    float derived_value() const { return extra; }
};

struct Resource {
    inline static int live{};

    inline static int destroyed{};

    Resource() { ++live; }

    ~Resource() noexcept {
        --live;
        ++destroyed;
    }
};

struct Failing {
    inline static bool fail{};

    Failing() {
        if (fail)
            throw std::runtime_error("Expected derived-field construction failure.");
    }

    ~Failing() noexcept = default;
};

struct Types {
    TypeId derived;

    TypeId base;

    TypeId described_native;

    TypeId described_base;

    TypeId described_derived;

    TypeId leaf;

    TypeId holder;

    TypeId failing;

    TypeId grand;
};

inline Types register_types(Runtime& runtime) {
    std::vector<TypeDefinition> definitions;
    definitions.push_back(
        NativeRegistration<Derived>{runtime, "inheritance.Derived", TypeKind::object}
            .base<Base>()
            .field<&Derived::extra>("extra")
            .field<&Derived::edge>("edge", {TypeReference{"inheritance.Leaf"}, ValueForm::strong_reference})
            .method<&Derived::derived_value>("derived_value")
            .build());
    definitions.push_back(
        NativeRegistration<Base>{runtime, "inheritance.Base", TypeKind::object}
            .field<&Base::value>("value")
            .hidden_field<&Base::hidden>("hidden", {TypeReference{"inheritance.Leaf"}, ValueForm::strong_reference})
            .method<&Base::bump>("bump")
            .build());
    TypeDefinition mixed{"inheritance.Mixed", TypeKind::object, {}, {}, {}};
    mixed.base = TypeReference::native<Base>();
    mixed.fields = {{"extra", {TypeReference::native<float>()}},
                    {"edge", {TypeReference{"inheritance.Leaf"}, ValueForm::strong_reference}}};
    definitions.push_back(std::move(mixed));
    TypeDefinition plain{"inheritance.Plain", TypeKind::object, {}, {}, {}};
    plain.fields = {
        {"number", {TypeReference::native<float>()}},
        {"hidden", {TypeReference{"inheritance.Leaf"}, ValueForm::strong_reference}, nullptr, nullptr, false}};
    definitions.push_back(std::move(plain));
    TypeDefinition child{"inheritance.PlainChild", TypeKind::object, {}, {}, {}};
    child.base = TypeReference{"inheritance.Plain"};
    child.fields = {{"extra", {TypeReference::native<float>()}},
                    {"edge", {TypeReference{"inheritance.Leaf"}, ValueForm::strong_reference}}};
    definitions.push_back(std::move(child));
    definitions.push_back(TypeDefinition{"inheritance.Leaf", TypeKind::object, {}, {}, {}});
    TypeDefinition holder{"inheritance.Holder", TypeKind::object, {}, {}, {}};
    holder.fields = {{"strong", {TypeReference::native<Base>(), ValueForm::strong_reference}},
                     {"weak", {TypeReference::native<Base>(), ValueForm::weak_reference}}};
    definitions.push_back(std::move(holder));
    TypeDefinition bad{"inheritance.FailingChild", TypeKind::object, {}, {}, {}};
    bad.base = TypeReference::native<Base>();
    bad.fields = {{"resource", {TypeReference::native<Resource>()}}, {"failure", {TypeReference::native<Failing>()}}};
    definitions.push_back(std::move(bad));
    definitions.push_back(NativeRegistration<Resource>{runtime, "inheritance.Resource", TypeKind::record}.build());
    definitions.push_back(NativeRegistration<Failing>{runtime, "inheritance.Failing", TypeKind::record}.build());
    TypeDefinition grand{"inheritance.Grand", TypeKind::object, {}, {}, {}};
    grand.base = TypeReference::native<Derived>();
    grand.fields = {{"tail", {TypeReference::native<float>()}}};
    definitions.push_back(std::move(grand));
    const auto ids = take(runtime.register_types(std::move(definitions)));
    return {ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], ids[6], ids[7], ids[10]};
}
}
