#pragma once
#ifndef SRB_FAST_HEADER
#define SRB_FAST_HEADER "srb_fast.hpp"
#endif
#include SRB_FAST_HEADER
#include "local_arch_data.hpp"
#include "e2e_format.hpp"
#ifdef SRB_EXACT_LOCAL_ROUTE_TABLE
#include "exact_local_route_table.hpp"
#ifndef _WIN32
extern "C" {
extern const unsigned char srb_exact_local_begin[];
extern const unsigned char srb_exact_local_end[];
}
#endif
#endif

#ifndef SRB_HOT_INNER_RADIUS
#define SRB_HOT_INNER_RADIUS 24
#endif
#ifdef SRB_EMBEDDED_ATLAS
#include "v15_dict.hpp"
#endif
#ifdef SRB_PERIOD_GATE
#ifdef SRB_GAP_GATE
#include "gap_gate_data.hpp"
#else
#include "period_gate_data.hpp"
#endif
#endif
#ifdef SRB_HIERARCHICAL_SELECTOR
#include "v54_selector_data.hpp"
#endif
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>

#ifndef SRB_EXTENSION_RADIUS
#define SRB_EXTENSION_RADIUS 104
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace srb_v3 {
class HybridEstimator {
#pragma pack(push,1)
 struct EndpointException{uint32_t index;uint16_t value;};
 struct EndpointExceptionHeader{char magic[8];uint32_t version,base_count,hot_count,reserved;};
#pragma pack(pop)
public:
 HybridEstimator(const std::string&path,int radius=72,unsigned=0,int=-1):radius_(radius){
  std::fill(src_,src_+496,-1);std::fill(dst_,dst_+496,-1);std::fill(hsrc_,hsrc_+496,-1);std::fill(hdst_,hdst_+496,-1);
  if(!radius){
#ifdef SRB_ROUTE_TAIL
#ifndef SRB_ROUTE_TAIL_PATH
#define SRB_ROUTE_TAIL_PATH "route_tail.bin"
#endif
   load_route_tail(SRB_ROUTE_TAIL_PATH);
#endif
   return;
  }
#ifdef _WIN32
  file_=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file_==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot open endpoint Atlas");
  LARGE_INTEGER size;GetFileSizeEx(file_,&size);bytes_=size.QuadPart;
  map_=CreateFileMappingA(file_,nullptr,PAGE_READONLY,0,0,nullptr);if(!map_)throw std::runtime_error("cannot create Atlas mapping");
  data_=static_cast<const char*>(MapViewOfFile(map_,FILE_MAP_READ,0,0,0));
#else
  uint64_t offset=0;
#ifdef SRB_EMBEDDED_ATLAS
  int fd=open("/proc/self/exe",O_RDONLY|O_CLOEXEC);
#else
  int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);
#endif
  struct stat st{};
  if(fd<0||fstat(fd,&st)){if(fd>=0)close(fd);throw std::runtime_error("cannot open endpoint Atlas");}bytes_=st.st_size;
#ifdef SRB_EMBEDDED_ATLAS
  srb_v15::AssetFooter footer{};
  static_assert(sizeof(footer)==48,"footer layout");
  if(bytes_<sizeof(footer)||pread(fd,&footer,sizeof(footer),bytes_-sizeof(footer))!=sizeof(footer)
     ||std::memcmp(footer.magic,"SRBV15ASSET2026",15)||footer.atlas_offset%4096
     ||footer.atlas_offset>uint64_t(bytes_)-sizeof(footer)
     ||footer.atlas_offset+footer.atlas_bytes>footer.dict_offset){
   close(fd);throw std::runtime_error("invalid embedded endpoint Atlas footer");
  }
  offset=footer.atlas_offset;bytes_=footer.atlas_bytes;
#endif
  void*mapped=mmap(nullptr,bytes_,PROT_READ,MAP_PRIVATE
#ifdef SRB_PREFAULT_ATLAS
       |MAP_POPULATE
#endif
       ,fd,offset);close(fd);
  data_=mapped==MAP_FAILED?nullptr:static_cast<const char*>(mapped);
#endif
  if(!data_||bytes_<64)throw std::runtime_error("cannot map endpoint Atlas");
  std::memcpy(&h_,data_,64);
  if(std::memcmp(h_.magic,"SRBE2E1\0",8)||(h_.version<1||h_.version>4)||h_.ports!=496||h_.base_radius!=24||h_.hot_radius!=72
#ifdef SRB_ATLAS_NO_BASE
     ||h_.sources||h_.targets
#else
     ||!h_.sources||h_.sources>496||!h_.targets||h_.targets>496
#endif
     ||!h_.hot_sources||h_.hot_sources>496||!h_.hot_targets||h_.hot_targets>496
     ||h_.hot_cells!=145*145-(2*SRB_HOT_INNER_RADIUS+1)*(2*SRB_HOT_INNER_RADIUS+1)
     ||h_.base_values!=uint64_t(h_.sources)*49*49*h_.targets
     ||h_.hot_values!=uint64_t(h_.hot_sources)*h_.hot_cells*h_.hot_targets)throw std::runtime_error("invalid endpoint Atlas");
  uint64_t prefix=64+2ull*(h_.sources+h_.targets+h_.hot_sources+h_.hot_targets);
  if(h_.version==2){
   bits_=h_.reserved;if(bits_<1||bits_>15||h_.base_values%16||h_.hot_values%16)throw std::runtime_error("bad packed layout");
   record_bytes_=2+2*bits_;mask_=(1u<<bits_)-1;base_bits_=hot_bits_=bits_;base_record_bytes_=hot_record_bytes_=record_bytes_;base_mask_=hot_mask_=mask_;
  }else if(h_.version==4){
   base_bits_=h_.reserved&255;hot_bits_=(h_.reserved>>8)&255;if(base_bits_<1||base_bits_>15||hot_bits_<1||hot_bits_>15||h_.base_values%16||h_.hot_values%16)throw std::runtime_error("bad hybrid packed layout");base_record_bytes_=2+2*base_bits_;hot_record_bytes_=2+2*hot_bits_;base_mask_=(1u<<base_bits_)-1;hot_mask_=(1u<<hot_bits_)-1;
  }
  uint64_t endpoint_bytes=0;
  if(h_.version==3){
   struct PackHeader{char magic[8];uint64_t base_bytes,hot_bytes;}ph{};std::memcpy(&ph,data_+prefix,sizeof(ph));if(std::memcmp(ph.magic,"SRBADP1",7))throw std::runtime_error("bad adaptive Atlas");
   auto setup=[&](uint64_t values,uint64_t records,const uint8_t*&q,const uint8_t*&offs,const uint8_t*&widths,const uint8_t*&data){uint64_t blocks=values/16,groups=(blocks+15)/16;offs=q;q+=(groups+1)*4;widths=q;q+=(blocks+1)/2;data=q;q+=records;};
   const uint8_t*q=reinterpret_cast<const uint8_t*>(data_+prefix+sizeof(ph));setup(h_.base_values,ph.base_bytes,q,base_offsets_,base_widths_,base_records_);setup(h_.hot_values,ph.hot_bytes,q,hot_offsets_,hot_widths_,hot_records_);endpoint_bytes=q-reinterpret_cast<const uint8_t*>(data_);
  }else if(h_.version==4){
   const uint64_t base_bytes=h_.base_values/16*base_record_bytes_,hot_bytes=h_.hot_values/16*hot_record_bytes_,exceptions_offset=prefix+base_bytes+hot_bytes;if(exceptions_offset+sizeof(EndpointExceptionHeader)>bytes_)throw std::runtime_error("truncated hybrid Atlas");EndpointExceptionHeader eh{};std::memcpy(&eh,data_+exceptions_offset,sizeof(eh));if(std::memcmp(eh.magic,"SRBEXC1",7)||eh.version!=1||eh.reserved)throw std::runtime_error("bad hybrid Atlas exceptions");base_exception_count_=eh.base_count;hot_exception_count_=eh.hot_count;base_exceptions_=reinterpret_cast<const EndpointException*>(data_+exceptions_offset+sizeof(eh));hot_exceptions_=base_exceptions_+base_exception_count_;endpoint_bytes=exceptions_offset+sizeof(eh)+uint64_t(base_exception_count_+hot_exception_count_)*sizeof(EndpointException);
  }else{const uint64_t payload=bits_?(h_.base_values+h_.hot_values)/16*record_bytes_:2*(h_.base_values+h_.hot_values);endpoint_bytes=prefix+payload;}
#ifdef SRB_ENDPOINT_EXTENSION
  if(endpoint_bytes>bytes_)throw std::runtime_error("truncated endpoint Atlas");
#else
  if(endpoint_bytes!=bytes_)throw std::runtime_error("truncated endpoint Atlas");
#endif
  const uint16_t*p=reinterpret_cast<const uint16_t*>(data_+64);
  auto ids=[&](int16_t*map,unsigned n){for(unsigned i=0;i<n;++i){if(*p>=496||map[*p]>=0)throw std::runtime_error("bad port ID");map[*p++]=i;}};
  ids(src_,h_.sources);ids(dst_,h_.targets);ids(hsrc_,h_.hot_sources);ids(hdst_,h_.hot_targets);
  base_=p;
  if(h_.version==3){base_=reinterpret_cast<const uint16_t*>(base_records_);hot_=reinterpret_cast<const uint16_t*>(hot_records_);}
  else if(h_.version==2){
   hot_=reinterpret_cast<const uint16_t*>(reinterpret_cast<const char*>(base_)+(h_.base_values/16)*record_bytes_);
   if(reinterpret_cast<const char*>(hot_)+(h_.hot_values/16)*record_bytes_!=data_+endpoint_bytes)throw std::runtime_error("truncated packed Atlas");
  }else if(h_.version==4){
   hot_=reinterpret_cast<const uint16_t*>(reinterpret_cast<const char*>(base_)+(h_.base_values/16)*base_record_bytes_);
   if(reinterpret_cast<const char*>(hot_)+(h_.hot_values/16)*hot_record_bytes_!=reinterpret_cast<const char*>(data_)+prefix+(h_.base_values/16)*base_record_bytes_+(h_.hot_values/16)*hot_record_bytes_)throw std::runtime_error("truncated hybrid packed Atlas");
  }else{hot_=base_+h_.base_values;if(reinterpret_cast<const char*>(hot_+h_.hot_values)!=data_+endpoint_bytes)throw std::runtime_error("truncated endpoint Atlas");}
  unsigned k=0;for(int y=-72;y<=72;++y)for(int x=-72;x<=72;++x)annulus_[(y+72)*145+x+72]=(std::abs(x)<=SRB_HOT_INNER_RADIUS&&std::abs(y)<=SRB_HOT_INNER_RADIUS)?65535:k++;
#ifdef SRB_EXACT_LOCAL_ROUTE_TABLE
#ifdef _WIN32
#ifndef SRB_EXACT_LOCAL_RESOURCE_ID
#define SRB_EXACT_LOCAL_RESOURCE_ID 101
#endif
  HRSRC exact_resource=FindResourceA(nullptr,
      MAKEINTRESOURCEA(SRB_EXACT_LOCAL_RESOURCE_ID),RT_RCDATA);
  if(!exact_resource)throw std::runtime_error("missing exact local table resource");
  HGLOBAL exact_loaded=LoadResource(nullptr,exact_resource);
  const void*exact_data=exact_loaded?LockResource(exact_loaded):nullptr;
  const DWORD exact_size=SizeofResource(nullptr,exact_resource);
  if(!exact_data||!exact_size)throw std::runtime_error("cannot load exact local table resource");
  exact_local_.reset(static_cast<const unsigned char*>(exact_data),size_t(exact_size));
#else
  exact_local_.reset(srb_exact_local_begin,
                     size_t(srb_exact_local_end-srb_exact_local_begin));
#endif
#endif
#ifdef SRB_ROUTE_TAIL
#ifndef SRB_ROUTE_TAIL_PATH
#define SRB_ROUTE_TAIL_PATH "route_tail.bin"
#endif
  load_route_tail(SRB_ROUTE_TAIL_PATH);
#endif
#ifdef SRB_ENDPOINT_EXTENSION
  if(bytes_>endpoint_bytes)init_extension(reinterpret_cast<const uint8_t*>(data_+endpoint_bytes),bytes_-endpoint_bytes);
  else load_extension("endpoint_extension.bin");
#endif
#ifdef SRB_ROUTE_PERIOD_TABLE
#ifndef SRB_ROUTE_PERIOD_TABLE_PATH
#define SRB_ROUTE_PERIOD_TABLE_PATH "route_period_table.bin"
#endif
  load_route_period_table(SRB_ROUTE_PERIOD_TABLE_PATH);
#endif
#ifdef SRB_SOURCE_MOVE_CACHE
  prepare_source_move_cache();
#endif
 }
 ~HybridEstimator(){
#ifdef _WIN32
  if(data_)UnmapViewOfFile(data_);if(map_)CloseHandle(map_);if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
#else
  if(data_)munmap(const_cast<char*>(data_),bytes_);
#endif
 }
 HybridEstimator(const HybridEstimator&)=delete;
 bool predict_spec(srb_fast::Slice from,srb_fast::Slice to,uint32_t&result,bool&local)const{
  srb_fast::Endpoint a,b;if(!parse_endpoints(from,to,a,b))return false;return predict_endpoints(a,b,result,local);
 }
 bool parse_endpoints(srb_fast::Slice from,srb_fast::Slice to,srb_fast::Endpoint&a,srb_fast::Endpoint&b)const{
#ifdef SRB_PRETRIMMED_ENDPOINTS
  return fallback_.parse_endpoint_pretrimmed(from,a)&&fallback_.parse_endpoint_pretrimmed(to,b);
#else
  return fallback_.parse_endpoint(from,a)&&fallback_.parse_endpoint(to,b);
#endif
 }
 bool predict_endpoints(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,uint32_t&result,bool&local)const{
  if(a.x==b.x&&a.y==b.y&&a.port==b.port){result=0;local=true;return true;}
  int dx=int(b.x)-a.x,dy=int(b.y)-a.y;
  // Crossable horizontal Blocks contain no SRB sites: their missing columns
  // do not consume a Net offset. Fold those columns out before the regular
  // fabric lookup; local_lookup_delay() charges the Block penalty once.
  bool endpoint_block_fold=false;
#ifdef SRB_LANDING_BLOCK_CONTEXT
  srb_fast::Endpoint local_source=a;
#endif
#ifdef SRB_BLOCK_FOLD_CACHE
  const int cached_fold=fallback_.local_dx_fold(a,b);dx+=cached_fold;endpoint_block_fold=cached_fold!=0;
#else
  for(int bi=1;bi<srb_fast_data::kBlockCount;++bi){
   if(a.y>=srb_fast_data::kBlockLower[bi]&&a.y<=srb_fast_data::kBlockUpper[bi]&&b.y>=srb_fast_data::kBlockLower[bi]&&b.y<=srb_fast_data::kBlockUpper[bi]){
    if(a.x<srb_fast_data::kBlockLeft[bi]&&b.x>srb_fast_data::kBlockRight[bi]){dx-=srb_fast_data::kBlockRight[bi]-srb_fast_data::kBlockLeft[bi]+1;endpoint_block_fold=true;}
    else if(b.x<srb_fast_data::kBlockLeft[bi]&&a.x>srb_fast_data::kBlockRight[bi]){dx+=srb_fast_data::kBlockRight[bi]-srb_fast_data::kBlockLeft[bi]+1;endpoint_block_fold=true;}
   }
  }
#endif
#ifdef SRB_ACTUAL_SOURCE_NET
  // Endpoint Atlases are generated in an obstacle-free fabric.  An Output
  // source has one compulsory first Net.  Resolve that Net at its real chip
  // coordinate: near a Block its physical landing point may be shifted by all
  // missing sites, and at a boundary/non-crossable landing it may disappear.
  // When both endpoints already cross the same horizontal Block band the
  // coordinate fold above has accounted for that shift.
  bool source_net_valid=true;uint16_t source_net_extra=0;
  if(srb_local_arch::kPortToInput[a.port]<0){
   const auto&e=srb_local_arch::kOutputNet[a.port];
   if(e.next_route>=0){
    int mx=0,my=0;
#ifdef SRB_SOURCE_MOVE_CACHE
    source_net_valid=cached_output_move(a,mx,my,source_net_extra);
#else
    source_net_valid=actual_output_move(a,e.dx,e.dy,mx,my,source_net_extra);
#endif
    if(source_net_valid){
     if(!endpoint_block_fold){
      dx-=mx-int(e.dx);dy-=my-int(e.dy);
#ifdef SRB_LANDING_BLOCK_CONTEXT
#ifdef SRB_SHIFTED_LANDING_ONLY
      if(mx!=int(e.dx)||my!=int(e.dy)){
#endif
      local_source.x=static_cast<int16_t>(int(a.x)+mx);
      local_source.y=static_cast<int16_t>(int(a.y)+my);
      fallback_.refresh_endpoint_metadata(local_source);
#ifdef SRB_SHIFTED_LANDING_ONLY
      }
#endif
#endif
     }
     else source_net_extra=0;
     source_net_extra+=source_excursion_gap(a,b,mx,my);
    }
   }
  }
#endif
  int d=std::max(std::abs(dx),std::abs(dy));
  uint16_t value=65535;
  if(radius_&&
#ifdef SRB_ACTUAL_SOURCE_NET
     source_net_valid&&
#endif
     (d<=radius_
#ifdef SRB_ENDPOINT_EXTENSION
     ||(extension_data_&&d<=SRB_EXTENSION_RADIUS)
#endif
     )&&
#ifdef SRB_MERGED_MODEL
#ifdef SRB_LANDING_BLOCK_CONTEXT
     fallback_.local_lookup_allowed(local_source,b)
#else
     fallback_.local_lookup_allowed(a,b)
#endif
#else
     safe(a,b)
#endif
     ){
   if(d<=
#ifdef SRB_EXACT_LOCAL_ROUTE_TABLE
      int(exact_local_.radius())
      )value=exact_local_.predict(a.port,b.port,dx,dy);
#else
      24
      &&src_[a.port]>=0&&dst_[b.port]>=0)value=get(base_,(uint64_t(src_[a.port])*49*49+(dy+24)*49+dx+24)*h_.targets+dst_[b.port]);
#endif
   else if(d>SRB_HOT_INNER_RADIUS&&d<=72&&hsrc_[a.port]>=0&&hdst_[b.port]>=0
#ifdef SRB_HOT_ANCHORS_ONLY
     &&false
#endif
     )value=get(hot_,(uint64_t(hsrc_[a.port])*h_.hot_cells+annulus_[(dy+72)*145+dx+72])*h_.hot_targets+hdst_[b.port]);
#ifdef SRB_ENDPOINT_EXTENSION
   else if(d>72&&d<=SRB_EXTENSION_RADIUS&&ext_src_[a.port]>=0&&ext_dst_[b.port]>=0&&extension_data_
#ifdef SRB_EXT_ANCHORS_ONLY
     &&false
#endif
     ){constexpr int ew=2*SRB_EXTENSION_RADIUS+1,ec=ew*ew-145*145;value=get_extension((uint64_t(ext_src_[a.port])*ec+ext_annulus_[(dy+SRB_EXTENSION_RADIUS)*ew+dx+SRB_EXTENSION_RADIUS])*256+ext_dst_[b.port]);}
#endif
  }
#ifdef SRB_LONG_EXTRAPOLATE_DISABLED
  // In the uniform routing fabric, long straight paths become periodic with
  // the maximum wire span (8).  Reuse two exact hot-Atlas samples with the
  // same displacement residue to extend that periodic tail.  Gap delays are
  // still added from the real coordinates below; paths crossing Blocks stay
  // on the conservative fallback model.
  if(value==65535&&d>72&&hsrc_[a.port]>=0&&hdst_[b.port]>=0&&
#ifdef SRB_MERGED_MODEL
#ifdef SRB_LANDING_BLOCK_CONTEXT
     fallback_.local_lookup_allowed(local_source,b)
#else
     fallback_.local_lookup_allowed(a,b)
#endif
#else
     safe(a,b)
#endif
     ){
   auto hot_value=[&](int qx,int qy)->uint16_t{
    uint16_t cell=annulus_[(qy+72)*145+qx+72];
    if(cell==65535)return 65535;
    return get(hot_,(uint64_t(hsrc_[a.port])*h_.hot_cells+cell)*h_.hot_targets+hdst_[b.port]);
   };
   if(std::abs(dy)>72&&std::abs(dx)<=72){
    int sign=dy>0?1:-1,ad=std::abs(dy),anchor=65+(ad-65)%8;
    int y1=sign*anchor,y0=sign*(anchor-8),steps=(ad-anchor)/8;
    uint16_t v1=hot_value(dx,y1),v0=hot_value(dx,y0);
    if(v1!=65535&&v0!=65535){int64_t q=int64_t(v1)+int64_t(steps)*(int(v1)-int(v0));if(q>=0&&q<65535)value=uint16_t(q);}
   }else if(std::abs(dx)>72&&std::abs(dy)<=72){
    int sign=dx>0?1:-1,ad=std::abs(dx),anchor=65+(ad-65)%8;
    int x1=sign*anchor,x0=sign*(anchor-8),steps=(ad-anchor)/8;
    uint16_t v1=hot_value(x1,dy),v0=hot_value(x0,dy);
    if(v1!=65535&&v0!=65535){int64_t q=int64_t(v1)+int64_t(steps)*(int(v1)-int(v0));if(q>=0&&q<65535)value=uint16_t(q);}
   }
  }
#endif
#ifdef SRB_ROUTE_PERIOD_TABLE
  // The endpoint Atlas is deliberately finite.  Beyond it, price the
  // obstacle-free routing core from an exact-Dijkstra-derived 10x12 affine
  // table.  This is a fixed number of array reads per source/target connector
  // pair; no graph search is performed online.
  if(value==65535&&d>72&&route_period_predict(a.port,b.port,dx,dy,value)&&
#ifdef SRB_MERGED_MODEL
     fallback_.local_lookup_allowed(a,b)
#else
     safe(a,b)
#endif
     ){}
#endif
#ifdef SRB_LONG_EXTRAPOLATE_V2
#ifndef SRB_PERIOD_SLOPE_SPAN
#define SRB_PERIOD_SLOPE_SPAN 1
#endif
  // The architecture data uses maximum spans of 10 horizontally and 12
  // vertically. Reuse the farthest available exact samples with those real
  // fabric periods (the extension provides anchors through radius 104).
  if(value==65535&&d>72&&hsrc_[a.port]>=0&&hdst_[b.port]>=0&&
#ifdef SRB_PERIOD_GATE
     period_allowed(a.port,dx,dy)&&
#endif
#ifdef SRB_MERGED_MODEL
     fallback_.local_lookup_allowed(a,b)
#else
     safe(a,b)
#endif
     ){
   const bool has_ext=
#ifdef SRB_ENDPOINT_EXTENSION
    extension_data_&&ext_src_[a.port]>=0&&ext_dst_[b.port]>=0;
#else
    false;
#endif
   const int limit=has_ext?SRB_EXTENSION_RADIUS:72;
   auto exact_value=[&](int qx,int qy)->uint16_t{
    int qd=std::max(std::abs(qx),std::abs(qy));
    if(qd<=24&&src_[a.port]>=0&&dst_[b.port]>=0)
     return get(base_,(uint64_t(src_[a.port])*49*49+(qy+24)*49+qx+24)*h_.targets+dst_[b.port]);
    if(qd<=72){uint16_t cell=annulus_[(qy+72)*145+qx+72];if(cell!=65535)return get(hot_,(uint64_t(hsrc_[a.port])*h_.hot_cells+cell)*h_.hot_targets+hdst_[b.port]);}
#ifdef SRB_ENDPOINT_EXTENSION
    if(has_ext&&qd<=SRB_EXTENSION_RADIUS){constexpr int ew=2*SRB_EXTENSION_RADIUS+1,ec=ew*ew-145*145;uint16_t cell=ext_annulus_[(qy+SRB_EXTENSION_RADIUS)*ew+qx+SRB_EXTENSION_RADIUS];if(cell!=65535)return get_extension((uint64_t(ext_src_[a.port])*ec+cell)*256+ext_dst_[b.port]);}
#endif
    return 65535;
   };
   auto extend=[&](int v,int old,int periods)->int64_t{
    int64_t num=int64_t(periods)*(int64_t(v)-old),den=SRB_PERIOD_SLOPE_SPAN;
    return int64_t(v)+(num>=0?(num+den/2)/den:(num-den/2)/den);
   };
   if(std::abs(dx)>limit&&std::abs(dy)>limit){
    constexpr int px=10,py=12;int sx=dx>0?1:-1,sy=dy>0?1:-1;
    int adx=std::abs(dx),ady=std::abs(dy),xs=limit-px+1,ys=limit-py+1;
    int ax=xs+(adx-xs)%px,ay=ys+(ady-ys)%py,nx=(adx-ax)/px,ny=(ady-ay)/py;
    uint16_t v=exact_value(sx*ax,sy*ay),vx=exact_value(sx*(ax-SRB_PERIOD_SLOPE_SPAN*px),sy*ay),vy=exact_value(sx*ax,sy*(ay-SRB_PERIOD_SLOPE_SPAN*py));
    if(v!=65535&&vx!=65535&&vy!=65535){int64_t q=extend(v,vx,nx)+extend(v,vy,ny)-v;if(q>=0&&q<65535)value=uint16_t(q);}
   }else if(std::abs(dy)>limit&&std::abs(dx)<=limit){
    constexpr int period=12;int start=limit-period+1,sign=dy>0?1:-1,ad=std::abs(dy),anchor=start+(ad-start)%period;
    int y1=sign*anchor,y0=sign*(anchor-SRB_PERIOD_SLOPE_SPAN*period),steps=(ad-anchor)/period;
    uint16_t v1=exact_value(dx,y1),v0=exact_value(dx,y0);
    if(v1!=65535&&v0!=65535){int64_t q=extend(v1,v0,steps);if(q>=0&&q<65535)value=uint16_t(q);}
   }else if(std::abs(dx)>limit&&std::abs(dy)<=limit){
    constexpr int period=10;int start=limit-period+1,sign=dx>0?1:-1,ad=std::abs(dx),anchor=start+(ad-start)%period;
    int x1=sign*anchor,x0=sign*(anchor-SRB_PERIOD_SLOPE_SPAN*period),steps=(ad-anchor)/period;
    uint16_t v1=exact_value(x1,dy),v0=exact_value(x0,dy);
    if(v1!=65535&&v0!=65535){int64_t q=extend(v1,v0,steps);if(q>=0&&q<65535)value=uint16_t(q);}
   }
  }
#endif
  if(value==65535&&tail_predict(a,b,value)){}
  local=value!=65535;
  if(local)result=value+std::abs(int(srb_fast_data::kXGapPrefix[b.x])-srb_fast_data::kXGapPrefix[a.x])+std::abs(int(srb_fast_data::kYGapPrefix[b.y])-srb_fast_data::kYGapPrefix[a.y])
#ifdef SRB_MERGED_MODEL
#ifdef SRB_LANDING_BLOCK_CONTEXT
    +fallback_.local_lookup_delay(local_source,b)
#else
    +fallback_.local_lookup_delay(a,b)
#endif
#endif
#ifdef SRB_ACTUAL_SOURCE_NET
    +source_net_extra
#endif
    ;
  else result=fallback_.predict(a,b);
#ifdef SRB_HIERARCHICAL_SELECTOR
  if(local&&selector_gate(a,b)){
   const uint32_t alternative=fallback_.predict(a,b);
   if(selector_choose(a,b,result,alternative))result=alternative;
  }
#endif
  return true;
 }
 void prefetch_endpoints(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b)const{
#if defined(__GNUC__) || defined(__clang__)
  // Cheap best-effort address prediction for the overwhelmingly common
  // obstacle-free lookup.  Exact prediction still performs all Block/source
  // corrections; a slightly wrong prefetch is harmless.
  const int dx=int(b.x)-a.x,dy=int(b.y)-a.y;
  const int d=std::max(std::abs(dx),std::abs(dy));
  const void*p=nullptr;
  if(h_.version==2||h_.version==4){
   if(d<=24&&src_[a.port]>=0&&dst_[b.port]>=0){
    uint64_t i=(uint64_t(src_[a.port])*49*49+(dy+24)*49+dx+24)*h_.targets+dst_[b.port];
    p=reinterpret_cast<const uint8_t*>(base_)+(i/16)*base_record_bytes_;
   }else if(d>24&&d<=72&&hsrc_[a.port]>=0&&hdst_[b.port]>=0){
    uint16_t c=annulus_[(dy+72)*145+dx+72];
    if(c!=65535){uint64_t i=(uint64_t(hsrc_[a.port])*h_.hot_cells+c)*h_.hot_targets+hdst_[b.port];p=reinterpret_cast<const uint8_t*>(hot_)+(i/16)*hot_record_bytes_;}
   }
#ifdef SRB_ENDPOINT_EXTENSION
   else if(d>72&&d<=SRB_EXTENSION_RADIUS&&extension_data_&&ext_src_[a.port]>=0&&ext_dst_[b.port]>=0){
    constexpr int ew=2*SRB_EXTENSION_RADIUS+1,ec=ew*ew-145*145;uint16_t c=ext_annulus_[(dy+SRB_EXTENSION_RADIUS)*ew+dx+SRB_EXTENSION_RADIUS];
    if(c!=65535){uint64_t i=(uint64_t(ext_src_[a.port])*ec+c)*256+ext_dst_[b.port];p=extension_data_+(i/16)*18;}
   }
#endif
  }
  if(p)__builtin_prefetch(p,0,0);
#else
  (void)a;(void)b;
#endif
 }
 size_t atlas_memory_bytes()const{return bytes_
#ifdef SRB_EXACT_LOCAL_ROUTE_TABLE
#ifdef _WIN32
  +exact_local_.size_bytes()
#else
  +size_t(srb_exact_local_end-srb_exact_local_begin)
#endif
#endif
#ifdef SRB_ENDPOINT_EXTENSION
  +extension_.size()
#endif
#ifdef SRB_ROUTE_PERIOD_TABLE
  +route_period_bytes_.size()
#endif
  ;}
 private:
