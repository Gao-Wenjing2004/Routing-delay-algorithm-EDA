#include "srb_hybrid.hpp"
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using srb_fast::Slice;
static void trim(Slice& s) {
    while (s.size && (s.data[0]==' ' || s.data[0]=='\t' || s.data[0]=='\r')) { ++s.data; --s.size; }
    while (s.size && (s.data[s.size-1]==' ' || s.data[s.size-1]=='\t' || s.data[s.size-1]=='\r')) --s.size;
}
static bool fields(Slice line, Slice& a, Slice& b) {
    const char* comma = static_cast<const char*>(std::memchr(line.data, ',', line.size));
    if (!comma) return false;
    a = Slice{line.data, static_cast<size_t>(comma-line.data)};
    b = Slice{comma+1, line.size-a.size-1};
    const char* last = static_cast<const char*>(std::memchr(b.data, ',', b.size));
    if (last) b.size = static_cast<size_t>(last-b.data);
    trim(a); trim(b);
    return a.size && b.size;
}
static bool ci(Slice s, const char* t) {
    if (s.size != std::strlen(t)) return false;
    for (size_t i=0;i<s.size;++i) if ((s.data[i] >= 'A' && s.data[i] <= 'Z' ? s.data[i]+32 : s.data[i]) != t[i]) return false;
    return true;
}
static void number(std::string& out, uint32_t value) {
    char digits[16]; char* end=digits+16; char* p=end;
    do { *--p=static_cast<char>('0'+value%10); value/=10; } while(value);
    out.append(p, end-p);
}
static void write_all(FILE* f, const char* data, size_t n) {
    if (n && std::fwrite(data, 1, n, f) != n) throw std::runtime_error("output write failed");
}
struct alignas(64) Worker {
    std::string output;
    uint64_t valid=0, bad=0, local=0;
};

int main(int argc, char** argv) {
    const auto start=std::chrono::steady_clock::now();
    const auto cpu_start=std::clock();
    try {
        std::string in, out;
        std::string atlas=(std::filesystem::path(argv[0]).parent_path()/"endpoint_atlas.bin").string();
        int threads=1;
        int radius=72;
        unsigned max_lookups=32;
        int full_radius=-1;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="-in" && i+1<argc) in=argv[++i];
            else if(arg=="-out" && i+1<argc) out=argv[++i];
            else if(arg=="-atlas" && i+1<argc) atlas=argv[++i];
            else if(arg=="-threads" && i+1<argc) threads=std::stoi(argv[++i]);
            else if(arg=="-radius" && i+1<argc) radius=std::stoi(argv[++i]);
            else if(arg=="-max-lookups" && i+1<argc) max_lookups=std::stoul(argv[++i]);
            else if(arg=="-full-radius" && i+1<argc) full_radius=std::stoi(argv[++i]);
            else throw std::runtime_error("usage: estimate -in request.csv -out result.csv [-atlas file] [-threads N]");
        }
        if(in.empty()||out.empty()||threads!=1||radius<0||radius>72||full_radius < -1||full_radius>72) throw std::runtime_error("single-thread contest: -threads must be 1; radius must be 0..72");
        if(std::filesystem::absolute(in).lexically_normal()==std::filesystem::absolute(out).lexically_normal() ||
           (std::filesystem::exists(out) && std::filesystem::equivalent(in,out))) throw std::runtime_error("input and output must differ");
        using File=std::unique_ptr<FILE, decltype(&std::fclose)>;
        File input(std::fopen(in.c_str(),"rb"), &std::fclose);
        if(!input) throw std::runtime_error("cannot open input");
        srb_v3::HybridEstimator estimator(atlas,radius,max_lookups,full_radius);
        File output(std::fopen(out.c_str(),"wb"), &std::fclose);
        if(!output) throw std::runtime_error("cannot open output");
        std::setvbuf(input.get(),nullptr,_IOFBF,1<<20);
        std::setvbuf(output.get(),nullptr,_IOFBF,1<<20);
        constexpr size_t BLOCK=4u<<20;
        std::vector<char> buffer(BLOCK+4096);
        Worker w;
        w.output.reserve(BLOCK + BLOCK/4);
        const auto stream_start=std::chrono::steady_clock::now();
        const auto stream_cpu_start=std::clock();
        write_all(output.get(),"From,To,Delay\n",sizeof("From,To,Delay\n")-1);
        size_t carry=0; bool first=true;
        uint64_t processed=0,bad=0,local=0;
        while(true) {
            size_t n=std::fread(buffer.data()+carry,1,BLOCK,input.get());
            if(std::ferror(input.get())) throw std::runtime_error("input read failed");
            bool eof=n<BLOCK; size_t total=carry+n;
            if(!total) break;
            size_t pos=0;
            w.output.clear(); w.valid=w.bad=w.local=0;
            while(pos<total) {
                const char* end=static_cast<const char*>(std::memchr(buffer.data()+pos,'\n',total-pos));
                if(!end && !eof) break;
                size_t stop=end ? static_cast<size_t>(end-buffer.data()) : total;
                Slice line{buffer.data()+pos,stop-pos};
                pos=end ? stop+1 : total;
                if(!line.size) continue;
                if(first) {
                    if(line.size>=3 && std::memcmp(line.data,"\xef\xbb\xbf",3)==0) { line.data+=3; line.size-=3; }
                    Slice a,b;
                    first=false;
                    if(fields(line,a,b) && ci(a,"from") && ci(b,"to")) continue;
                }
                    Slice a,b; uint32_t delay=0; bool used_local=false;
                    if(!fields(line,a,b)||!estimator.predict_spec(a,b,delay,used_local)) { ++w.bad; continue; }
                    w.output.append(a.data,a.size); w.output.push_back(',');
                    w.output.append(b.data,b.size); w.output.push_back(',');
                    number(w.output,delay); w.output.push_back('\n');
                    ++w.valid; w.local+=used_local;
            }
            write_all(output.get(),w.output.data(),w.output.size()); processed+=w.valid; bad+=w.bad; local+=w.local;
            carry=total-pos;
            if(carry>4096) throw std::runtime_error("CSV row exceeds 4096 bytes");
            if(carry) std::memmove(buffer.data(),buffer.data()+pos,carry);
            if(eof) break;
        }
        if(std::fflush(output.get())!=0) throw std::runtime_error("output flush failed");
        if(std::fclose(output.release())!=0) throw std::runtime_error("output close failed");
        double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        double stream_elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-stream_start).count();
        std::cerr<<"processed="<<processed<<" bad_rows="<<bad<<" local_queries="<<local<<" fallback_queries="<<processed-local
                 <<" threads="<<threads<<" total_elapsed="<<elapsed<<" stream_elapsed="<<stream_elapsed
                 <<" total_cpu="<<double(std::clock()-cpu_start)/CLOCKS_PER_SEC<<" stream_cpu="<<double(std::clock()-stream_cpu_start)/CLOCKS_PER_SEC
                 <<" atlas_memory_bytes="<<estimator.atlas_memory_bytes()<<'\n';
        return bad?1:0;
    } catch(const std::exception& e) { std::cerr<<"error: "<<e.what()<<'\n'; return 1; }
}
