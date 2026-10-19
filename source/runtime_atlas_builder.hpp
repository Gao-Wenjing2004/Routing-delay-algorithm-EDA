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
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace srb_runtime_atlas {
using namespace srb_local_arch;
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

inline void pack16(BufferedWriter&out,const uint16_t*v){
  constexpr unsigned BITS=10,MASK=(1u<<BITS)-1;uint16_t lo=INF16;
  for(int i=0;i<16;++i)if(v[i]!=INF16)lo=std::min(lo,v[i]);if(lo==INF16)lo=0;
  uint8_t record[22]{};std::memcpy(record,&lo,2);
  for(unsigned i=0;i<16;++i){uint32_t d=v[i]==INF16?MASK:uint32_t(v[i]-lo);if(d>=MASK)d=MASK;unsigned bit=i*BITS,byte=2+bit/8,shift=bit%8;uint32_t word=d<<shift;record[byte]|=uint8_t(word);record[byte+1]|=uint8_t(word>>8);if(byte+2<22)record[byte+2]|=uint8_t(word>>16);}
  out.bytes(record,sizeof(record));
}

#pragma pack(push,1)
struct ExtHeader{char magic[8];uint32_t version,inner,radius,cells,sources,targets,bits,reserved;uint64_t values;};
struct ExtException{uint32_t index;uint16_t value;};
#pragma pack(pop)

inline void pack16_extension(BufferedWriter&out,const uint16_t*v,uint64_t base,std::vector<ExtException>&exceptions){
  constexpr unsigned MASK=255;uint16_t lo=INF16;for(int i=0;i<16;++i)if(v[i]!=INF16)lo=std::min(lo,v[i]);if(lo==INF16)lo=0;uint8_t record[18]{};std::memcpy(record,&lo,2);
  for(int i=0;i<16;++i){uint32_t d=v[i]==INF16?MASK:uint32_t(v[i]-lo);if(d>=MASK){record[2+i]=MASK;exceptions.push_back({uint32_t(base+i),v[i]});}else record[2+i]=uint8_t(d);}out.bytes(record,sizeof(record));
}

template<size_t NS,size_t NT>
inline void generate_section(BufferedWriter&out,int radius,int inner,const uint16_t(&sources)[NS],const uint16_t(&targets)[NT]){
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
  size_t source_count=NS;
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
    for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx){if(std::max(std::abs(dx),std::abs(dy))<=inner)continue;for(size_t t=0;t<NT;t+=16){for(int j=0;j<16;++j){uint16_t v=target_value(*field,width,center,dx-sdx,dy-sdy,targets[t+j]);values[j]=v==INF16?v:uint16_t(std::min<unsigned>(INF16-1,unsigned(v)+unsigned(scost)));}pack16(out,values);}}
    double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();std::fprintf(stderr,"\ratlas radius=%d source=%zu/%zu elapsed=%.1fs",radius,si+1,source_count,sec);std::fflush(stderr);
  }
  std::fprintf(stderr,"\n");
}

inline uint64_t file_size(const std::string&path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("cannot stat runtime Atlas");return uint64_t(f.tellg());}

inline void append_extension(const std::string&path){
#ifndef SRB_EXTENSION_MARGIN
#define SRB_EXTENSION_MARGIN 8
#endif
  using namespace srb_runtime_ext_ids;constexpr int inner=72,radius=104,margin=SRB_EXTENSION_MARGIN,width=2*(radius+margin)+1,center=radius+margin;constexpr uint32_t cells=209*209-145*145;
  ExtHeader h{};std::memcpy(h.magic,"SRBEXT1",7);h.version=2;h.inner=inner;h.radius=radius;h.cells=cells;h.sources=uint32_t(std::size(sources));h.targets=uint32_t(std::size(targets));h.bits=8;h.values=uint64_t(h.sources)*cells*h.targets;
  uint64_t header_offset=file_size(path);std::vector<ExtException>exceptions;
  {
    BufferedWriter out(path,true);out.raw(&h,sizeof(h));out.raw(sources,sizeof(sources));out.raw(targets,sizeof(targets));
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
  }
  std::fstream patch(path,std::ios::binary|std::ios::in|std::ios::out);if(!patch)throw std::runtime_error("cannot patch extension header");patch.seekp(std::streamoff(header_offset));patch.write(reinterpret_cast<const char*>(&h),sizeof(h));if(!patch)throw std::runtime_error("cannot patch extension header");
}

inline std::string build(const std::string&path){
  using namespace srb_runtime_ids;e2e::Header h{};std::memcpy(h.magic,"SRBE2E1\0",8);h.version=2;h.ports=496;h.base_radius=24;h.hot_radius=72;h.sources=uint32_t(std::size(base_sources));h.targets=uint32_t(std::size(base_targets));h.hot_sources=uint32_t(std::size(hot_sources));h.hot_targets=uint32_t(std::size(hot_targets));h.hot_cells=145*145-49*49;h.reserved=10;h.base_values=uint64_t(h.sources)*49*49*h.targets;h.hot_values=uint64_t(h.hot_sources)*h.hot_cells*h.hot_targets;
#ifdef SRB_BASE_ROUTE_REUSE
  uint16_t ordered_base[std::size(base_sources)];std::copy(std::begin(base_sources),std::end(base_sources),ordered_base);
  auto route_key=[](uint16_t p){int r,dx,dy,c;if(single_seed(p,r,dx,dy,c)&&std::abs(dx)<=8&&std::abs(dy)<=8)return r;return ROUTES+int(p);};
  std::stable_sort(std::begin(ordered_base),std::end(ordered_base),[&](uint16_t a,uint16_t b){return route_key(a)<route_key(b);});
#else
  const auto&ordered_base=base_sources;
#endif
  {BufferedWriter out(path);out.raw(&h,sizeof(h));out.raw(ordered_base,sizeof(ordered_base));out.raw(base_targets,sizeof(base_targets));out.raw(hot_sources,sizeof(hot_sources));out.raw(hot_targets,sizeof(hot_targets));generate_section(out,24,-1,ordered_base,base_targets);generate_section(out,72,24,hot_sources,hot_targets);out.flush();}
#ifndef SRB_ATLAS_NO_EXTENSION
  append_extension(path);
#endif
  return path;
}
}
