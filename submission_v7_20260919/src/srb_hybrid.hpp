#pragma once
#include "srb_fast.hpp"
#include "local_arch_data.hpp"
#include "e2e_format.hpp"
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>
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
public:
 HybridEstimator(const std::string&path,int radius=72,unsigned=0,int=-1):radius_(radius){
  std::fill(src_,src_+496,-1);std::fill(dst_,dst_+496,-1);std::fill(hsrc_,hsrc_+496,-1);std::fill(hdst_,hdst_+496,-1);
  if(!radius)return;
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
  struct Footer { char magic[16];uint64_t offset,bytes; } footer{};
  static_assert(sizeof(Footer)==32,"footer layout");
  if(bytes_<sizeof(Footer)||pread(fd,&footer,sizeof(Footer),bytes_-sizeof(Footer))!=sizeof(Footer)
     ||std::memcmp(footer.magic,"SRB7ATLAS20260919",16)||footer.offset%4096
     ||footer.offset>bytes_-sizeof(Footer)||footer.bytes!=bytes_-sizeof(Footer)-footer.offset){
   close(fd);throw std::runtime_error("invalid embedded endpoint Atlas footer");
  }
  offset=footer.offset;bytes_=footer.bytes;
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
  if(std::memcmp(h_.magic,"SRBE2E1\0",8)||(h_.version!=1&&h_.version!=2)||h_.ports!=496||h_.base_radius!=24||h_.hot_radius!=72
     ||!h_.sources||h_.sources>496||!h_.targets||h_.targets>496||!h_.hot_sources||h_.hot_sources>496||!h_.hot_targets||h_.hot_targets>496
     ||h_.hot_cells!=145*145-49*49
     ||h_.base_values!=uint64_t(h_.sources)*49*49*h_.targets
     ||h_.hot_values!=uint64_t(h_.hot_sources)*h_.hot_cells*h_.hot_targets)throw std::runtime_error("invalid endpoint Atlas");
  uint64_t prefix=64+2ull*(h_.sources+h_.targets+h_.hot_sources+h_.hot_targets);
  if(h_.version==2){
   bits_=h_.reserved;if(bits_<1||bits_>15||h_.base_values%16||h_.hot_values%16)throw std::runtime_error("bad packed layout");
   record_bytes_=2+2*bits_;mask_=(1u<<bits_)-1;
  }
  const uint64_t payload=bits_?(h_.base_values+h_.hot_values)/16*record_bytes_:2*(h_.base_values+h_.hot_values);
  if(prefix+payload!=bytes_)throw std::runtime_error("truncated endpoint Atlas");
  const uint16_t*p=reinterpret_cast<const uint16_t*>(data_+64);
  auto ids=[&](int16_t*map,unsigned n){for(unsigned i=0;i<n;++i){if(*p>=496||map[*p]>=0)throw std::runtime_error("bad port ID");map[*p++]=i;}};
  ids(src_,h_.sources);ids(dst_,h_.targets);ids(hsrc_,h_.hot_sources);ids(hdst_,h_.hot_targets);
  base_=p;
  if(h_.version==2){
   hot_=reinterpret_cast<const uint16_t*>(reinterpret_cast<const char*>(base_)+(h_.base_values/16)*record_bytes_);
   if(reinterpret_cast<const char*>(hot_)+(h_.hot_values/16)*record_bytes_!=data_+bytes_)throw std::runtime_error("truncated packed Atlas");
  }else{hot_=base_+h_.base_values;if(reinterpret_cast<const char*>(hot_+h_.hot_values)!=data_+bytes_)throw std::runtime_error("truncated endpoint Atlas");}
  unsigned k=0;for(int y=-72;y<=72;++y)for(int x=-72;x<=72;++x)annulus_[(y+72)*145+x+72]=(std::abs(x)<=24&&std::abs(y)<=24)?65535:k++;
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
  srb_fast::Endpoint a,b;if(!fallback_.parse_endpoint(from,a)||!fallback_.parse_endpoint(to,b))return false;
  if(a.x==b.x&&a.y==b.y&&a.port==b.port){result=0;local=true;return true;}
  int dx=int(b.x)-a.x,dy=int(b.y)-a.y,d=std::max(std::abs(dx),std::abs(dy));
  uint16_t value=65535;
  if(radius_&&d<=radius_&&
#ifdef SRB_MERGED_MODEL
     fallback_.local_lookup_allowed(a,b)
#else
     safe(a,b)
#endif
     ){
   if(d<=24&&src_[a.port]>=0&&dst_[b.port]>=0)value=get(base_,(uint64_t(src_[a.port])*49*49+(dy+24)*49+dx+24)*h_.targets+dst_[b.port]);
   else if(d>24&&d<=72&&hsrc_[a.port]>=0&&hdst_[b.port]>=0)value=get(hot_,(uint64_t(hsrc_[a.port])*h_.hot_cells+annulus_[(dy+72)*145+dx+72])*h_.hot_targets+hdst_[b.port]);
  }
  local=value!=65535;
  if(local)result=value+std::abs(int(srb_fast_data::kXGapPrefix[b.x])-srb_fast_data::kXGapPrefix[a.x])+std::abs(int(srb_fast_data::kYGapPrefix[b.y])-srb_fast_data::kYGapPrefix[a.y]);
  else result=fallback_.predict(a,b);
  return true;
 }
 size_t atlas_memory_bytes()const{return bytes_;}
private:
 uint16_t get(const uint16_t*data,uint64_t index)const{
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
 unsigned bits_=0,record_bytes_=0,mask_=0;
 int16_t src_[496],dst_[496],hsrc_[496],hdst_[496];uint16_t annulus_[145*145];
#ifdef _WIN32
 HANDLE file_=INVALID_HANDLE_VALUE,map_=nullptr;
#endif
};
}