#ifdef SRB_ROUTE_PERIOD_TABLE
#pragma pack(push,1)
 struct RoutePeriodHeader{
  char magic[8];uint32_t version,routes,x_period,y_period,x_anchor0,y_anchor0,quadrants;uint64_t records,bytes;uint8_t reserved[12];
 };
 struct RoutePeriodRecord{uint16_t anchor;int16_t x_slope,y_slope;};
#pragma pack(pop)
 static_assert(sizeof(RoutePeriodHeader)==64,"route period header layout");
 static_assert(sizeof(RoutePeriodRecord)==6,"route period record layout");
 void load_route_period_table(const char*path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("cannot open route period table");auto n=f.tellg();f.seekg(0);route_period_bytes_.resize(size_t(n));f.read(reinterpret_cast<char*>(route_period_bytes_.data()),std::streamsize(route_period_bytes_.size()));if(!f)throw std::runtime_error("truncated route period table");
  if(route_period_bytes_.size()<sizeof(RoutePeriodHeader))throw std::runtime_error("short route period table");RoutePeriodHeader h{};std::memcpy(&h,route_period_bytes_.data(),sizeof(h));
  const uint64_t expected=4ull*10*12*160*160;if(std::memcmp(h.magic,"SRBPER1",7)||h.version!=1||h.routes!=160||h.x_period!=10||h.y_period!=12||h.x_anchor0!=95||h.y_anchor0!=93||h.quadrants!=4||h.records!=expected||h.bytes!=expected*sizeof(RoutePeriodRecord)||route_period_bytes_.size()!=sizeof(h)+h.bytes)throw std::runtime_error("bad route period table");
  route_period_records_=reinterpret_cast<const RoutePeriodRecord*>(route_period_bytes_.data()+sizeof(h));
 }
 static int positive_mod(int value,int divisor){int result=value%divisor;return result<0?result+divisor:result;}
 int route_period_cost(int source,int target,int dx,int dy)const{
  if(!route_period_records_||source<0||source>=160||target<0||target>=160||!dx||!dy)return 65535;
  const int sx=dx>0?1:-1,sy=dy>0?1:-1,ax_abs=95+positive_mod(std::abs(dx)-95,10),ay_abs=93+positive_mod(std::abs(dy)-93,12),nx=(std::abs(dx)-ax_abs)/10,ny=(std::abs(dy)-ay_abs)/12;
  const unsigned quadrant=unsigned((sx>0)*2+(sy>0));const unsigned rx=unsigned(ax_abs-95),ry=unsigned(ay_abs-93);
  const uint64_t index=((((uint64_t(source)*4+quadrant)*10+rx)*12+ry)*160+unsigned(target));const RoutePeriodRecord&r=route_period_records_[index];
  if(r.anchor==65535)return 65535;
  const int64_t value=int64_t(r.anchor)+int64_t(nx)*r.x_slope+int64_t(ny)*r.y_slope;return value>=0&&value<65535?int(value):65535;
 }
 bool route_period_predict(uint16_t source_port,uint16_t target_port,int dx,int dy,uint16_t&answer)const{
  using namespace srb_local_arch;if(!route_period_records_)return false;uint32_t best=65535;
  struct Seed{int route,dx,dy,cost;};Seed seeds[64];int source_count=0;const int input=kPortToInput[source_port];
  if(input>=0){const int route=kInputToRoute[input];if(route>=0)seeds[source_count++]={route,0,0,0};else for(uint32_t i=kTransitionOffset[input];i<kTransitionOffset[input+1]&&source_count<64;++i){const auto&e=kTransitions[i];seeds[source_count++]={e.next_route,e.dx,e.dy,e.cost};}}
  else{const auto&e=kOutputNet[source_port];if(e.next_route>=0)seeds[source_count++]={e.next_route,e.dx,e.dy,0};}
  PortCost targets[160];int target_count=0;const int target_input=kPortToInput[target_port];
  if(target_input>=0){const int route=kInputToRoute[target_input];if(route>=0)targets[target_count++]={uint16_t(route),0};}
  else for(uint32_t i=kTargetOffset[target_port];i<kTargetOffset[target_port+1]&&target_count<160;++i)targets[target_count++]=kTargetArcs[i];
  for(int si=0;si<source_count;++si)for(int ti=0;ti<target_count;++ti){const int core=route_period_cost(seeds[si].route,targets[ti].id,dx-seeds[si].dx,dy-seeds[si].dy);if(core!=65535)best=std::min(best,uint32_t(core+seeds[si].cost+targets[ti].cost));}
  if(best>=65535)return false;answer=uint16_t(best);return true;
 }
