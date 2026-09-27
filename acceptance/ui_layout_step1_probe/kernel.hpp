#pragma once
// RESEARCH ONLY: value-batch kernel for testing size dependencies and publication.
// Not the production Interface: caller IDs are not generational resource handles.
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace layout_step1 {
constexpr uint32_t no_parent=std::numeric_limits<uint32_t>::max();
enum class Code { bad_input, limit, unsupported_dependency, busy, provider, numeric_range };
struct Failure { Code code; uint32_t node=no_parent; };
template<class T> using Result=std::expected<T,Failure>;
struct Size { float width{},height{}; bool operator==(const Size&) const=default; };
enum class Rule { fixed,content,expand };
struct Axis {
    Rule rule{Rule::content}; float value{}; float minimum{}; std::optional<float> maximum;
    static Axis fixed(float size){return {Rule::fixed,size,0,{}};}
    static Axis content(float minimum=0,std::optional<float> maximum={}){return {Rule::content,0,minimum,maximum};}
    static Axis expand(float weight=1,float minimum=0,std::optional<float> maximum={}){return {Rule::expand,weight,minimum,maximum};}
    bool operator==(const Axis&) const=default;
};
struct Edges {float left{},top{},right{},bottom{};bool operator==(const Edges&) const=default;};
enum class Kind { row,column,overlay,box,measured };
struct Node {
    uint64_t id{};uint32_t parent{no_parent};Kind kind{Kind::box};
    Axis width{Axis::expand()},height{Axis::content()};Edges padding{};float gap{};
    bool anchored{},fill_width{true},fill_height{true};
    float align_x{},align_y{},anchor_x{},anchor_y{},pivot_x{},pivot_y{},offset_x{},offset_y{};
    uint64_t content_key{},content_revision{};
    bool operator==(const Node&) const=default;
};
struct Box {uint64_t id{},parent_id{};float x{},y{},width{},height{};bool operator==(const Box&) const=default;};
struct Query {uint64_t content_key{},content_revision{},provider_revision{};std::optional<float> width;bool operator==(const Query&) const=default;};
// Empty width asks unwrapped intrinsic width; known width asks natural H(W).
// No viewport height is passed as an implicit content-height bound.
using Measure=Result<Size>(*)(void*,const Query&);
struct Limits {uint32_t nodes{},depth{};};
struct Stats {uint64_t computes{},publications{},measure_calls{},intrinsic_queries{},width_queries{},cache_hits{},visited{};};
// Borrowed geometry survives input edits and failed computes; the next successful
// publication or Kernel destruction ends the borrow. Copy explicitly to retain it.
struct View {uint64_t publication{},input_revision{};std::span<const Box> boxes;};
struct Share {double minimum{},weight{};std::optional<double> maximum;};
// Expanding slots share the entire pool: clamp(lambda*weight,min,max).
Result<std::vector<double>> distribute(std::span<const Share> items,double pool);

class Kernel {
public:
    Kernel(Measure measure,void* user,Limits limits):measure_(measure),user_(user),limits_(limits){}
    Kernel(const Kernel&)=delete;Kernel& operator=(const Kernel&)=delete;
    Result<void> set_nodes(std::span<const Node>);
    Result<View> compute(Size viewport,uint64_t provider_revision);
    View view() const {return {publication_,published_input_,published_};}
    Stats stats() const {return stats_;}
private:
    struct Cached {bool valid{};Query query{};Size size{};uint64_t attempt{};};
    struct Cache {Cached intrinsic,constrained;uint64_t touched{};};
    Result<Size> measured(uint32_t node,std::optional<float> width);
    Result<float> intrinsic(uint32_t node);
    Result<void> resolve_widths(Size);
    std::vector<Share> shares_;std::vector<uint32_t> share_indices_;
    std::vector<double> shares_out_,weight_sums_;std::vector<std::pair<double,std::size_t>> events_;
    Result<void> resolve_heights();
    Result<void> arrange(Size);
    Measure measure_;void* user_;Limits limits_;bool active_{};
    std::vector<Node> nodes_;std::vector<uint32_t> offsets_,children_,order_;
    std::vector<Box> working_,published_;std::vector<Cache> cache_,pending_;
    std::vector<float> intrinsic_width_,natural_height_,slot_width_,slot_height_;
    std::vector<uint8_t> intrinsic_valid_;std::vector<uint32_t> intrinsic_stack_,touched_;
    uint64_t input_revision_{},published_input_{},publication_{},provider_revision_{},published_provider_{},attempt_{};
    Size published_viewport_{};Stats stats_{};
};
}
