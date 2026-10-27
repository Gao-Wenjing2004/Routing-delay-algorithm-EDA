#pragma once

#include "e2e_format.hpp"
#include "local_arch_data.hpp"
#include "runtime_atlas_ids.hpp"
#include "runtime_extension_ids.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef SRB_PARALLEL_ATLAS_BUILD
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sched.h>
#include <unistd.h>
#endif
#endif

#ifndef SRB_EXTENSION_RADIUS
#define SRB_EXTENSION_RADIUS 104
#endif

#ifndef SRB_EXTENSION_ROUTE_GROUP_LIMIT
#define SRB_EXTENSION_ROUTE_GROUP_LIMIT 1024
#endif

namespace srb_runtime_atlas {
using namespace srb_local_arch;

#ifdef SRB_PARALLEL_ATLAS_BUILD
inline size_t atlas_parallelism(size_t limit){
  size_t available=1;
#ifdef _WIN32
  available=std::max<size_t>(1,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
#else
  cpu_set_t set;CPU_ZERO(&set);if(::sched_getaffinity(0,sizeof(set),&set)==0)available=std::max<size_t>(1,CPU_COUNT(&set));
  if(FILE*quota=std::fopen("/sys/fs/cgroup/cpu.max","rb")){
    char amount[32]{};unsigned period=0;if(std::fscanf(quota,"%31s %u",amount,&period)==2&&std::strcmp(amount,"max")&&period){unsigned long long value=std::strtoull(amount,nullptr,10);available=std::min(available,size_t(std::max<unsigned long long>(1,(value+period-1)/period)));}std::fclose(quota);
  }else{
    FILE*q=std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us","rb"),*p=std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us","rb");long long value=-1,period=0;if(q){std::fscanf(q,"%lld",&value);std::fclose(q);}if(p){std::fscanf(p,"%lld",&period);std::fclose(p);}if(value>0&&period>0)available=std::min(available,size_t(std::max(1ll,(value+period-1)/period)));
  }
#endif
  return std::max<size_t>(1,std::min(limit,available));
}
#endif
constexpr uint32_t INF = std::numeric_limits<uint32_t>::max();
constexpr uint16_t INF16 = 65535;
constexpr int ROUTES = 160;
#ifdef SRB_DIST16
using Distance = uint16_t;
static constexpr Distance DIST_INF = INF16;
#else
using Distance = uint32_t;
static constexpr Distance DIST_INF = INF;
#endif

class RadixHeap {
  using Item = std::pair<uint32_t,uint32_t>;
  std::array<std::vector<Item>,33> buckets_;
  uint32_t last_=0; uint64_t size_=0;
  static int bucket(uint32_t a,uint32_t b){uint32_t v=a^b;return v?32-__builtin_clz(v):0;}
  void pull(){int i=1;while(i<33&&buckets_[i].empty())++i;if(i==33)return;uint32_t next=INF;for(const auto&q:buckets_[i])next=std::min(next,q.first);last_=next;std::vector<Item> v;v.swap(buckets_[i]);for(const auto&q:v)buckets_[bucket(q.first,last_)].push_back(q);}
 public:
  void clear(){for(auto&v:buckets_)v.clear();last_=0;size_=0;}
  bool empty()const{return size_==0;}
  void push(uint32_t d,uint32_t s){buckets_[bucket(d,last_)].push_back({d,s});++size_;}
  Item pop(){if(buckets_[0].empty())pull();Item q=buckets_[0].back();buckets_[0].pop_back();--size_;return q;}
};

// Transition weights are bounded by 135.  A circular Dial queue avoids the
// radix heap's repeated pair redistribution and allocations while preserving
// exactly the same Dijkstra distances.
class DialHeap {
  static constexpr uint32_t SPAN = 136;
  using Item = std::pair<uint32_t,uint32_t>;
  std::array<std::vector<Item>,SPAN> buckets_;
  uint32_t current_=0;
  uint64_t size_=0;
 public:
  void clear(){for(auto&v:buckets_)v.clear();current_=0;size_=0;}
  bool empty()const{return size_==0;}
  void push(uint32_t d,uint32_t s){buckets_[d%SPAN].push_back({d,s});++size_;}
  Item pop(){
    for(;;++current_){
      auto&bucket=buckets_[current_%SPAN];
      while(!bucket.empty()){
        Item q=bucket.back();bucket.pop_back();--size_;
        if(q.first<current_)continue;
        if(q.first==current_)return q;
        // With maximum edge weight SPAN-1, a future item cannot share the
        // current bucket while a smaller live key exists. Put it back and
        // continue advancing the distance cursor.
        bucket.push_back(q);++size_;break;
      }
    }
  }
};

#ifdef SRB_DIAL_QUEUE
using WorkHeap = DialHeap;
#else
using WorkHeap = RadixHeap;
#endif

class BufferedWriter {
  std::ofstream out_;
  std::vector<uint8_t> buffer_;
 public:
  explicit BufferedWriter(const std::string&path,bool append=false):out_(path,std::ios::binary|(append?std::ios::app:std::ios::trunc)){if(!out_)throw std::runtime_error("cannot create runtime Atlas");buffer_.reserve(8u<<20);}
  ~BufferedWriter(){flush();}
  void raw(const void*p,size_t n){flush();out_.write(static_cast<const char*>(p),std::streamsize(n));if(!out_)throw std::runtime_error("runtime Atlas write failed");}
  void bytes(const uint8_t*p,size_t n){if(buffer_.size()+n>buffer_.capacity())flush();buffer_.insert(buffer_.end(),p,p+n);}
  void flush(){if(!buffer_.empty()){out_.write(reinterpret_cast<const char*>(buffer_.data()),std::streamsize(buffer_.size()));buffer_.clear();if(!out_)throw std::runtime_error("runtime Atlas write failed");}}
};

class VectorWriter {
 public:
  std::vector<uint8_t> data;
  void raw(const void*p,size_t n){const auto*q=static_cast<const uint8_t*>(p);data.insert(data.end(),q,q+n);}
  void bytes(const uint8_t*p,size_t n){data.insert(data.end(),p,p+n);}
  void flush(){}
};

inline uint32_t state_id(int x,int y,int route,int width){return uint32_t((y*width+x)*ROUTES+route);}

inline void seed_port(uint16_t port,int center,int width,std::vector<Distance>&dist,WorkHeap&heap){
  int input=kPortToInput[port];
  auto add=[&](int route,int dx,int dy,int cost){uint32_t s=state_id(center+dx,center+dy,route,width);if(uint32_t(cost)<dist[s]){dist[s]=uint32_t(cost);heap.push(uint32_t(cost),s);}};
  if(input>=0){int route=kInputToRoute[input];if(route>=0)add(route,0,0,0);else for(uint32_t i=kTransitionOffset[input];i<kTransitionOffset[input+1];++i){const auto&e=kTransitions[i];add(e.next_route,e.dx,e.dy,e.cost);}}
  else {const auto&e=kOutputNet[port];if(e.next_route>=0)add(e.next_route,e.dx,e.dy,0);}
}

// Many base-table ports enter the routing fabric through the same route with
// only a small seed displacement/cost difference.  Their full Dijkstra field
// is therefore a translated copy of one canonical route field.
inline bool single_seed(uint16_t port,int&route,int&dx,int&dy,int&cost){
  int input=kPortToInput[port];
  if(input>=0){
    route=kInputToRoute[input];
    if(route>=0){dx=dy=cost=0;return true;}
    if(kTransitionOffset[input+1]-kTransitionOffset[input]!=1)return false;
    const auto&e=kTransitions[kTransitionOffset[input]];route=e.next_route;dx=e.dx;dy=e.dy;cost=e.cost;return route>=0;
  }
  const auto&e=kOutputNet[port];route=e.next_route;dx=e.dx;dy=e.dy;cost=0;return route>=0;
}

inline void run_dijkstra(std::vector<Distance>&dist,WorkHeap&heap,int width){
  while(!heap.empty()){
    auto q=heap.pop();if(dist[q.second]!=q.first)continue;
    int route=q.second%ROUTES,cell=q.second/ROUTES,x=cell%width,y=cell/width;uint16_t input=kRouteToInput[route];
    for(uint32_t i=kTransitionOffset[input];i<kTransitionOffset[input+1];++i){const auto&e=kTransitions[i];int nx=x+e.dx,ny=y+e.dy;if(nx<0||nx>=width||ny<0||ny>=width)continue;uint32_t next=state_id(nx,ny,e.next_route,width),nd=q.first+e.cost;if(nd<dist[next]){dist[next]=nd;heap.push(nd,next);}}
  }
}

inline uint16_t target_value(const std::vector<Distance>&dist,int width,int center,int dx,int dy,uint16_t port){
  size_t base=state_id(center+dx,center+dy,0,width);uint32_t best=INF;int input=kPortToInput[port];
  if(input>=0){int route=kInputToRoute[input];if(route>=0)best=dist[base+route];}
  else for(uint32_t i=kTargetOffset[port];i<kTargetOffset[port+1];++i){const auto&a=kTargetArcs[i];if(dist[base+a.id]!=DIST_INF)best=std::min(best,uint32_t(dist[base+a.id])+a.cost);}
  return best>=INF16?INF16:uint16_t(best);
}

template<class Writer>
inline void pack16(Writer&out,const uint16_t*v){
  constexpr unsigned BITS=10,MASK=(1u<<BITS)-1;uint16_t lo=INF16;
  for(int i=0;i<16;++i)if(v[i]!=INF16)lo=std::min(lo,v[i]);if(lo==INF16)lo=0;
  uint8_t record[22]{};std::memcpy(record,&lo,2);
  for(unsigned i=0;i<16;++i){uint32_t d=v[i]==INF16?MASK:uint32_t(v[i]-lo);if(d>=MASK)d=MASK;unsigned bit=i*BITS,byte=2+bit/8,shift=bit%8;uint32_t word=d<<shift;record[byte]|=uint8_t(word);record[byte+1]|=uint8_t(word>>8);if(byte+2<22)record[byte+2]|=uint8_t(word>>16);}
  out.bytes(record,sizeof(record));
}

#pragma pack(push,1)
struct EndpointException{uint32_t index;uint16_t value;};
struct EndpointExceptionHeader{char magic[8];uint32_t version,base_count,hot_count,reserved;};
struct ExtHeader{char magic[8];uint32_t version,inner,radius,cells,sources,targets,bits,reserved;uint64_t values;};
struct ExtException{uint32_t index;uint16_t value;};
#pragma pack(pop)

template<class Writer>
inline void pack16_variable(Writer&out,const uint16_t*v,unsigned bits,
                            uint64_t base,std::vector<EndpointException>&exceptions){
  const unsigned mask=(1u<<bits)-1,record_bytes=2+2*bits;uint16_t lo=INF16;
  for(int i=0;i<16;++i)if(v[i]!=INF16)lo=std::min(lo,v[i]);if(lo==INF16)lo=0;
  uint8_t record[32]{};std::memcpy(record,&lo,2);
  for(unsigned i=0;i<16;++i){uint32_t d=v[i]==INF16?mask:uint32_t(v[i]-lo);if(d>=mask){d=mask;if(v[i]!=INF16)exceptions.push_back({uint32_t(base+i),v[i]});}unsigned bit=i*bits,byte=2+bit/8,shift=bit%8;uint32_t word=d<<shift;record[byte]|=uint8_t(word);record[byte+1]|=uint8_t(word>>8);if(byte+2<record_bytes)record[byte+2]|=uint8_t(word>>16);}
  out.bytes(record,record_bytes);
}

template<class Writer>
inline void pack16_extension(Writer&out,const uint16_t*v,uint64_t base,std::vector<ExtException>&exceptions){
  constexpr unsigned MASK=255;uint16_t lo=INF16;for(int i=0;i<16;++i)if(v[i]!=INF16)lo=std::min(lo,v[i]);if(lo==INF16)lo=0;uint8_t record[18]{};std::memcpy(record,&lo,2);
  for(int i=0;i<16;++i){uint32_t d=v[i]==INF16?MASK:uint32_t(v[i]-lo);if(d>=MASK){record[2+i]=MASK;exceptions.push_back({uint32_t(base+i),v[i]});}else record[2+i]=uint8_t(d);}out.bytes(record,sizeof(record));
}

template<size_t NS,size_t NT>
inline void generate_section(BufferedWriter&out,int radius,int inner,const uint16_t(&sources)[NS],const uint16_t(&targets)[NT],unsigned bits=10,std::vector<EndpointException>*exceptions=nullptr){
  static_assert(NT%16==0,"target count must align packed blocks");
#ifndef SRB_BASE_MARGIN
#define SRB_BASE_MARGIN 48
#endif
#ifndef SRB_HOT_MARGIN
#define SRB_HOT_MARGIN 8
#endif
  const int margin=radius==24?SRB_BASE_MARGIN:SRB_HOT_MARGIN,width=2*(radius+margin)+1,center=radius+margin;
  std::vector<Distance>dist(size_t(width)*width*ROUTES);WorkHeap heap;uint16_t values[16];auto started=std::chrono::steady_clock::now();
#ifdef SRB_BASE_ROUTE_REUSE
  std::vector<Distance> route_field;int cached_route=-1;
#endif
  size_t source_count=NS;uint64_t value_index=0;
#ifdef SRB_ATLAS_TEST_COUNT
  source_count=std::min(source_count,size_t(SRB_ATLAS_TEST_COUNT));
#elif defined(SRB_ATLAS_TEST_ONE)
  source_count=1;
#endif
  for(size_t si=0;si<source_count;++si){
    const std::vector<Distance>*field=&dist;int sdx=0,sdy=0,scost=0,sroute=-1;
#ifdef SRB_BASE_ROUTE_REUSE
    bool reuse=radius==24&&single_seed(sources[si],sroute,sdx,sdy,scost)&&std::abs(sdx)<=margin&&std::abs(sdy)<=margin;
    if(reuse){
      if(cached_route!=sroute){route_field.assign(size_t(width)*width*ROUTES,DIST_INF);heap.clear();uint32_t start=state_id(center,center,sroute,width);route_field[start]=0;heap.push(0,start);run_dijkstra(route_field,heap,width);cached_route=sroute;}
      field=&route_field;
    }else
#endif
    {sdx=sdy=scost=0;std::fill(dist.begin(),dist.end(),DIST_INF);heap.clear();seed_port(sources[si],center,width,dist,heap);run_dijkstra(dist,heap,width);}
    for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){if(std::max(std::abs(dx),std::abs(dy))<=inner)continue;for(size_t t=0;t<NT;t+=16){for(int j=0;j<16;++j){uint16_t v=target_value(*field,width,center,dx-sdx,dy-sdy,targets[t+j]);values[j]=v==INF16?v:uint16_t(std::min<unsigned>(INF16-1,unsigned(v)+unsigned(scost)));}if(exceptions)pack16_variable(out,values,bits,value_index,*exceptions);else pack16(out,values);value_index+=16;}}
    double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas radius=%d source=%zu/%zu elapsed=%.1fs",radius,si+1,source_count,sec);std::fflush(stderr);
  }
  std::fprintf(stderr,"\n");
}