#endif
#ifdef SRB_HIERARCHICAL_SELECTOR
 static uint8_t selector_distance_bin(int d){
  static constexpr int edge[13]={16,24,32,40,48,56,72,104,128,192,256,384,512};
  uint8_t b=0;while(b<13&&d>edge[b])++b;return b;
 }
 static uint32_t selector_key(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b){
  const int dx=int(b.x)-a.x,dy=int(b.y)-a.y,adx=std::abs(dx),ady=std::abs(dy);
  const uint8_t db=selector_distance_bin(std::max(adx,ady));
  const uint8_t orient=uint8_t((dx>0)+2*(dy>0)+4*(adx>ady)+8*(ady>adx));
  return (uint32_t(a.port)*14u+db)*16u+orient;
 }
 static bool selector_gate(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b){
  const uint32_t key=selector_key(a,b);
  return srb_v54_selector::bit(srb_v54_selector::kGateBits,key);
 }
 static bool selector_choose(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,uint32_t atlas,uint32_t alternative){
  const int delta=int(alternative)-int(atlas),magnitude=std::abs(delta);
  const uint32_t sign=delta<0?0u:delta>0?2u:1u;
  static constexpr int edge[8]={0,2,4,8,16,32,64,128};
  uint32_t mag=0;while(mag<8&&magnitude>edge[mag])++mag;
  const uint32_t key=(selector_key(a,b)*3u+sign)*9u+mag;
  return srb_v54_selector::bit(srb_v54_selector::kFineBits,key);
 }
