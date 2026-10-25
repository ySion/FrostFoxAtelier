#include "assets/mesh_import.hpp"
#include "assets/assets.hpp"
#include "assets/file.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace atelier::assets {
namespace {
using Json=nlohmann::json;
// Sized for the 2K Intel Sponza (largest file: 3.75M triangles, 134 MiB of buffers, 72 images,
// 1.1 GiB decoded) with about 2x headroom; documented in docs/modules/editor/projects.md.
constexpr std::size_t maximum_document=16ull<<20, maximum_buffers=512ull<<20, maximum_image_file=64ull<<20,
    maximum_images=256, maximum_decoded=2ull<<30, maximum_triangles=8'000'000, maximum_nodes=65536;
constexpr std::size_t no_slot=~std::size_t{};

const Json empty_array=Json::array(),empty_object=Json::object();

void require(bool ok,std::string_view message) { if(!ok) throw std::runtime_error("glTF: "+std::string(message)); }
const Json& field(const Json& object,std::string_view key,const Json& fallback) {
    const auto found=object.find(key);return found==object.end()?fallback:*found;
}
const Json& member(const Json& object,std::string_view key) {
    const auto found=object.find(key);require(found!=object.end(),"missing "+std::string(key)+".");return *found;
}
const Json& element(const Json& gltf,std::string_view array,const Json& index) {
    const auto& items=member(gltf,array);
    require(items.is_array() && index.is_number_unsigned() && index.get<std::size_t>()<items.size(),"invalid "+std::string(array)+" index.");
    return items[index.get<std::size_t>()];
}
double number(const Json& object,std::string_view key,double fallback) {
    const auto found=object.find(key);if(found==object.end()) return fallback;
    require(found->is_number() && std::isfinite(found->get<double>()),std::string(key)+" must be a finite number.");
    return found->get<double>();
}
template<std::size_t N> std::array<float,N> numbers(const Json& object,std::string_view key,std::array<float,N> fallback) {
    const auto found=object.find(key);if(found==object.end()) return fallback;
    require(found->is_array() && found->size()==N,std::string(key)+" has the wrong length.");
    std::array<float,N> result{};
    for(std::size_t i=0;i<N;++i) {
        require((*found)[i].is_number() && std::isfinite((*found)[i].get<double>()),std::string(key)+" must hold finite numbers.");
        result[i]=(*found)[i].get<float>();
    }
    return result;
}
// RFC 3986 percent-decoding; the result is a relative path inside the source directory.
std::filesystem::path dependency(const Json& uri) {
    require(uri.is_string(),"uri must be a string.");
    const auto text=uri.get<std::string>();
    require(!text.starts_with("data:"),"embedded data URIs are not supported.");
    std::string decoded;
    for(std::size_t i=0;i<text.size();++i) {
        if(text[i]!='%') {decoded+=text[i];continue;}
        unsigned value{};
        require(i+2<text.size() && std::from_chars(text.data()+i+1,text.data()+i+3,value,16).ptr==text.data()+i+3,"invalid percent-encoding in uri.");
        decoded+=static_cast<char>(value);i+=2;
    }
    const auto path=std::filesystem::path(wide(decoded));
    require(!path.empty() && !path.has_root_path(),"dependency must be a relative path.");
    for(const auto& part:path) require(part!=L".." && part.wstring().find(L':')==std::wstring::npos,"dependency must stay inside the source directory.");
    return path.lexically_normal();
}
using Matrix=std::array<double,16>; // Column-major, as glTF stores it.
Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix r{};
    for(std::size_t c=0;c<4;++c) for(std::size_t row=0;row<4;++row)
        for(std::size_t k=0;k<4;++k) r[c*4+row]+=a[k*4+row]*b[c*4+k];
    return r;
}
Matrix local_matrix(const Json& node) {
    if(node.contains("matrix")) {
        const auto m=numbers<16>(node,"matrix",{});Matrix r{};std::ranges::copy(m,r.begin());return r;
    }
    const auto t=numbers<3>(node,"translation",{0,0,0});const auto q=numbers<4>(node,"rotation",{0,0,0,1});const auto s=numbers<3>(node,"scale",{1,1,1});
    const double x=q[0],y=q[1],z=q[2],w=q[3];
    const std::array<double,9> rotation{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w), 2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w), 2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)};
    Matrix r{};
    for(std::size_t c=0;c<3;++c) for(std::size_t row=0;row<3;++row) r[c*4+row]=rotation[c*3+row]*s[c];
    r[12]=t[0];r[13]=t[1];r[14]=t[2];r[15]=1;return r;
}
struct Accessor { const std::byte* data{}; std::size_t count{},stride{}; std::uint32_t component{}; };
std::size_t component_size(std::uint32_t type) {return type==5121?1:type==5123?2:4;}
Accessor accessor(const Json& gltf,std::span<const std::string_view> buffers,const Json& index,std::string_view type,std::initializer_list<std::uint32_t> components) {
    const auto& a=element(gltf,"accessors",index);
    require(!a.contains("sparse"),"sparse accessors are not supported.");
    require(member(a,"type").get<std::string>()==type,"accessor has the wrong type for its attribute.");
    const auto component=member(a,"componentType").get<std::uint32_t>();
    require(std::ranges::find(components,component)!=components.end(),"accessor has an unsupported component type.");
    require(!a.value("normalized",false),"normalized accessors are not supported.");
    const auto& view=element(gltf,"bufferViews",member(a,"bufferView"));
    const auto buffer=member(view,"buffer");require(buffer.is_number_unsigned() && buffer.get<std::size_t>()<buffers.size(),"invalid buffer index.");
    const auto bytes=buffers[buffer.get<std::size_t>()];
    const std::uint64_t width=(type=="SCALAR"?1:type=="VEC2"?2:type=="VEC3"?3:4)*component_size(component);
    const std::uint64_t view_offset=view.value("byteOffset",std::uint64_t{}),view_length=member(view,"byteLength").get<std::uint64_t>();
    const std::uint64_t offset=a.value("byteOffset",std::uint64_t{}),count=member(a,"count").get<std::uint64_t>();
    const std::uint64_t stride=view.value("byteStride",width);
    require(count>0 && count<=std::uint64_t{1}<<32 && stride>=width && stride<=252,"accessor has an invalid count or stride.");
    require(view_offset<=bytes.size() && view_length<=bytes.size()-view_offset && offset<=view_length &&
        (count-1)*stride+width<=view_length-offset,"accessor exceeds its buffer.");
    return {reinterpret_cast<const std::byte*>(bytes.data())+view_offset+offset,static_cast<std::size_t>(count),static_cast<std::size_t>(stride),component};
}
std::uint32_t index_at(const Accessor& a,std::size_t i) {
    const auto* p=a.data+i*a.stride;
    if(a.component==5121) return std::to_integer<std::uint32_t>(*p);
    if(a.component==5123) {std::uint16_t v;std::memcpy(&v,p,2);return v;}
    std::uint32_t v;std::memcpy(&v,p,4);return v;
}
template<std::size_t N> std::array<double,N> floats_at(const Accessor& a,std::size_t i) {
    std::array<float,N> v;std::memcpy(v.data(),a.data+i*a.stride,N*4);
    std::array<double,N> result;
    for(std::size_t k=0;k<N;++k) {require(std::isfinite(v[k]),"a vertex attribute is not finite.");result[k]=v[k];}
    return result;
}
std::string alpha_mode(const Json& material) {
    auto mode=material.value("alphaMode",std::string("OPAQUE"));
    require(mode=="OPAQUE" || mode=="MASK" || mode=="BLEND","invalid alphaMode.");return mode;
}
std::string material_name(const Json& material,std::size_t index) {
    auto name=material.value("name",std::string{});
    return name.empty()?"material "+std::to_string(index):name;
}
}