#ifdef SRB_PARALLEL_ATLAS_BUILD
struct EndpointSectionResult{std::vector<uint8_t>data;std::vector<EndpointException>exceptions;};

template<size_t NT>
inline EndpointSectionResult pack_endpoint_source(int radius,int inner,const uint16_t(&targets)[NT],unsigned bits,uint64_t value_base,const std::vector<Distance>&dist,int width,int center,int sdx,int sdy,int scost){
  const uint64_t excluded=inner>=0?uint64_t(2*inner+1)*(2*inner+1):0,cells=uint64_t(2*radius+1)*(2*radius+1)-excluded;
  EndpointSectionResult result;VectorWriter out;out.data.reserve(size_t(cells*(NT/16)*(2+2*bits)));
  uint16_t values[16];uint64_t index=value_base;
  for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){if(std::max(std::abs(dx),std::abs(dy))<=inner)continue;for(size_t t=0;t<NT;t+=16){for(int j=0;j<16;++j){uint16_t v=target_value(dist,width,center,dx-sdx,dy-sdy,targets[t+j]);values[j]=v==INF16?v:uint16_t(std::min<unsigned>(INF16-1,unsigned(v)+unsigned(scost)));}pack16_variable(out,values,bits,index,result.exceptions);index+=16;}}
  result.data=std::move(out.data);return result;
}