#endif
#ifdef SRB_SOURCE_MOVE_CACHE
 void prepare_source_move_cache(){
  using namespace srb_local_arch;
  std::fill(source_move_class_,source_move_class_+496,int16_t(-1));
  std::vector<std::pair<int8_t,int8_t>> moves;
  for(int p=0;p<496;++p){const auto&e=kOutputNet[p];if(kPortToInput[p]>=0||e.next_route<0)continue;size_t i=0;while(i<moves.size()&&(moves[i].first!=e.dx||moves[i].second!=e.dy))++i;if(i==moves.size())moves.push_back({e.dx,e.dy});source_move_class_[p]=int16_t(i);}
  source_move_cache_.resize(moves.size()*120u*550u);
  for(size_t m=0;m<moves.size();++m)for(int x=0;x<120;++x)for(int y=0;y<550;++y){srb_fast::Endpoint a;a.x=int16_t(x);a.y=int16_t(y);int dx=0,dy=0;uint16_t extra=0;bool valid=actual_output_move(a,moves[m].first,moves[m].second,dx,dy,extra);uint32_t code=valid?0x80000000u|uint8_t(int8_t(dx))|(uint32_t(uint8_t(int8_t(dy)))<<8)|(uint32_t(extra)<<16):0;source_move_cache_[(m*120u+unsigned(x))*550u+unsigned(y)]=code;}
 }
 bool cached_output_move(const srb_fast::Endpoint&a,int&dx,int&dy,uint16_t&extra)const{
  const int c=source_move_class_[a.port];if(c<0)return false;const uint32_t code=source_move_cache_[(size_t(c)*120u+unsigned(a.x))*550u+unsigned(a.y)];if(!(code&0x80000000u))return false;dx=int(int8_t(code&255));dy=int(int8_t((code>>8)&255));extra=uint16_t((code>>16)&0x1ff);return true;
 }
