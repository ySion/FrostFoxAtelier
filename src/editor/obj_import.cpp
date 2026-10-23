#include "editor/mesh_import.hpp"
#include "editor/project.hpp"
#include "editor/image_decode.hpp"
#include <charconv>
#include <sstream>
#include <unordered_map>

namespace atelier::editor {
namespace {
void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
std::string rest(std::istringstream& row) {
    std::string value;std::getline(row>>std::ws,value);
    const auto end=value.find_last_not_of(" \t\r");if(end==value.npos) return {};
    value.resize(end+1);return value;
}
std::filesystem::path relative_path(std::string value) {
    std::ranges::replace(value,'\\','/');
    if(value.size()>1 && value.front()=='"' && value.back()=='"') value=value.substr(1,value.size()-2);
    auto path=std::filesystem::path(wide(value));
    require(!path.empty() && !path.has_root_path(),"OBJ dependency must be relative.");
    for(const auto& part:path) require(part!=L".." && part.wstring().find(L':')==std::wstring::npos,
        "OBJ dependency must stay inside the source directory.");
    return path.lexically_normal();
}
std::size_t index(std::string_view text,std::size_t count) {
    std::int64_t value{};auto [end,ec]=std::from_chars(text.data(),text.data()+text.size(),value);
    require(ec==std::errc{} && end==text.data()+text.size() && value!=0,"Invalid OBJ index.");
    value=value>0?value-1:static_cast<std::int64_t>(count)+value;
    require(value>=0 && std::uint64_t(value)<count,"OBJ index is out of range.");return static_cast<std::size_t>(value);
}
float scalar(std::istringstream& row) {
    float v{};require(bool(row>>v) && std::isfinite(v),"Invalid OBJ/MTL number.");return v;
}
std::vector<std::array<unsigned,3>> triangulate(const std::vector<geometry::Vertex>& face) {
    if(face.size()==3) return {{0,1,2}};
    std::array<double,3> normal{};
    for(std::size_t i=0;i<face.size();++i) {
        const auto& a=face[i].position;const auto& b=face[(i+1)%face.size()].position;
        normal[0]+=(double(a[1])-b[1])*(double(a[2])+b[2]);
        normal[1]+=(double(a[2])-b[2])*(double(a[0])+b[0]);
        normal[2]+=(double(a[0])-b[0])*(double(a[1])+b[1]);
    }
    unsigned axis=0;for(unsigned i=1;i<3;++i) if(std::abs(normal[i])>std::abs(normal[axis])) axis=i;
    const unsigned x=(axis+1)%3,y=(axis+2)%3;const double sign=normal[axis]<0?-1:1;
    const auto cross=[&](unsigned a,unsigned b,unsigned c) {
        return sign*((double(face[b].position[x])-face[a].position[x])*(double(face[c].position[y])-face[a].position[y])-
            (double(face[b].position[y])-face[a].position[y])*(double(face[c].position[x])-face[a].position[x]));
    };
    std::vector<unsigned> remaining;for(unsigned i=0;i<face.size();++i) remaining.push_back(i);
    std::vector<std::array<unsigned,3>> triangles;
    while(remaining.size()>3) {
        bool found=false;
        for(std::size_t i=0;i<remaining.size();++i) {
            const auto a=remaining[(i+remaining.size()-1)%remaining.size()],b=remaining[i],c=remaining[(i+1)%remaining.size()];
            if(cross(a,b,c)<0) continue;
            bool inside=false;
            for(auto p:remaining) if(p!=a && p!=b && p!=c && cross(a,b,c)>0 &&
                cross(a,b,p)>=0 && cross(b,c,p)>=0 && cross(c,a,p)>=0) {inside=true;break;}
            if(inside) continue;
            triangles.push_back({a,b,c});remaining.erase(remaining.begin()+static_cast<std::ptrdiff_t>(i));found=true;break;
        }
        require(found,"OBJ polygon cannot be triangulated.");
    }
    triangles.push_back({remaining[0],remaining[1],remaining[2]});return triangles;
}
}
MeshImport load_obj(const std::filesystem::path& source,ImageCache* shared_images) {
    MeshImport result;
    const auto root=std::filesystem::canonical(source.parent_path());
    std::unordered_map<std::string,std::size_t> files;
    std::unordered_map<std::string,material::ImageRef> images;
    std::size_t total_bytes{},image_bytes{};
    const auto read=[&](const std::filesystem::path& relative)->std::size_t {
        const auto key=utf8(relative.generic_wstring());
        if(const auto found=files.find(key);found!=files.end()) return found->second;
        require(files.size()<512,"OBJ has too many dependencies.");
        const auto path=std::filesystem::weakly_canonical(root/relative);
        const auto within=path.lexically_relative(root);
        require(!within.empty() && !within.has_root_path() && *within.begin()!=L"..","OBJ dependency escapes through a link.");
        auto bytes=read_file(path,128*1024*1024);if(!bytes) throw std::runtime_error(bytes.error().message);
        require(total_bytes+bytes->size()<=512ull*1024*1024,"OBJ dependency bytes exceed 512 MiB.");total_bytes+=bytes->size();
        const auto slot=result.files.size();files.emplace(key,slot);result.files.push_back({relative,std::move(*bytes)});return slot;
    };
    const auto image=[&](const std::filesystem::path& relative) {
        const auto key=utf8(relative.generic_wstring());
        if(const auto found=images.find(key);found!=images.end()) return found->second;
        const auto slot=read(relative);const std::string_view bytes=result.files[slot].bytes;
        auto loaded=decode_images(std::span(&bytes,1),shared_images).front();
        require(image_bytes+loaded->pixels().size()<=768ull*1024*1024,"Decoded OBJ textures exceed 768 MiB.");
        image_bytes+=loaded->pixels().size();images.emplace(key,loaded);return loaded;
    };
    std::vector<material::Slot> materials(1);
    std::vector<material::Description> descriptions(1);
    std::unordered_map<std::string,std::uint32_t> material_names;
    const auto mtl=[&](const std::filesystem::path& relative) {
        const auto slot=read(relative);std::istringstream input(result.files[slot].bytes);input.imbue(std::locale::classic());
        std::string line,kind;material::Description* current=nullptr;
        while(std::getline(input,line)) {
            if(auto p=line.find('#');p!=line.npos) line.resize(p);
            std::istringstream row(line);row.imbue(std::locale::classic());if(!(row>>kind)) continue;
            if(kind=="newmtl") {
                auto name=rest(row);require(!name.empty() && material_names.size()<1024,"Invalid or excessive MTL material names.");
                require(material_names.emplace(name,static_cast<std::uint32_t>(materials.size())).second,"Duplicate MTL material name.");
                materials.push_back({std::move(name)});descriptions.emplace_back();current=&descriptions.back();
            } else {
                require(current!=nullptr,"MTL property precedes newmtl.");
                if(kind=="Kd") for(unsigned i=0;i<3;++i) current->parameters.base_color[i]=scalar(row);
                else if(kind=="d" || kind=="Tr") {const auto v=scalar(row);current->parameters.base_color[3]=kind=="d"?v:1-v;}
                else if(kind.starts_with("map_") || kind=="bump") {
                    auto name=rest(row);require(!name.empty() && name.front()!='-',"MTL map options are not supported.");
                    const auto path=relative.parent_path()/relative_path(name);
                    if(kind=="map_Kd") current->texture(material::Input::base_color).image=image(path);
                    else if(kind=="map_d") current->texture(material::Input::opacity).image=image(path);
                    else static_cast<void>(read(path)); // Preserve unrendered source maps during import.
                } else if(kind!="Ka" && kind!="Ks" && kind!="Ke" && kind!="Ns" && kind!="Ni" && kind!="Tf" && kind!="illum")
                    throw std::runtime_error("Unsupported MTL record: "+kind);
            }
        }
    };
    const auto source_slot=read(relative_path(utf8(source.filename().wstring())));
    std::istringstream input(result.files[source_slot].bytes);input.imbue(std::locale::classic());
    std::vector<std::array<float,3>> positions,normals;
    std::vector<std::array<float,2>> uv;
    std::vector<geometry::Vertex> vertices;std::vector<std::uint8_t> masks;std::vector<geometry::MeshPart> parts;
    std::uint32_t material{};std::string line,kind;
    while(std::getline(input,line)) {
        if(auto p=line.find('#');p!=line.npos) line.resize(p);
        std::istringstream row(line);row.imbue(std::locale::classic());if(!(row>>kind)) continue;
        if(kind=="v" || kind=="vn") {
            auto& target=kind=="v"?positions:normals;require(target.size()<1000000,"OBJ exceeds the vertex limit.");
            target.push_back({scalar(row),scalar(row),scalar(row)});
        } else if(kind=="vt") {
            require(uv.size()<1000000,"OBJ exceeds the UV limit.");uv.push_back({scalar(row),1-scalar(row)});
        } else if(kind=="mtllib") mtl(relative_path(rest(row)));
        else if(kind=="usemtl") {
            const auto found=material_names.find(rest(row));require(found!=material_names.end(),"OBJ references an unknown material.");material=found->second;
        } else if(kind=="f") {
            std::vector<geometry::Vertex> face;std::vector<bool> has_normal;std::string token;
            while(row>>token) {
                require(face.size()<64,"OBJ polygon exceeds 64 corners.");geometry::Vertex vertex{};
                const auto a=token.find('/'),b=a==token.npos?token.npos:token.find('/',a+1);
                std::ranges::copy(positions.at(index(std::string_view(token).substr(0,a),positions.size())),vertex.position);
                if(a!=token.npos && a+1<(b==token.npos?token.size():b))
                    std::ranges::copy(uv.at(index(std::string_view(token).substr(a+1,b==token.npos?token.npos:b-a-1),uv.size())),vertex.uv);
                else require(std::ranges::none_of(descriptions[material].textures,[](const auto& t){return bool(t.image);}),"Textured OBJ face requires UV indices.");
                const bool normal=b!=token.npos && b+1<token.size();
                if(normal) std::ranges::copy(normals.at(index(std::string_view(token).substr(b+1),normals.size())),vertex.normal);
                face.push_back(vertex);has_normal.push_back(normal);
            }
            require(face.size()>=3,"OBJ face requires at least three corners.");
            const auto triangles=triangulate(face);
            require(vertices.size()+triangles.size()*3<=3000000,"OBJ exceeds the triangle limit.");
            if(parts.empty() || parts.back().material!=material) parts.push_back({static_cast<std::uint32_t>(vertices.size()),0,material});
            for(const auto& t:triangles) {
                std::array<geometry::Vertex,3> triangle{face[t[0]],face[t[1]],face[t[2]]};
                std::array<double,3> a{},b{},n{};
                for(unsigned i=0;i<3;++i){a[i]=double(triangle[1].position[i])-triangle[0].position[i];b[i]=double(triangle[2].position[i])-triangle[0].position[i];}
                n={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};const auto length=std::hypot(n[0],n[1],n[2]);
                std::uint8_t mask=0;
                for(unsigned i=0;i<3;++i) {
                    if(!has_normal[t[i]]) {require(length>0,"Degenerate OBJ face needs explicit normals.");for(unsigned j=0;j<3;++j) triangle[i].normal[j]=static_cast<float>(n[j]/length);}
                    const auto j=t[(i+1)%3],k=t[(i+2)%3];if((j+1)%face.size()==k || (k+1)%face.size()==j) mask|=static_cast<std::uint8_t>(1u<<i);
                }
                vertices.insert(vertices.end(),triangle.begin(),triangle.end());masks.push_back(mask);parts.back().vertex_count+=3;
            }
        } else if(kind!="o" && kind!="g" && kind!="s") throw std::runtime_error("Unsupported OBJ record: "+kind);
    }
    for(std::size_t i=0;i<materials.size();++i) {
        auto& value=descriptions[i];
        const auto& color=value.texture(material::Input::base_color).image;
        bool alpha=value.parameters.base_color[3]<1 || bool(value.texture(material::Input::opacity).image);
        if(color)alpha|=color->has_alpha();
        if(alpha)value.parameters.coverage=material::Coverage::mask;
        materials[i].surface=std::make_shared<const material::Surface>(std::move(value));
    }
    result.mesh=std::make_shared<const geometry::Mesh>(std::move(vertices),std::move(masks),std::move(materials),std::move(parts));
    return result;
}
}