template<size_t NT>
inline EndpointSectionResult generate_endpoint_source(int radius,int inner,uint16_t source,const uint16_t(&targets)[NT],unsigned bits,uint64_t value_base){
  const int margin=radius==24?SRB_BASE_MARGIN:SRB_HOT_MARGIN,width=2*(radius+margin)+1,center=radius+margin;
  std::vector<Distance>dist(size_t(width)*width*ROUTES,DIST_INF);WorkHeap heap;int sdx=0,sdy=0,scost=0,sroute=-1;
#ifdef SRB_BASE_ROUTE_REUSE
  const bool reuse=radius==24&&single_seed(source,sroute,sdx,sdy,scost)&&std::abs(sdx)<=margin&&std::abs(sdy)<=margin;
  if(reuse){uint32_t start=state_id(center,center,sroute,width);dist[start]=0;heap.push(0,start);run_dijkstra(dist,heap,width);}
  else
#endif
  {sdx=sdy=scost=0;seed_port(source,center,width,dist,heap);run_dijkstra(dist,heap,width);}
  return pack_endpoint_source(radius,inner,targets,bits,value_base,dist,width,center,sdx,sdy,scost);
}

#if defined(SRB_PARALLEL_BASE_ROUTE_REUSE) && defined(SRB_BASE_ROUTE_REUSE)
template<size_t NT>
inline std::vector<EndpointSectionResult> generate_endpoint_route_group(
    int radius,int inner,const uint16_t*begin,const uint16_t*end,
    const uint16_t(&targets)[NT],unsigned bits,uint64_t first_value_base,
    uint64_t values_per_source){
  const int margin=radius==24?SRB_BASE_MARGIN:SRB_HOT_MARGIN,width=2*(radius+margin)+1,center=radius+margin;
  int route=-1,first_dx=0,first_dy=0,first_cost=0;
  if(begin==end||radius!=24||!single_seed(*begin,route,first_dx,first_dy,first_cost)
     ||std::abs(first_dx)>margin||std::abs(first_dy)>margin)
    throw std::runtime_error("invalid reusable Base route group");
  std::vector<Distance>dist(size_t(width)*width*ROUTES,DIST_INF);WorkHeap heap;
  const uint32_t start=state_id(center,center,route,width);dist[start]=0;heap.push(0,start);run_dijkstra(dist,heap,width);
  std::vector<EndpointSectionResult> result;result.reserve(size_t(end-begin));
  for(const uint16_t*p=begin;p!=end;++p){
    int source_route=-1,sdx=0,sdy=0,scost=0;
    if(!single_seed(*p,source_route,sdx,sdy,scost)||source_route!=route
       ||std::abs(sdx)>margin||std::abs(sdy)>margin)
      throw std::runtime_error("mixed reusable Base route group");
    result.push_back(pack_endpoint_source(radius,inner,targets,bits,
      first_value_base+uint64_t(p-begin)*values_per_source,
      dist,width,center,sdx,sdy,scost));
  }
  return result;
}
#endif