#endif
#ifdef SRB_ACTUAL_SOURCE_NET
 static bool actual_output_move(const srb_fast::Endpoint&a,int ndx,int ndy,int&dx,int&dy,uint16_t&extra){
  using namespace srb_fast_data;
  const bool horizontal=ndx!=0;const int delta=horizontal?ndx:ndy;
  extra=0;if(!delta){dx=dy=0;return true;}const int step=delta>0?1:-1,need=std::abs(delta);
  int x=a.x,y=a.y,counted=0,guard=0;
  while(counted<need){
   if(++guard>700)return false;if(horizontal)x+=step;else y+=step;
   if(x<0||x>=120||y<0||y>=550)return false;
   int block=-1;for(int i=0;i<kBlockCount;++i)if(x>=kBlockLeft[i]&&x<=kBlockRight[i]&&y>=kBlockLower[i]&&y<=kBlockUpper[i]){block=i;break;}
   if(block<0){++counted;continue;}
   // Only Blocks 1..6 are horizontally crossable.  Their missing sites do
   // not consume the Net offset; all other missing sites do.
   if(horizontal&&block>0)extra=292;else ++counted;
  }
  for(int i=0;i<kBlockCount;++i)if(x>=kBlockLeft[i]&&x<=kBlockRight[i]&&y>=kBlockLower[i]&&y<=kBlockUpper[i])return false;
  dx=x-a.x;dy=y-a.y;return true;
 }
 static uint16_t source_excursion_gap(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,int dx,int dy){
  using namespace srb_fast_data;
  auto extra=[](int s,int t,int landing,const uint16_t*p)->uint16_t{
   if(landing>s){
    if(t>=landing)return 0;
    int q=t>=s?int(p[landing])-p[t]:int(p[landing])-p[s];return uint16_t(2*q);
   }
   if(landing<s){
    if(t<=landing)return 0;
    int q=t<=s?int(p[t])-p[landing]:int(p[s])-p[landing];return uint16_t(2*q);
   }
   return 0;
  };
  return dx?extra(a.x,b.x,a.x+dx,kXGapPrefix):extra(a.y,b.y,a.y+dy,kYGapPrefix);
 }
