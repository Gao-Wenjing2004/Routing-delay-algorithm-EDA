#pragma once
#include "binding_contract.hpp"
#include "adaptive_d72.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>

namespace srb_v11_short48 {
class Table {
public:
    using Parent=srb_v11_adaptive_d72::Table;
    struct Prepared {
        const uint16_t* value=nullptr;
        const Table* owner=nullptr;
        int16_t phi=0;
        uint8_t basis=255;
        Status status=Status::Unprepared;
    };
    // Borrow the complete parent; no duplicate Short/Lower/Long/model allocation.
    Table(const Parent& parent,const std::string& capsule,const std::string& pack,
          const srb_v11_adaptive_d72::Binding& parent_binding,const Binding& binding)
        :parent_(parent),binding_(binding) {
        const uint16_t endian=1;
        require(*reinterpret_cast<const uint8_t*>(&endian)==1,"little-endian host required");
        validate_binding(parent_binding);
        require(std::filesystem::file_size(capsule)==binding_.capsule_bytes &&
            srb_v11_adaptive_a::sha256_file(capsule)==binding_.capsule_sha256,"capsule exact bytes/SHA");
        Parameters parameters=read_parameters(pack,parent_binding);
        make_phi(parameters);
        std::ifstream stream(capsule,std::ios::binary);require(bool(stream),"capsule open");
        auto prefix=read(stream,16);
        require(!std::memcmp(prefix.data(),"SRBSA4F1",8) && u32(prefix.data()+8)==1 &&
                u32(prefix.data()+12)==binding_.header_bytes,"capsule prefix/version/header length");
        auto header=read(stream,binding_.header_bytes);
        require(digest(header)==binding_.header_sha256,"exact Root-validated JSON header SHA");
        make_cell_map();
        new_values_.assign(size_t(Sources)*DomainCells*Added,65535);
        for(unsigned source=0;source<Sources;++source)load_frame(stream,source,parameters);
        auto compressed=read(stream,binding_.map_bytes);
        require(digest(compressed)==binding_.map_sha256,"choice compressed SHA");
        choice_=xz(compressed,ChoiceFields);
        require(checksum(choice_.data(),choice_.size())==binding_.map_crc &&
                digest(choice_)==binding_.map_raw_sha256 &&
                std::all_of(choice_.begin(),choice_.end(),[](uint8_t v){return v<Basis;}),"choice exact raw product/CRC/SHA/range");
        require(stream.peek()==EOF && !stream.bad(),"capsule exact final length, no trailing bytes");
        // P/N/C4 are load-local. Old32 phi stays borrowed from the same parent.
    }
    bool basis_cost_raw(unsigned source_row,unsigned cell,unsigned basis,BasisRaw& out)const {
        out=BasisRaw{};
        if(source_row>=Sources||cell>=Cells||basis>=Basis||!domain(cell))return false;
        const uint16_t value=basis<Old ? parent_.legacy().medoid_value(source_row,cell,basis)
            :new_values_[(uint64_t(source_row)*DomainCells+cell_map_[cell])*Added+basis-Old];
        if(value==65535)return false;
        out.value=value;return true;
    }
    bool prepare_selected(unsigned source,unsigned target,int dx,int dy,bool guarded_original_A,Prepared& out)const {
        out=Prepared{};
        if(!guarded_original_A){out.status=Status::NotEligible;return false;}
        if(dx<-72||dx>72||dy<-72||dy>72){out.status=Status::OutsideDomain;return false;}
        const unsigned cell=unsigned(dy+72)*Side+unsigned(dx+72);
        if(!domain(cell)){out.status=Status::OutsideDomain;return false;}
        if(source>=496||target>=496||source_row_[source]<0||target_row_[target]<0){out.status=Status::UnknownPort;return false;}
        srb_v11_short_k32::Table::PreparedQuery old{};
        if(!parent_.legacy().prepare(source,target,dx,dy,old)||*old.value==65535){out.status=Status::ParentMissing;return false;}
        const unsigned sr=unsigned(source_row_[source]),tr=unsigned(target_row_[target]);
        const unsigned quad=srb_v11_adaptive_d72::quadrant(dx,dy),phase=srb_v11_adaptive_d72::phase(dx,dy);
        const unsigned chosen=choice_[(sr*4+quad)*Targets+tr]; // One basis lookup; no candidate loop.
        const uint16_t* value=chosen<Old ? old.value-parent_.legacy().assignment()[tr]+chosen
            :new_values_.data()+(uint64_t(sr)*DomainCells+cell_map_[cell])*Added+chosen-Old;
        if(*value==65535){out.status=Status::ChosenMissing;return false;}
        out.value=value;out.owner=this;out.basis=uint8_t(chosen);
        out.phi=phi_at(chosen,quad*120+phase,tr);out.status=Status::Ready;return true;
    }
    bool selected_basis_raw(const Prepared& query,BasisRaw& out)const {
        out=BasisRaw{};
        if(query.owner!=this||query.status!=Status::Ready||!query.value||*query.value==65535)return false;
        out.value=*query.value;return true;
    }
    bool projected_teacher_raw(const Prepared& query,ProjectedRaw& out)const {
        out=ProjectedRaw{};BasisRaw cost;
        if(!selected_basis_raw(query,cost))return false;
        out.value=uint32_t(std::max<int64_t>(0,int64_t(cost.value)+query.phi));return true;
    }
    uint8_t choice_at(unsigned sr,unsigned quad,unsigned target_row)const {
        require(sr<Sources&&quad<4&&target_row<Targets,"audit choice range");
        return choice_[(sr*4+quad)*Targets+target_row];
    }
    int16_t phi_at(unsigned basis,unsigned group,unsigned target_row)const {
        require(basis<Basis&&group<Groups&&target_row<Targets,"audit phi range");
        return basis<Old ? parent_.adaptive_bias()[(basis*Groups+group)*Targets+target_row]
            :new_phi_[((basis-Old)*Groups+group)*Targets+target_row];
    }
    const Parent& parent()const{return parent_;}
    size_t persistent_new16_cost_bytes()const{return new_values_.size()*sizeof(uint16_t);}
    size_t persistent_new16_phi_bytes()const{return new_phi_.size()*sizeof(int16_t);}
    size_t persistent_cellmap_bytes()const{return sizeof(cell_map_);}
    size_t owned_payload_bytes()const{return persistent_new16_cost_bytes()+persistent_new16_phi_bytes()+choice_.size()+persistent_cellmap_bytes()+sizeof(source_row_)+sizeof(target_row_);}
    static bool domain(unsigned cell){
        const int x=int(cell%Side)-72,y=int(cell/Side)-72;
        return std::abs(x)+std::abs(y)<=72&&(std::abs(x)>26||std::abs(y)>26);
    }
private:
    struct Parameters{std::vector<int32_t> p;std::array<uint16_t,Groups> n{};std::vector<uint8_t> c4;};
    static_assert(DomainCells<65535,"compact u16 cell index leaves missing sentinel");
    static_assert(uint64_t(Sources)*DomainCells*Added*2==27118080ull,"exact compact new16 payload");
    static_assert(uint64_t(Added)*Groups*Targets*2==3932160ull,"only new16 phi is owned");
    static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
    static uint32_t u32(const uint8_t* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
    static uint32_t checksum(const void* data,size_t bytes){return uint32_t(crc32(0,static_cast<const Bytef*>(data),uInt(bytes)));}
    static std::string digest(const std::vector<uint8_t>& data){srb_v11_adaptive_a::Sha256 h;h.update(data.data(),data.size());return h.finish();}
    static bool sha_format(const std::string& value){return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
    static std::vector<uint8_t> read(std::istream& stream,size_t bytes){
        require(bytes<=8000000,"bounded component before allocation");std::vector<uint8_t> out(bytes);
        stream.read(reinterpret_cast<char*>(out.data()),std::streamsize(bytes));require(bool(stream),"truncated component");return out;
    }
    static std::vector<uint8_t> xz(const std::vector<uint8_t>& in,size_t bytes){
        require(!in.empty()&&bytes<=8000000&&in.size()<=bytes+1048576,"XZ bounded exact product");
        std::vector<uint8_t> out(bytes);uint64_t memory=64ull<<20;size_t ip=0,op=0;
        require(lzma_stream_buffer_decode(&memory,0,nullptr,in.data(),&ip,in.size(),out.data(),&op,out.size())==LZMA_OK&&
            ip==in.size()&&op==out.size(),"one XZ stream, exact output, memory and no trailing bytes");return out;
    }
    static std::vector<uint8_t> zlib_exact(const uint8_t* in,size_t length,size_t bytes){
        // One guard byte lets inflate consume the end marker even at an exact output boundary.
        std::vector<uint8_t> out(bytes+1);z_stream z{};require(inflateInit(&z)==Z_OK,"zlib init");
        z.next_in=const_cast<Bytef*>(in);z.avail_in=uInt(length);z.next_out=out.data();z.avail_out=uInt(out.size());
        const int code=inflate(&z,Z_FINISH);const bool ok=code==Z_STREAM_END&&z.total_in==length&&z.total_out==bytes&&z.avail_in==0;
        inflateEnd(&z);require(ok,"one zlib stream, exact output and no trailing bytes");out.resize(bytes);return out;
    }
    void validate_binding(const srb_v11_adaptive_d72::Binding& old){
        require(binding_.verified_new_fields==NewFields&&binding_.verified_assembled_fields==AssembledFields&&
            binding_.verified_old_fields==OldFields&&binding_.verified_phi_fields==PhiFields&&
            binding_.verified_fit_fields==LossFields&&binding_.verified_choices==ChoiceFields,"completed exact full110 fresh products");
        for(const auto* text:{&binding_.capsule_sha256,&binding_.header_sha256,&binding_.map_sha256,&binding_.map_raw_sha256,
            &binding_.pack_sha256,&binding_.completed_full110_sha256,&binding_.fresh_sha256,&binding_.generation_frozen_sha256,
            &binding_.fresh_resource_sha256,&binding_.parent_build_sha256,&binding_.parent_raw_proof_sha256})require(sha_format(*text),"typed proof SHA format");
        require(binding_.header_bytes>0&&binding_.header_bytes<=1048576&&binding_.map_bytes>0&&binding_.map_bytes<=1161216,
            "typed bounded header/map");
        require(old.graph_signature==Graph&&binding_.sources==old.sources&&binding_.targets==old.targets&&
            binding_.old_portIDs==old.medoids&&binding_.old_assignment==old.old_assignment,"same complete parent ordered maps");
        require(parent_.legacy().header().sources==Sources&&parent_.legacy().header().targets==Targets&&
            parent_.legacy().header().prototypes==Old&&parent_.legacy().header().signature==Graph&&
            parent_.adaptive_bias().size()==size_t(Old)*Groups*Targets,"complete actual borrowed parent dimensions");
        require(std::equal(binding_.sources.begin(),binding_.sources.end(),parent_.legacy().sources().begin())&&
            std::equal(binding_.targets.begin(),binding_.targets.end(),parent_.legacy().targets().begin())&&
            std::equal(binding_.old_portIDs.begin(),binding_.old_portIDs.end(),parent_.legacy().medoids().begin()),"actual borrowed parent maps");
        source_row_.fill(-1);target_row_.fill(-1);
        for(unsigned sr=0;sr<Sources;++sr){const unsigned id=binding_.sources[sr];require(id<496&&source_row_[id]<0,"unique source IDs");source_row_[id]=int16_t(sr);}
        for(unsigned tr=0;tr<Targets;++tr){const unsigned id=binding_.targets[tr];require(id<496&&target_row_[id]<0,"unique target IDs");target_row_[id]=int16_t(tr);}
        std::array<bool,Targets> seen{};uint64_t bytes=16+binding_.header_bytes+binding_.map_bytes;
        for(unsigned m=0;m<Basis;++m){unsigned tr=binding_.basis_target_rows[m];require(tr<Targets&&!seen[tr]&&binding_.targets[tr]==binding_.basis_portIDs[m],"unique basis rows/real IDs");seen[tr]=true;
            require(m<Old?binding_.basis_portIDs[m]==binding_.old_portIDs[m]:binding_.basis_portIDs[m]==AddedIDs[m-Old],"old32 original order plus fixed16");}
        for(const auto& frame:binding_.frames){
            require(frame.xz_bytes>0&&frame.xz_bytes<=1721376&&frame.bitmap_bytes>0&&frame.bitmap_bytes<=1090626&&
                frame.bytes==16+uint64_t(frame.xz_bytes)+frame.bitmap_bytes&&frame.finite<=DomainCells*Added&&
                sha_format(frame.sha256)&&sha_format(frame.authoritative_npy_sha256),"typed bounded frame descriptor");bytes+=frame.bytes;
        }
        require(bytes==binding_.capsule_bytes&&bytes<100000000,"exact aggregate capsule length");
    }
    Parameters read_parameters(const std::string& path,const srb_v11_adaptive_d72::Binding& expected)const{
        require(srb_v11_adaptive_a::sha256_file(path)==binding_.pack_sha256,"actual original C4/P/N pack SHA");
        std::ifstream stream(path,std::ios::binary);require(bool(stream),"parent pack open");
        auto bytes=read(stream,120);srb_v11_adaptive_d72::PackHeader h{};std::memcpy(&h,bytes.data(),120);
        require(!std::memcmp(h.magic,"SRBAA001",8)&&h.version==1&&h.ports==496&&h.sources==Sources&&h.targets==Targets&&
            h.medoids==Old&&h.quadrants==4&&h.phases==120&&h.radius==72&&h.search==80&&h.cells==Cells&&h.geometry==76&&
            h.streams==3&&h.graph_signature==Graph&&srb_v11_adaptive_d72::digest_hex(h.d76_sha256)==expected.d76_asset_sha256&&
            h.old_bias_crc==expected.old_bias_crc&&h.expanded_bias_crc==expected.expanded_bias_crc,"actual original parameter pack header");
        auto maps=read(stream,1052);require(checksum(maps.data(),maps.size())==h.maps_crc,"pack maps CRC");size_t at=0;
        for(auto id:binding_.sources){require(srb_v11_adaptive_d72::read_u16_le(maps.data()+at)==id,"pack source order");at+=2;}
        for(auto id:binding_.targets){require(srb_v11_adaptive_d72::read_u16_le(maps.data()+at)==id,"pack target order");at+=2;}
        for(auto id:binding_.old_portIDs){require(srb_v11_adaptive_d72::read_u16_le(maps.data()+at)==id,"pack old32 order");at+=2;}
        for(auto id:binding_.old_assignment)require(maps[at++]==id,"pack oldproto assignment");
        require(h.potential_bytes<=1540096&&h.count_bytes<=1050576&&h.assignment_bytes<=1161660,"bounded original parameter containers");
        const auto p=srb_v11_adaptive_d72::decode_container_blob(read(stream,h.potential_bytes),3,{4,120,256},4,expected.potential_crc);
        const auto n=srb_v11_adaptive_d72::decode_container_blob(read(stream,h.count_bytes),4,{4,120},2,expected.count_crc);
        Parameters result;result.c4=srb_v11_adaptive_d72::decode_container_blob(read(stream,h.assignment_bytes),2,{110,4,256},1,expected.assignment_crc);
        require(stream.peek()==EOF&&!stream.bad(),"original pack exact length");
        result.p.resize(Groups*Targets);
        for(size_t i=0;i<result.p.size();++i)result.p[i]=srb_v11_adaptive_d72::read_i32_le(p.data()+4*i);
        for(unsigned group=0;group<Groups;++group){result.n[group]=srb_v11_adaptive_d72::read_u16_le(n.data()+2*group);require(result.n[group]>=2,"all480 original N>=2, no beta substitute");}
        require(result.c4==parent_.adaptive_assignment()&&std::all_of(result.c4.begin(),result.c4.end(),[](uint8_t v){return v<Old;}),"actual borrowed old C4 exact");return result;
    }
    void make_cell_map(){
        cell_map_.fill(65535);unsigned count=0;
        for(unsigned cell=0;cell<Cells;++cell)if(domain(cell))cell_map_[cell]=uint16_t(count++);
        require(count==DomainCells,"exact compact outside26 geometry product");
    }
    void make_phi(const Parameters& parameters){
        new_phi_.resize(size_t(Added)*Groups*Targets);
        for(unsigned m=0;m<Basis;++m)for(unsigned group=0;group<Groups;++group)for(unsigned target=0;target<Targets;++target){
            const int64_t numerator=int64_t(parameters.p[group*Targets+target])-parameters.p[group*Targets+binding_.basis_target_rows[m]];
            const int64_t value=srb_v11_adaptive_d72::round_even(numerator,parameters.n[group]);
            require(value>=-32768&&value<=32767,"full selected48 signed16 phi guard");
            if(m<Old)require(parent_.adaptive_bias()[(m*Groups+group)*Targets+target]==value,"recomputed old32 phi equals borrowed parent");
            else new_phi_[((m-Old)*Groups+group)*Targets+target]=int16_t(value);
        }
        const auto& old_phi=parent_.adaptive_bias();const uint32_t old_crc=checksum(old_phi.data(),old_phi.size()*2);
        const uint32_t all_crc=uint32_t(crc32(old_crc,reinterpret_cast<const Bytef*>(new_phi_.data()),uInt(new_phi_.size()*2)));
        require(old_crc==binding_.old_phi_crc&&all_crc==binding_.phi_crc,"all48 concatenated phi CRC and complete borrowed old32 order/values");
    }
    void load_frame(std::istream& stream,unsigned sr,const Parameters& parameters){
        const auto& expected=binding_.frames[sr];auto frame=read(stream,expected.bytes);
        require(digest(frame)==expected.sha256&&u32(frame.data())==expected.xz_bytes&&u32(frame.data()+4)==expected.bitmap_bytes&&
            u32(frame.data()+8)==expected.new_crc&&u32(frame.data()+12)==expected.finite,"source frame prefix/SHA binding");
        const size_t count=size_t(Cells)*Added;
        std::vector<uint8_t> compressed(frame.begin()+16,frame.begin()+16+expected.xz_bytes);
        const auto planes=xz(compressed,count*2);const auto bits=zlib_exact(frame.data()+16+expected.xz_bytes,expected.bitmap_bytes,42050);
        std::vector<uint16_t> residual(count),base(count);std::vector<int16_t> used(count);std::vector<uint8_t> selected(count);
        for(size_t i=0;i<count;++i){const unsigned z=planes[i]|unsigned(planes[count+i])<<8;residual[i]=uint16_t((z>>1)^unsigned(-int(z&1)));}
        // Preserve the original dense per-source CRC order, never all110 dense.
        std::vector<uint16_t> dense_source(count,65535);uint16_t* out=dense_source.data();
        unsigned finite=0;int low=32768,high=-32769;
        uint32_t assembled_crc=0,shared_crc=0,old_crc=0;
        for(unsigned cell=0;cell<Cells;++cell){
            const int x=int(cell%Side)-72,y=int(cell/Side)-72;const unsigned quad=srb_v11_adaptive_d72::quadrant(x,y),phase=srb_v11_adaptive_d72::phase(x,y),group=quad*120+phase;
            std::array<uint16_t,Basis> assembled{};std::array<uint16_t,Old> actual_old{},shared{};
            for(unsigned m=0;m<Old;++m){actual_old[m]=parent_.legacy().medoid_value(sr,cell,m);shared[m]=domain(cell)?actual_old[m]:65535;assembled[m]=shared[m];}
            for(unsigned column=0;column<Added;++column){const size_t i=size_t(cell)*Added+column;
                uint32_t word=residual[i];if(cell%Side>=10)word+=residual[i-10*Added];if(cell>=Side)word+=residual[i-Side*Added];
                if(cell>=Side&&cell%Side>=10)word-=residual[i-(Side+10)*Added];residual[i]=uint16_t(word);
                const unsigned target=binding_.basis_target_rows[Old+column],m=parameters.c4[(sr*4+quad)*Targets+target];
                base[i]=actual_old[m];selected[i]=uint8_t(m);used[i]=parent_.adaptive_bias()[(m*Groups+group)*Targets+target];
                const bool present=(bits[i>>3]>>(i&7))&1;
                if(!present){require(residual[i]==0,"missing/outside residual must be zero");assembled[Old+column]=65535;continue;}
                require(domain(cell)&&base[i]!=65535,"finite new16 requires fixed old C4 raw base and exact domain");
                const int r=residual[i]>=32768?int(residual[i])-65536:int(residual[i]);low=std::min(low,r);high=std::max(high,r);
                const int64_t predicted=std::max<int64_t>(0,int64_t(base[i])+used[i]);const int64_t value=predicted+r;
                require(value>=0&&value<65535,"restored exact newmedoid uint16 range");out[i]=uint16_t(value);assembled[Old+column]=out[i];++finite;
            }
            assembled_crc=uint32_t(crc32(assembled_crc,reinterpret_cast<const Bytef*>(assembled.data()),Basis*2));
            shared_crc=uint32_t(crc32(shared_crc,reinterpret_cast<const Bytef*>(shared.data()),Old*2));
            old_crc=uint32_t(crc32(old_crc,reinterpret_cast<const Bytef*>(actual_old.data()),Old*2));
        }
        require(finite==expected.finite&&(!finite||(low==expected.residual_min&&high==expected.residual_max))&&
            checksum(out,count*2)==expected.new_crc&&assembled_crc==expected.assembled_crc&&shared_crc==expected.shared_old_crc&&
            old_crc==expected.old_full_crc&&checksum(base.data(),count*2)==expected.base_crc&&
            checksum(used.data(),count*2)==expected.storage_phi_crc&&checksum(selected.data(),count)==expected.storage_choice_crc,
            "complete source cost/mask/old32/assembled48/storage predictor CRC and signed16 extrema");
        // Commit only after every original full-canvas/source CRC has passed.
        for(unsigned cell=0;cell<Cells;++cell)if(cell_map_[cell]!=65535){
            uint16_t* destination=new_values_.data()+(uint64_t(sr)*DomainCells+cell_map_[cell])*Added;
            std::copy_n(out+size_t(cell)*Added,Added,destination);
        }
    }
    const Parent& parent_;
    const Binding binding_;
    std::array<int16_t,496> source_row_{},target_row_{};
    std::array<uint16_t,Cells> cell_map_{};
    std::vector<uint16_t> new_values_;
    std::vector<int16_t> new_phi_;
    std::vector<uint8_t> choice_;
};
}