template<size_t NS,size_t NT>
inline void generate_section_parallel(BufferedWriter&out,int radius,int inner,const uint16_t(&sources)[NS],const uint16_t(&targets)[NT],unsigned bits,std::vector<EndpointException>&exceptions){
  static_assert(NT%16==0,"target count must align packed blocks");const uint64_t excluded=inner>=0?uint64_t(2*inner+1)*(2*inner+1):0,cells=uint64_t(2*radius+1)*(2*radius+1)-excluded,values_per_source=cells*NT;const size_t window=atlas_parallelism(std::min<size_t>(SRB_PARALLEL_ATLAS_BUILD,NS));auto started=std::chrono::steady_clock::now();
#if defined(SRB_PARALLEL_BASE_ROUTE_REUSE) && defined(SRB_BASE_ROUTE_REUSE)
  if(radius==24){
    struct Group{size_t begin,end;bool reuse;};std::vector<Group>groups;
    for(size_t begin=0;begin<NS;){int route=-1,dx=0,dy=0,cost=0;bool reuse=single_seed(sources[begin],route,dx,dy,cost)&&std::abs(dx)<=SRB_BASE_MARGIN&&std::abs(dy)<=SRB_BASE_MARGIN;size_t end=begin+1;if(reuse)while(end<NS){int next_route=-1,next_dx=0,next_dy=0,next_cost=0;if(!single_seed(sources[end],next_route,next_dx,next_dy,next_cost)||next_route!=route||std::abs(next_dx)>SRB_BASE_MARGIN||std::abs(next_dy)>SRB_BASE_MARGIN)break;++end;}groups.push_back({begin,end,reuse});begin=end;}
    size_t completed=0;
    for(size_t group_begin=0;group_begin<groups.size();group_begin+=window){size_t group_end=std::min(groups.size(),group_begin+window);std::vector<std::future<std::vector<EndpointSectionResult>>>futures;futures.reserve(group_end-group_begin);for(size_t gi=group_begin;gi<group_end;++gi)futures.emplace_back(std::async(std::launch::async,[&,gi]{const Group&g=groups[gi];if(g.reuse)return generate_endpoint_route_group(radius,inner,sources+g.begin,sources+g.end,targets,bits,uint64_t(g.begin)*values_per_source,values_per_source);std::vector<EndpointSectionResult>single;single.push_back(generate_endpoint_source(radius,inner,sources[g.begin],targets,bits,uint64_t(g.begin)*values_per_source));return single;}));for(size_t gi=group_begin;gi<group_end;++gi){std::vector<EndpointSectionResult>results=futures[gi-group_begin].get();for(EndpointSectionResult&result:results){out.bytes(result.data.data(),result.data.size());exceptions.insert(exceptions.end(),result.exceptions.begin(),result.exceptions.end());++completed;double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas shared-route radius=%d source=%zu/%zu group=%zu/%zu elapsed=%.1fs",radius,completed,size_t(NS),gi+1,groups.size(),sec);std::fflush(stderr);}}}
    std::fprintf(stderr,"\n");return;
  }
#endif
  for(size_t begin=0;begin<NS;begin+=window){size_t end=std::min(NS,begin+window);std::vector<std::future<EndpointSectionResult>>futures;futures.reserve(end-begin);for(size_t si=begin;si<end;++si)futures.emplace_back(std::async(std::launch::async,[&,si]{return generate_endpoint_source(radius,inner,sources[si],targets,bits,uint64_t(si)*values_per_source);}));for(size_t si=begin;si<end;++si){EndpointSectionResult result=futures[si-begin].get();out.bytes(result.data.data(),result.data.size());exceptions.insert(exceptions.end(),result.exceptions.begin(),result.exceptions.end());double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas parallel radius=%d source=%zu/%zu elapsed=%.1fs",radius,si+1,size_t(NS),sec);std::fflush(stderr);}}
  std::fprintf(stderr,"\n");
}
#endif

inline uint64_t file_size(const std::string&path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("cannot stat runtime Atlas");return uint64_t(f.tellg());}

#ifndef SRB_EXTENSION_MARGIN
#define SRB_EXTENSION_MARGIN 8
#endif
#ifdef SRB_PARALLEL_ATLAS_BUILD
struct ExtensionSectionResult{std::vector<uint8_t>data;std::vector<ExtException>exceptions;};

template<size_t NT>
inline ExtensionSectionResult pack_extension_source(const uint16_t(&targets)[NT],size_t source_index,
    const std::vector<Distance>&dist,int width,int center,int sdx,int sdy,int scost){
  constexpr int inner=72,radius=SRB_EXTENSION_RADIUS;constexpr uint32_t cells=(2*radius+1)*(2*radius+1)-145*145;ExtensionSectionResult result;VectorWriter out;out.data.reserve(size_t(cells*(NT/16)*18));uint16_t values[16];uint64_t source_base=uint64_t(source_index)*cells*NT,cell_index=0;
  for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){if(std::max(std::abs(dx),std::abs(dy))<=inner)continue;for(size_t t=0;t<NT;t+=16){for(int j=0;j<16;++j){uint16_t v=target_value(dist,width,center,dx-sdx,dy-sdy,targets[t+j]);values[j]=v==INF16?v:uint16_t(std::min<unsigned>(INF16-1,unsigned(v)+unsigned(scost)));}pack16_extension(out,values,source_base+cell_index*NT+t,result.exceptions);}++cell_index;}
  result.data=std::move(out.data);return result;
}

template<size_t NT>
inline ExtensionSectionResult generate_extension_source(uint16_t source,const uint16_t(&targets)[NT],size_t source_index){
  constexpr int radius=SRB_EXTENSION_RADIUS,margin=SRB_EXTENSION_MARGIN,width=2*(radius+margin)+1,center=radius+margin;std::vector<Distance>dist(size_t(width)*width*ROUTES,DIST_INF);WorkHeap heap;seed_port(source,center,width,dist,heap);run_dijkstra(dist,heap,width);
  return pack_extension_source(targets,source_index,dist,width,center,0,0,0);
}

#ifdef SRB_PARALLEL_EXTENSION_ROUTE_REUSE
template<size_t NT>
inline std::vector<ExtensionSectionResult> generate_extension_route_group(
    const uint16_t*begin,const uint16_t*end,const uint16_t(&targets)[NT],size_t first_source_index){
  constexpr int radius=SRB_EXTENSION_RADIUS,margin=SRB_EXTENSION_MARGIN,width=2*(radius+margin)+1,center=radius+margin;
  int route=-1,first_dx=0,first_dy=0,first_cost=0;
  if(begin==end||!single_seed(*begin,route,first_dx,first_dy,first_cost)
     ||std::abs(first_dx)>margin||std::abs(first_dy)>margin)
    throw std::runtime_error("invalid reusable Extension route group");
  std::vector<Distance>dist(size_t(width)*width*ROUTES,DIST_INF);WorkHeap heap;
  const uint32_t start=state_id(center,center,route,width);dist[start]=0;heap.push(0,start);run_dijkstra(dist,heap,width);
  std::vector<ExtensionSectionResult> result;result.reserve(size_t(end-begin));
  for(const uint16_t*p=begin;p!=end;++p){
    int source_route=-1,sdx=0,sdy=0,scost=0;
    if(!single_seed(*p,source_route,sdx,sdy,scost)||source_route!=route
       ||std::abs(sdx)>margin||std::abs(sdy)>margin)
      throw std::runtime_error("mixed reusable Extension route group");
    result.push_back(pack_extension_source(targets,first_source_index+size_t(p-begin),dist,width,center,sdx,sdy,scost));
  }
  return result;
}
#endif
#endif

inline void append_extension(const std::string&path){
#ifndef SRB_EXTENSION_MARGIN
#define SRB_EXTENSION_MARGIN 8
#endif
  using namespace srb_runtime_ext_ids;constexpr int inner=72,radius=SRB_EXTENSION_RADIUS,margin=SRB_EXTENSION_MARGIN,width=2*(radius+margin)+1,center=radius+margin;constexpr uint32_t cells=(2*radius+1)*(2*radius+1)-145*145;
  ExtHeader h{};std::memcpy(h.magic,"SRBEXT1",7);h.version=2;h.inner=inner;h.radius=radius;h.cells=cells;h.sources=uint32_t(std::size(sources));h.targets=uint32_t(std::size(targets));h.bits=8;h.values=uint64_t(h.sources)*cells*h.targets;
#ifdef SRB_PARALLEL_EXTENSION_ROUTE_REUSE
  uint16_t ordered_sources[std::size(sources)];std::copy(std::begin(sources),std::end(sources),ordered_sources);
  auto extension_route_key=[](uint16_t p){int r,dx,dy,c;if(single_seed(p,r,dx,dy,c)&&std::abs(dx)<=SRB_EXTENSION_MARGIN&&std::abs(dy)<=SRB_EXTENSION_MARGIN)return r;return ROUTES+int(p);};
  std::stable_sort(std::begin(ordered_sources),std::end(ordered_sources),[&](uint16_t a,uint16_t b){return extension_route_key(a)<extension_route_key(b);});
#else
  const auto&ordered_sources=sources;
#endif
  uint64_t header_offset=file_size(path);std::vector<ExtException>exceptions;
  {
    BufferedWriter out(path,true);out.raw(&h,sizeof(h));out.raw(ordered_sources,sizeof(ordered_sources));out.raw(targets,sizeof(targets));
#ifdef SRB_PARALLEL_ATLAS_BUILD
    const size_t source_count=std::size(sources),window=atlas_parallelism(std::min<size_t>(SRB_PARALLEL_ATLAS_BUILD,source_count));auto started=std::chrono::steady_clock::now();
#ifdef SRB_PARALLEL_EXTENSION_ROUTE_REUSE
    struct Group{size_t begin,end;bool reuse;};std::vector<Group>groups;
    for(size_t begin=0;begin<source_count;){int route=-1,dx=0,dy=0,cost=0;bool reuse=single_seed(ordered_sources[begin],route,dx,dy,cost)&&std::abs(dx)<=margin&&std::abs(dy)<=margin;size_t end=begin+1;if(reuse)while(end<source_count&&end-begin<size_t(SRB_EXTENSION_ROUTE_GROUP_LIMIT)){int next_route=-1,next_dx=0,next_dy=0,next_cost=0;if(!single_seed(ordered_sources[end],next_route,next_dx,next_dy,next_cost)||next_route!=route||std::abs(next_dx)>margin||std::abs(next_dy)>margin)break;++end;}groups.push_back({begin,end,reuse});begin=end;}
    size_t completed=0;
    for(size_t group_begin=0;group_begin<groups.size();group_begin+=window){size_t group_end=std::min(groups.size(),group_begin+window);std::vector<std::future<std::vector<ExtensionSectionResult>>>futures;futures.reserve(group_end-group_begin);for(size_t gi=group_begin;gi<group_end;++gi)futures.emplace_back(std::async(std::launch::async,[&,gi]{const Group&g=groups[gi];if(g.reuse)return generate_extension_route_group(ordered_sources+g.begin,ordered_sources+g.end,targets,g.begin);std::vector<ExtensionSectionResult>single;single.push_back(generate_extension_source(ordered_sources[g.begin],targets,g.begin));return single;}));for(size_t gi=group_begin;gi<group_end;++gi){std::vector<ExtensionSectionResult>results=futures[gi-group_begin].get();for(ExtensionSectionResult&result:results){out.bytes(result.data.data(),result.data.size());exceptions.insert(exceptions.end(),result.exceptions.begin(),result.exceptions.end());++completed;double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas shared-route extension source=%zu/%zu group=%zu/%zu elapsed=%.1fs",completed,source_count,gi+1,groups.size(),sec);std::fflush(stderr);}}}
#else
    for(size_t begin=0;begin<source_count;begin+=window){size_t end=std::min(source_count,begin+window);std::vector<std::future<ExtensionSectionResult>>futures;futures.reserve(end-begin);for(size_t si=begin;si<end;++si)futures.emplace_back(std::async(std::launch::async,[&,si]{return generate_extension_source(sources[si],targets,si);}));for(size_t si=begin;si<end;++si){ExtensionSectionResult result=futures[si-begin].get();out.bytes(result.data.data(),result.data.size());exceptions.insert(exceptions.end(),result.exceptions.begin(),result.exceptions.end());double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas parallel extension source=%zu/%zu elapsed=%.1fs",si+1,source_count,sec);std::fflush(stderr);}}
#endif
    std::fprintf(stderr,"\n");h.reserved=uint32_t(exceptions.size());out.raw(exceptions.data(),exceptions.size()*sizeof(ExtException));out.flush();
#else
    std::vector<Distance>dist(size_t(width)*width*ROUTES);WorkHeap heap;uint16_t values[16];auto started=std::chrono::steady_clock::now();
    size_t source_count=std::size(sources);
#ifdef SRB_ATLAS_TEST_COUNT
    source_count=std::min(source_count,size_t(SRB_ATLAS_TEST_COUNT));
#elif defined(SRB_ATLAS_TEST_ONE)
    source_count=1;
#endif
    for(size_t si=0;si<source_count;++si){
      std::fill(dist.begin(),dist.end(),DIST_INF);heap.clear();seed_port(sources[si],center,width,dist,heap);
      while(!heap.empty()){auto q=heap.pop();if(dist[q.second]!=q.first)continue;int route=q.second%ROUTES,cell=q.second/ROUTES,x=cell%width,y=cell/width;uint16_t input=kRouteToInput[route];for(uint32_t i=kTransitionOffset[input];i<kTransitionOffset[input+1];++i){const auto&e=kTransitions[i];int nx=x+e.dx,ny=y+e.dy;if(nx<0||nx>=width||ny<0||ny>=width)continue;uint32_t next=state_id(nx,ny,e.next_route,width),nd=q.first+e.cost;if(nd<dist[next]){dist[next]=nd;heap.push(nd,next);}}}
      uint64_t source_base=uint64_t(si)*cells*std::size(targets),cell_index=0;
      for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){if(std::max(std::abs(dx),std::abs(dy))<=inner)continue;for(size_t t=0;t<std::size(targets);t+=16){for(int j=0;j<16;++j)values[j]=target_value(dist,width,center,dx,dy,targets[t+j]);pack16_extension(out,values,source_base+cell_index*std::size(targets)+t,exceptions);}++cell_index;}
      double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas extension source=%zu/%zu elapsed=%.1fs",si+1,source_count,sec);std::fflush(stderr);
    }
    std::fprintf(stderr,"\n");h.reserved=uint32_t(exceptions.size());out.raw(exceptions.data(),exceptions.size()*sizeof(ExtException));out.flush();
#endif
  }
  std::fstream patch(path,std::ios::binary|std::ios::in|std::ios::out);if(!patch)throw std::runtime_error("cannot patch extension header");patch.seekp(std::streamoff(header_offset));patch.write(reinterpret_cast<const char*>(&h),sizeof(h));if(!patch)throw std::runtime_error("cannot patch extension header");
}

inline std::string build(const std::string&path){
  using namespace srb_runtime_ids;e2e::Header h{};std::memcpy(h.magic,"SRBE2E1\0",8);
#ifdef SRB_ATLAS_HYBRID_PACK
  h.version=4;
#else
  h.version=2;
#endif
  h.ports=496;h.base_radius=24;h.hot_radius=72;
#ifdef SRB_ATLAS_NO_BASE
  h.sources=0;h.targets=0;
#else
  h.sources=uint32_t(std::size(base_sources));h.targets=uint32_t(std::size(base_targets));
#endif
  h.hot_sources=uint32_t(std::size(hot_sources));h.hot_targets=uint32_t(std::size(hot_targets));h.hot_cells=145*145-49*49;
#ifdef SRB_ATLAS_HYBRID_PACK
  h.reserved=9u|(8u<<8);
#else
  h.reserved=10;
#endif
  h.base_values=uint64_t(h.sources)*49*49*h.targets;h.hot_values=uint64_t(h.hot_sources)*h.hot_cells*h.hot_targets;
#if defined(SRB_BASE_ROUTE_REUSE) && !defined(SRB_ATLAS_NO_BASE)
  uint16_t ordered_base[std::size(base_sources)];std::copy(std::begin(base_sources),std::end(base_sources),ordered_base);
  auto route_key=[](uint16_t p){int r,dx,dy,c;
#ifdef SRB_FULL_BASE_ROUTE_REUSE
    constexpr int reorder_margin=SRB_BASE_MARGIN;
#else
    constexpr int reorder_margin=8;
#endif
    if(single_seed(p,r,dx,dy,c)&&std::abs(dx)<=reorder_margin&&std::abs(dy)<=reorder_margin)return r;return ROUTES+int(p);};
  std::stable_sort(std::begin(ordered_base),std::end(ordered_base),[&](uint16_t a,uint16_t b){return route_key(a)<route_key(b);});
#elif !defined(SRB_ATLAS_NO_BASE)
  const auto&ordered_base=base_sources;
#endif
  {BufferedWriter out(path);out.raw(&h,sizeof(h));
#ifndef SRB_ATLAS_NO_BASE
   out.raw(ordered_base,sizeof(ordered_base));out.raw(base_targets,sizeof(base_targets));
#endif
   out.raw(hot_sources,sizeof(hot_sources));out.raw(hot_targets,sizeof(hot_targets));
#ifdef SRB_ATLAS_HYBRID_PACK
   std::vector<EndpointException>base_exceptions,hot_exceptions;
#ifndef SRB_ATLAS_NO_BASE
#ifdef SRB_PARALLEL_BASE_ATLAS_BUILD
   generate_section_parallel(out,24,-1,ordered_base,base_targets,9,base_exceptions);
#else
   generate_section(out,24,-1,ordered_base,base_targets,9,&base_exceptions);
#endif
#endif
#ifdef SRB_PARALLEL_ATLAS_BUILD
   generate_section_parallel(out,72,24,hot_sources,hot_targets,8,hot_exceptions);
#else
   generate_section(out,72,24,hot_sources,hot_targets,8,&hot_exceptions);
#endif
   EndpointExceptionHeader eh{};std::memcpy(eh.magic,"SRBEXC1",7);eh.version=1;eh.base_count=uint32_t(base_exceptions.size());eh.hot_count=uint32_t(hot_exceptions.size());out.raw(&eh,sizeof(eh));out.raw(base_exceptions.data(),base_exceptions.size()*sizeof(EndpointException));out.raw(hot_exceptions.data(),hot_exceptions.size()*sizeof(EndpointException));
#else
#ifndef SRB_ATLAS_NO_BASE
   generate_section(out,24,-1,ordered_base,base_targets);generate_section(out,72,24,hot_sources,hot_targets);
#else
   generate_section(out,72,24,hot_sources,hot_targets);
#endif
#endif
   out.flush();}
#ifndef SRB_ATLAS_NO_EXTENSION
  append_extension(path);
#endif
  return path;
}
}