#endif
#ifdef SRB_PERIOD_GATE
 static bool period_allowed(uint16_t port,int dx,int dy){
#ifdef SRB_GAP_GATE
  int d=std::max(std::abs(dx),std::abs(dy)),b=std::min(7,std::max(0,(d-1)/64));
  unsigned i=(((unsigned(port)*8+b)*2+(dx>0))*2+(dy>0));return srb_gap_gate_data::kGate[i]!=0;
#else
  int d=std::max(std::abs(dx),std::abs(dy)),b=d<=104?0:d<=128?1:d<=192?2:d<=256?3:d<=320?4:d<=384?5:d<=448?6:7;
  unsigned i=(((unsigned(port)*8+b)*2+(dx>0))*2+(dy>0));return srb_period_gate_data::kGate[i]!=0;
#endif
 }
#endif
#ifdef SRB_ENDPOINT_EXTENSION
#pragma pack(push,1)
 struct ExtHeader{char magic[8];uint32_t version,inner,radius,cells,sources,targets,bits,reserved;uint64_t values;};
 struct ExtException{uint32_t index;uint16_t value;};
#pragma pack(pop)
 void init_extension(const uint8_t*data,size_t n){
  if(n<sizeof(ExtHeader))throw std::runtime_error("truncated endpoint extension");ExtHeader e{};std::memcpy(&e,data,sizeof(e));
  if(std::memcmp(e.magic,"SRBEXT1",7)||e.version!=2||e.inner!=72||e.radius!=SRB_EXTENSION_RADIUS||!e.sources||e.sources>496||e.targets!=256||e.bits!=8||e.values!=uint64_t(e.sources)*e.cells*e.targets||n!=sizeof(e)+2ull*(e.sources+e.targets)+(e.values/16)*18+uint64_t(e.reserved)*sizeof(ExtException))throw std::runtime_error("bad endpoint extension");
  std::fill(ext_src_,ext_src_+496,-1);std::fill(ext_dst_,ext_dst_+496,-1);auto*p=reinterpret_cast<const uint16_t*>(data+sizeof(e));for(unsigned i=0;i<e.sources;++i){if(p[i]>=496||ext_src_[p[i]]>=0)throw std::runtime_error("bad extension source IDs");ext_src_[p[i]]=i;}p+=e.sources;for(unsigned i=0;i<e.targets;++i){if(p[i]>=496||ext_dst_[p[i]]>=0)throw std::runtime_error("bad extension target IDs");ext_dst_[p[i]]=i;}extension_data_=reinterpret_cast<const uint8_t*>(p+e.targets);extension_exceptions_=reinterpret_cast<const ExtException*>(extension_data_+(e.values/16)*18);extension_exception_count_=e.reserved;
  constexpr int ew=2*SRB_EXTENSION_RADIUS+1;std::fill(ext_annulus_,ext_annulus_+ew*ew,65535);unsigned k=0;for(int y=-SRB_EXTENSION_RADIUS;y<=SRB_EXTENSION_RADIUS;++y)for(int x=-SRB_EXTENSION_RADIUS;x<=SRB_EXTENSION_RADIUS;++x)if(std::max(std::abs(x),std::abs(y))>72)ext_annulus_[(y+SRB_EXTENSION_RADIUS)*ew+x+SRB_EXTENSION_RADIUS]=k++;
 }
 void load_extension(const char*path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return;auto n=f.tellg();f.seekg(0);extension_.resize(size_t(n));f.read(reinterpret_cast<char*>(extension_.data()),extension_.size());if(!f)throw std::runtime_error("truncated endpoint extension");init_extension(extension_.data(),extension_.size());
 }
 uint16_t get_extension(uint64_t index)const{auto*p=extension_data_+(index/16)*18;uint16_t lo;std::memcpy(&lo,p,2);uint8_t v=p[2+index%16];if(v!=255)return uint16_t(lo+v);uint32_t l=0,r=extension_exception_count_;while(l<r){uint32_t m=(l+r)/2;if(extension_exceptions_[m].index<uint32_t(index))l=m+1;else r=m;}return l<extension_exception_count_&&extension_exceptions_[l].index==uint32_t(index)?extension_exceptions_[l].value:65535;}