MeshImport load_gltf(const std::filesystem::path& source,ImageCache* shared_images) {
    MeshImport result;
    const auto root=std::filesystem::canonical(source.parent_path());
    std::unordered_map<std::wstring,std::size_t> read_files;
    const auto read=[&](const std::filesystem::path& relative,std::size_t maximum) {
        if(const auto found=read_files.find(relative.generic_wstring());found!=read_files.end()) {
            require(result.files[found->second].bytes.size()<=maximum,"a dependency exceeds its size limit.");return found->second;
        }
        const auto path=std::filesystem::weakly_canonical(root/relative);
        const auto within=path.lexically_relative(root);
        require(!within.empty() && !within.has_root_path() && *within.begin()!=L"..","dependency escapes through a link.");
        auto bytes=read_file(path,maximum);if(!bytes) throw std::runtime_error(bytes.error().message);
        read_files.emplace(relative.generic_wstring(),result.files.size());
        result.files.push_back({relative,std::move(*bytes)});return result.files.size()-1;
    };
    Json gltf;
    try {gltf=Json::parse(result.files[read(source.filename(),maximum_document)].bytes);}
    catch(const Json::exception& error) {throw std::runtime_error("glTF: invalid JSON: "+std::string(error.what()));}
    try {
        require(gltf.is_object(),"document must be an object.");
        require(member(member(gltf,"asset"),"version").get<std::string>().starts_with("2."),"only glTF 2.x is supported.");
        if(const auto found=gltf.find("extensionsRequired");found!=gltf.end() && !found->empty())
            require(false,"required extensions are not supported: "+found->dump());
        // glTF is right-handed and the engine left-handed, both +Y up in meters (renderer contract:
        // "相机为左手系，+Y 向上"): mirroring X keeps an asset's +Z front facing +Z.
        const Matrix conversion{-1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const auto& nodes=field(gltf,"nodes",empty_array);
        require(nodes.is_array() && nodes.size()<=maximum_nodes,"too many nodes.");
        const auto& scene=element(gltf,"scenes",gltf.value("scene",Json(0u)));
        // Depth-first in file order, so parts follow the document.
        std::vector<std::pair<std::size_t,Matrix>> placed,stack;std::vector<bool> visited(nodes.size());
        const auto push=[&](const Json& indices,const Matrix& parent) {
            require(indices.is_array(),"node lists must be arrays.");
            for(auto index=indices.rbegin();index!=indices.rend();++index) {
                require(index->is_number_unsigned() && index->get<std::size_t>()<nodes.size(),"invalid node index.");
                stack.push_back({index->get<std::size_t>(),parent});
            }
        };
        push(field(scene,"nodes",empty_array),conversion);
        while(!stack.empty()) {
            const auto [index,parent]=stack.back();stack.pop_back();
            require(!visited[index],"node graph is not a tree.");visited[index]=true;
            const auto& node=nodes[index];require(!node.contains("skin"),"skinned meshes are not supported.");
            const auto world=multiply(parent,local_matrix(node));
            if(node.contains("mesh")) placed.push_back({index,world});
            push(field(node,"children",empty_array),world);
        }
        const auto primitives=[&](std::size_t node)->const Json& {return member(element(gltf,"meshes",member(nodes[node],"mesh")),"primitives");};
        const auto corner_count=[&](const Json& primitive) {
            return member(element(gltf,"accessors",primitive.contains("indices")?primitive["indices"]:member(member(primitive,"attributes"),"POSITION")),"count").get<std::size_t>();
        };
        // Material slots: materials used by imported primitives, in file order; BLEND is left out.
        const auto& materials=field(gltf,"materials",empty_array);
        std::vector<bool> used(materials.size());bool default_used=false;std::size_t corners{};
        struct Skipped {std::size_t primitives{},triangles{};};std::map<std::size_t,Skipped> skipped;
        for(const auto& [index,world]:placed) for(const auto& primitive:primitives(index)) {
            require(primitive.value("mode",4)==4,"only triangle-list primitives are supported.");
            const auto count=corner_count(primitive);
            if(primitive.contains("material")) {
                const auto& m=primitive["material"];const auto& material=element(gltf,"materials",m);
                if(alpha_mode(material)=="BLEND") {auto& s=skipped[m.get<std::size_t>()];++s.primitives;s.triangles+=count/3;continue;}
                used[m.get<std::size_t>()]=true;
            } else default_used=true;
            corners+=count;require(corners<=maximum_triangles*3,"too many triangles.");
        }
        for(const auto& [m,count]:skipped) result.notes.push_back("BLEND material skipped: "+material_name(materials[m],m)+
            " ("+std::to_string(count.primitives)+" primitives, "+std::to_string(count.triangles)+" triangles); alpha blending is not supported.");
        // Read every dependency before taking views: result.files grows while reading.
        std::vector<std::size_t> buffer_files;std::size_t buffer_bytes{};
        for(const auto& buffer:field(gltf,"buffers",empty_array)) {
            const auto file=read(dependency(member(buffer,"uri")),maximum_buffers-buffer_bytes);
            buffer_bytes+=result.files[file].bytes.size();
            require(member(buffer,"byteLength").get<std::uint64_t>()<=result.files[file].bytes.size(),"buffer is shorter than its byteLength.");
            buffer_files.push_back(file);
        }
        std::vector<std::size_t> image_slot(field(gltf,"images",empty_array).size(),no_slot),image_files;
        const auto texture_image=[&](const Json& info) {
            require(info.value("texCoord",0)==0,"textures must use TEXCOORD_0.");
            const auto& texture=element(gltf,"textures",member(info,"index"));
            const auto& image=element(gltf,"images",member(texture,"source"));
            auto& slot=image_slot[texture["source"].get<std::size_t>()];
            if(slot==no_slot) {
                require(image_files.size()<maximum_images,"too many images.");
                require(!image.contains("bufferView"),"images stored in buffers are not supported.");
                const auto path=dependency(member(image,"uri"));
                // Generated materials reference the image as a Project texture asset.
                require(is_texture_path(path),"images must be PNG, JPEG, BMP or TIFF files.");
                slot=image_files.size();image_files.push_back(read(path,maximum_image_file));
            }
            return slot;
        };
        const auto pbr_of=[](const Json& material)->const Json& {return field(material,"pbrMetallicRoughness",empty_object);};
        for(std::size_t m=0;m<materials.size();++m) if(used[m]) {
            const auto& pbr=pbr_of(materials[m]);
            for(const auto* key:{"baseColorTexture","metallicRoughnessTexture"}) if(pbr.contains(key)) texture_image(pbr[key]);
            for(const auto* key:{"normalTexture","emissiveTexture"}) if(materials[m].contains(key)) texture_image(materials[m][key]);
        }
        std::vector<std::string_view> buffers,image_sources;
        for(const auto file:buffer_files) buffers.push_back(result.files[file].bytes);
        for(const auto file:image_files) image_sources.push_back(result.files[file].bytes);
        const auto decoded=decode_images(image_sources,shared_images,maximum_decoded);
        // An empty object is the glTF default material, used by primitives without one.
        const auto surface=[&](const Json& source_material,std::array<std::filesystem::path,material::input_count>& files) {
            material::Description description;auto& p=description.parameters;
            const auto texture=[&](material::Input input,const Json& info,std::uint32_t channel) {
                auto& target=description.texture(input);const auto image=texture_image(info);
                target.image=decoded[image];target.channel=channel;files[static_cast<std::size_t>(input)]=result.files[image_files[image]].relative;
                const auto& source_texture=element(gltf,"textures",info["index"]);
                const auto& sampler=source_texture.contains("sampler")?element(gltf,"samplers",source_texture["sampler"]):empty_object;
                const auto wrap=sampler.value("wrapS",10497);
                require(wrap==sampler.value("wrapT",10497) && (wrap==10497 || wrap==33071 || wrap==33648),"samplers must use one supported wrap mode for both axes.");
                target.address=wrap==33071?material::Address::clamp:wrap==33648?material::Address::mirror:material::Address::repeat;
            };
            const auto& pbr=pbr_of(source_material);
            p.base_color=numbers<4>(pbr,"baseColorFactor",{1,1,1,1});
            p.metallic=static_cast<float>(number(pbr,"metallicFactor",1));p.roughness=static_cast<float>(number(pbr,"roughnessFactor",1));
            if(pbr.contains("baseColorTexture")) texture(material::Input::base_color,pbr["baseColorTexture"],0);
            if(pbr.contains("metallicRoughnessTexture")) {
                texture(material::Input::roughness,pbr["metallicRoughnessTexture"],1);
                texture(material::Input::metallic,pbr["metallicRoughnessTexture"],2);
            }
            if(source_material.contains("normalTexture")) {
                texture(material::Input::normal,source_material["normalTexture"],0);
                p.normal_scale=static_cast<float>(number(source_material["normalTexture"],"scale",1));
            }
            p.emissive=numbers<3>(source_material,"emissiveFactor",{0,0,0});
            if(source_material.contains("emissiveTexture")) texture(material::Input::emissive,source_material["emissiveTexture"],0);
            if(alpha_mode(source_material)=="MASK") {
                p.coverage=material::Coverage::mask;p.alpha_cutoff=static_cast<float>(number(source_material,"alphaCutoff",.5));
            }
            p.double_sided=p.emission_two_sided=source_material.value("doubleSided",false);
            return std::make_shared<const material::Surface>(std::move(description));
        };
        // Slot names are unique: scenes bind materials to slots by name.
        std::vector<material::Slot> slots;std::vector<std::size_t> slot_of(materials.size(),no_slot);std::unordered_set<std::string> names;
        const auto add_slot=[&](const std::string& name,const Json& source_material) {
            auto unique=name;for(std::size_t n=2;!names.insert(unique).second;++n) unique=name+" ("+std::to_string(n)+")";
            slots.push_back({std::move(unique),surface(source_material,result.textures.emplace_back())});
        };
        for(std::size_t m=0;m<materials.size();++m) if(used[m]) {slot_of[m]=slots.size();add_slot(material_name(materials[m],m),materials[m]);}
        const auto default_slot=slots.size();
        if(default_used) add_slot("default",empty_object);
        std::vector<geometry::Vertex> vertices;vertices.reserve(corners);std::vector<geometry::MeshPart> parts;
        for(const auto& [index,world]:placed) {
            const std::array<double,9> linear{world[0],world[1],world[2],world[4],world[5],world[6],world[8],world[9],world[10]};
            const double det=linear[0]*(linear[4]*linear[8]-linear[7]*linear[5])-linear[3]*(linear[1]*linear[8]-linear[7]*linear[2])+linear[6]*(linear[1]*linear[5]-linear[4]*linear[2]);
            require(std::isfinite(det) && det!=0,"a mesh node has a singular transform.");
            // Normals use the cofactor matrix: the inverse transpose scaled by the positive |det|.
            const double sign=det<0?-1:1;
            const std::array<double,9> cofactor{
                (linear[4]*linear[8]-linear[5]*linear[7])*sign,(linear[5]*linear[6]-linear[3]*linear[8])*sign,(linear[3]*linear[7]-linear[4]*linear[6])*sign,
                (linear[2]*linear[7]-linear[1]*linear[8])*sign,(linear[0]*linear[8]-linear[2]*linear[6])*sign,(linear[1]*linear[6]-linear[0]*linear[7])*sign,
                (linear[1]*linear[5]-linear[2]*linear[4])*sign,(linear[2]*linear[3]-linear[0]*linear[5])*sign,(linear[0]*linear[4]-linear[1]*linear[3])*sign};
            const auto apply=[](const std::array<double,9>& m,const auto& v) {
                return std::array<double,3>{m[0]*v[0]+m[3]*v[1]+m[6]*v[2],m[1]*v[0]+m[4]*v[1]+m[7]*v[2],m[2]*v[0]+m[5]*v[1]+m[8]*v[2]};
            };
            const auto store_unit=[](const std::array<double,3>& v,float* out,std::string_view what) {
                const auto length=std::hypot(v[0],v[1],v[2]);require(length>0 && std::isfinite(length),what);
                for(std::size_t axis=0;axis<3;++axis) out[axis]=static_cast<float>(v[axis]/length);
            };
            for(const auto& primitive:primitives(index)) {
                const auto slot=primitive.contains("material")?slot_of[primitive["material"].get<std::size_t>()]:default_slot;
                if(slot==no_slot) continue; // BLEND, reported in notes.
                const auto& attributes=member(primitive,"attributes");
                const auto position=accessor(gltf,buffers,member(attributes,"POSITION"),"VEC3",{5126});
                const auto optional=[&](const char* name,std::string_view type) {
                    std::optional<Accessor> a;
                    if(attributes.contains(name)) {a=accessor(gltf,buffers,attributes[name],type,{5126});require(a->count==position.count,"vertex attributes differ in count.");}
                    return a;
                };
                const auto normal=optional("NORMAL","VEC3"),tangent=optional("TANGENT","VEC4"),uv=optional("TEXCOORD_0","VEC2");
                require(uv || std::ranges::none_of(slots[slot].surface->description().textures,[](const auto& t){return bool(t.image);}),"a textured primitive has no TEXCOORD_0.");
                std::vector<geometry::Vertex> source(position.count);
                for(std::size_t v=0;v<position.count;++v) {
                    auto& out=source[v];const auto p=floats_at<3>(position,v);
                    for(std::size_t axis=0;axis<3;++axis)
                        out.position[axis]=static_cast<float>(world[axis]*p[0]+world[4+axis]*p[1]+world[8+axis]*p[2]+world[12+axis]);
                    if(normal) store_unit(apply(cofactor,floats_at<3>(*normal,v)),out.normal,"a normal is zero.");
                    if(tangent) {
                        const auto t=floats_at<4>(*tangent,v);const auto mapped=apply(linear,t);
                        const auto length=std::hypot(mapped[0],mapped[1],mapped[2]);
                        for(std::size_t axis=0;axis<3;++axis) out.tangent[axis]=length>0?static_cast<float>(mapped[axis]/length):0.f;
                        // The bitangent cross(N,T)*w must follow the transformed surface, so a mirror flips w.
                        out.tangent[3]=(t[3]<0)==(det<0)?1.f:-1.f;
                    }
                    if(uv) {const auto t=floats_at<2>(*uv,v);out.uv[0]=static_cast<float>(t[0]);out.uv[1]=static_cast<float>(t[1]);}
                }
                std::optional<Accessor> indices;
                if(primitive.contains("indices")) indices=accessor(gltf,buffers,primitive["indices"],"SCALAR",{5121,5123,5125});
                const auto count=indices?indices->count:position.count;
                require(count%3==0,"a triangle list has a partial triangle.");
                const auto first=vertices.size();
                for(std::size_t c=0;c<count;c+=3) {
                    std::array<geometry::Vertex,3> triangle;
                    for(std::size_t k=0;k<3;++k) {
                        const auto i=indices?index_at(*indices,c+k):static_cast<std::uint32_t>(c+k);
                        require(i<position.count,"an index exceeds its vertex count.");triangle[k]=source[i];
                    }
                    if(!normal) {
                        // glTF fronts are counter-clockwise before the node transform; sign(det) restores them after it.
                        std::array<double,3> a{},b{};
                        for(std::size_t axis=0;axis<3;++axis) {a[axis]=double(triangle[1].position[axis])-triangle[0].position[axis];b[axis]=double(triangle[2].position[axis])-triangle[0].position[axis];}
                        float flat[3];
                        store_unit({(a[1]*b[2]-a[2]*b[1])*sign,(a[2]*b[0]-a[0]*b[2])*sign,(a[0]*b[1]-a[1]*b[0])*sign},flat,"a triangle without normals is degenerate.");
                        for(auto& v:triangle) std::ranges::copy(flat,v.normal);
                    }
                    vertices.insert(vertices.end(),triangle.begin(),triangle.end());
                }
                if(!tangent) {
                    // Mikk takes handedness from the UV winding and assumes normals follow the triangle
                    // winding, which after the conversion holds only when det>0; there its bitangent
                    // points toward +V, while glTF normal maps put +Y toward -V.
                    const auto generated=std::span(vertices).subspan(first);
                    geometry::generate_tangents(generated);
                    if(det>0) for(auto& v:generated) v.tangent[3]=-v.tangent[3];
                }
                parts.push_back({static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(slot)});
            }
        }
        require(!vertices.empty(),"the scene has no importable triangles.");
        result.mesh=std::make_shared<const geometry::Mesh>(std::move(vertices),std::vector<std::uint8_t>{},std::move(slots),std::move(parts),geometry::Tangents::authored);
    } catch(const Json::exception& error) {throw std::runtime_error("glTF: "+std::string(error.what()));}
    return result;
}

bool is_mesh_path(const std::filesystem::path& path) {return path.extension()==L".obj" || path.extension()==L".gltf";}
MeshImport load_mesh(const std::filesystem::path& source,ImageCache* images) {
    if(source.extension()==L".gltf") return load_gltf(source,images);
    if(source.extension()==L".obj") return load_obj(source,images);
    throw std::runtime_error("Mesh sources are Wavefront OBJ or glTF 2.0 (.gltf).");
}
}