#endif
#ifdef SRB_ROUTE_TAIL
#pragma pack(push,1)
 struct TailHeader{char magic[8];uint32_t version,routes,xradius,y0,y1,bands,xcount;uint64_t values;};
 struct CompactTailHeader{char magic[8];uint32_t version,routes,xradius,y0,y1,bands,xcount;uint64_t values,bytes;};
#pragma pack(pop)
 void load_route_tail(const char*path){
  std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)return;auto n=f.tellg();f.seekg(0);char magic[8];f.read(magic,8);f.seekg(0);
  if(!std::memcmp(magic,"SRBTLC1",7)){
   CompactTailHeader c{};f.read(reinterpret_cast<char*>(&c),sizeof(c));
   if(c.version!=1||c.routes!=160||c.xradius!=127||c.y0!=137||c.y1!=152||c.bands!=32||c.xcount!=255||c.values!=208896000ull||uint64_t(n)!=sizeof(c)+c.bytes)throw std::runtime_error("bad compact route tail");
   std::memcpy(tail_header_.magic,"SRBTAIL1",8);tail_header_.version=c.version;tail_header_.routes=c.routes;tail_header_.xradius=c.xradius;tail_header_.y0=c.y0;tail_header_.y1=c.y1;tail_header_.bands=c.bands;tail_header_.xcount=c.xcount;tail_header_.values=c.values;
   std::vector<uint8_t> data(size_t(c.bytes));f.read(reinterpret_cast<char*>(data.data()),std::streamsize(data.size()));if(!f)throw std::runtime_error("truncated compact route tail");
   tail_.resize(size_t(c.values));const uint8_t*p=data.data(),*end=p+data.size();
   for(size_t sr=0;sr<160;++sr)for(size_t band=0;band<32;++band)for(size_t tr=0;tr<160;++tr){if(end-p<2)throw std::runtime_error("bad compact route stream");uint16_t v=uint16_t(p[0])|(uint16_t(p[1])<<8);p+=2;tail_[((sr*32+band)*255)*160+tr]=v;for(size_t x=1;x<255;++x){if(p==end)throw std::runtime_error("bad compact route stream");int8_t d=int8_t(*p++);if(d==-128){if(end-p<2)throw std::runtime_error("bad compact route stream");v=uint16_t(p[0])|(uint16_t(p[1])<<8);p+=2;}else v=uint16_t(int(v)+int(d));tail_[((sr*32+band)*255+x)*160+tr]=v;}}
   if(p!=end)throw std::runtime_error("extra compact route data");
  }else{
   f.read(reinterpret_cast<char*>(&tail_header_),sizeof(tail_header_));if(std::memcmp(tail_header_.magic,"SRBTAIL1",8)||tail_header_.version!=1||tail_header_.routes!=160||tail_header_.xradius!=127||tail_header_.y0!=137||tail_header_.y1!=152||tail_header_.bands!=32||tail_header_.xcount!=255||tail_header_.values!=208896000ull||uint64_t(n)!=sizeof(TailHeader)+tail_header_.values*2)throw std::runtime_error("bad route tail");
   tail_.resize(tail_header_.values);f.read(reinterpret_cast<char*>(tail_.data()),tail_.size()*2);if(!f)throw std::runtime_error("truncated route tail");
  }
 }
 uint16_t tail_at(int sr,int band,int dx,int tr)const{
  uint64_t i=((uint64_t(sr)*32+band)*255+(dx+127))*160+tr;return tail_[i];
 }
 bool tail_predict(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,uint16_t&answer)const{
  using namespace srb_local_arch;if(tail_.empty()||!fallback_.local_lookup_allowed(a,b))return false;
  struct Seed{int r,x,y,c;};Seed seeds[64];int ns=0;
  int input=kPortToInput[a.port];
  if(input>=0){int r=kInputToRoute[input];if(r>=0)seeds[ns++]={r,0,0,0};else for(uint32_t i=kTransitionOffset[input];i<kTransitionOffset[input+1]&&ns<64;++i){auto&e=kTransitions[i];seeds[ns++]={e.next_route,e.dx,e.dy,e.cost};}}
  else{auto&e=kOutputNet[a.port];if(e.next_route>=0)seeds[ns++]={e.next_route,e.dx,e.dy,0};}
  PortCost targets[160];int nt=0;int ti=kPortToInput[b.port];
  if(ti>=0){int r=kInputToRoute[ti];if(r>=0)targets[nt++]={uint16_t(r),0};}
  else for(uint32_t i=kTargetOffset[b.port];i<kTargetOffset[b.port+1]&&nt<160;++i)targets[nt++]=kTargetArcs[i];
  int best=65535,dx0=int(b.x)-a.x,dy0=int(b.y)-a.y;
  for(int si=0;si<ns;++si)for(int tj=0;tj<nt;++tj){int dx=dx0-seeds[si].x,dy=dy0-seeds[si].y,ad=std::abs(dy);if(std::abs(dx)>127||ad<153)continue;int anchor=145+(ad-145)%8,steps=(ad-anchor)/8,y1=dy>0?anchor:-anchor,y0=dy>0?anchor-8:-(anchor-8);int b1=y1<0?y1+152:16+y1-137,b0=y0<0?y0+152:16+y0-137;uint16_t v1=tail_at(seeds[si].r,b1,dx,targets[tj].id),v0=tail_at(seeds[si].r,b0,dx,targets[tj].id);if(v1==65535||v0==65535)continue;int q=int(v1)+steps*(int(v1)-int(v0))+seeds[si].c+targets[tj].cost;if(q>=0&&q<best)best=q;}
  if(best>=65535)return false;answer=uint16_t(best);return true;
 }
#else
 bool tail_predict(const srb_fast::Endpoint&,const srb_fast::Endpoint&,uint16_t&)const{return false;}
#endif
 uint16_t get(const uint16_t*data,uint64_t index)const{
  if(h_.version==3){const bool base=h_.base_values&&data==base_;const uint8_t*offs=base?base_offsets_:hot_offsets_,*widths=base?base_widths_:hot_widths_,*records=base?base_records_:hot_records_;uint64_t block=index/16,group=block/16;uint32_t off;std::memcpy(&off,offs+group*4,4);for(uint64_t b=group*16;b<block;++b){unsigned z=(widths[b/2]>>(4*(b&1)))&15;off+=2+2*z;}unsigned z=(widths[block/2]>>(4*(block&1)))&15;auto*p=records+off;uint16_t lo;std::memcpy(&lo,p,2);unsigned bit=(index%16)*z,shift=bit%8;p+=2+bit/8;uint32_t v=p[0]|uint32_t(p[1])<<8;if(shift+z>16)v|=uint32_t(p[2])<<16;v=(v>>shift)&((1u<<z)-1);return v==((1u<<z)-1)?65535:uint16_t(lo+v);}
  if(h_.version==4){const bool base=h_.base_values&&data==base_;const unsigned bits=base?base_bits_:hot_bits_,record_bytes=base?base_record_bytes_:hot_record_bytes_,mask=base?base_mask_:hot_mask_;auto*p=reinterpret_cast<const uint8_t*>(data)+(index/16)*record_bytes;uint16_t lo;std::memcpy(&lo,p,2);unsigned bit=(index%16)*bits,shift=bit%8;p+=2+bit/8;uint16_t raw;std::memcpy(&raw,p,2);uint32_t v=raw;if(shift+bits>16)v|=uint32_t(p[2])<<16;v=(v>>shift)&mask;if(v!=mask)return uint16_t(lo+v);const EndpointException*exceptions=base?base_exceptions_:hot_exceptions_;uint32_t count=base?base_exception_count_:hot_exception_count_,l=0,r=count;while(l<r){uint32_t m=(l+r)/2;if(exceptions[m].index<uint32_t(index))l=m+1;else r=m;}return l<count&&exceptions[l].index==uint32_t(index)?exceptions[l].value:65535;}
  if(!bits_)return data[index];
  auto*p=reinterpret_cast<const uint8_t*>(data)+(index/16)*record_bytes_;
  uint16_t lo;std::memcpy(&lo,p,2);unsigned bit=(index%16)*bits_,shift=bit%8;
  p+=2+bit/8;uint16_t raw;std::memcpy(&raw,p,2);uint32_t v=raw;if(shift+bits_>16)v|=uint32_t(p[2])<<16;
  v=(v>>shift)&mask_;return v==mask_?65535:uint16_t(lo+v);
 }
 static bool safe(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b){
  using namespace srb_fast_data;
  for(int i=0;i<kBlockCount;++i){
   if(std::min(a.x,b.x)>kBlockRight[i]||std::max(a.x,b.x)<kBlockLeft[i]||std::min(a.y,b.y)>kBlockUpper[i]||std::max(a.y,b.y)<kBlockLower[i])continue;
   if((a.x<kBlockLeft[i]&&b.x>kBlockRight[i])||(b.x<kBlockLeft[i]&&a.x>kBlockRight[i])||(a.y<kBlockLower[i]&&b.y>kBlockUpper[i])||(b.y<kBlockLower[i]&&a.y>kBlockUpper[i]))return false;
  }return true;
 }
 srb_fast::FastEstimator fallback_;e2e::Header h_{};int radius_;size_t bytes_=0;const char*data_=nullptr;const uint16_t*base_=nullptr,*hot_=nullptr;
 unsigned bits_=0,record_bytes_=0,mask_=0,base_bits_=0,hot_bits_=0,base_record_bytes_=0,hot_record_bytes_=0,base_mask_=0,hot_mask_=0;
 const EndpointException*base_exceptions_=nullptr,*hot_exceptions_=nullptr;uint32_t base_exception_count_=0,hot_exception_count_=0;
 const uint8_t*base_offsets_=nullptr,*base_widths_=nullptr,*base_records_=nullptr,*hot_offsets_=nullptr,*hot_widths_=nullptr,*hot_records_=nullptr;
 int16_t src_[496],dst_[496],hsrc_[496],hdst_[496];uint16_t annulus_[145*145];
#ifdef SRB_EXACT_LOCAL_ROUTE_TABLE
 srb_exact_local::View exact_local_;
#endif
#ifdef SRB_SOURCE_MOVE_CACHE
 int16_t source_move_class_[496];std::vector<uint32_t>source_move_cache_;
#endif
#ifdef SRB_ENDPOINT_EXTENSION
 std::vector<uint8_t>extension_;const uint8_t*extension_data_=nullptr;const ExtException*extension_exceptions_=nullptr;uint32_t extension_exception_count_=0;int16_t ext_src_[496],ext_dst_[496];uint16_t ext_annulus_[(2*SRB_EXTENSION_RADIUS+1)*(2*SRB_EXTENSION_RADIUS+1)];
#endif
#ifdef SRB_ROUTE_TAIL
 TailHeader tail_header_{};std::vector<uint16_t>tail_;
#endif
#ifdef SRB_ROUTE_PERIOD_TABLE
 std::vector<uint8_t>route_period_bytes_;const RoutePeriodRecord*route_period_records_=nullptr;
#endif
#ifdef _WIN32
 HANDLE file_=INVALID_HANDLE_VALUE,map_=nullptr;
#endif
};
}
