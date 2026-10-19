#include "estimator.hpp"
#include "io_fast.hpp"
#ifndef SRB_IO_OPTIMIZED
#define SRB_IO_OPTIMIZED 1
#endif
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#ifndef SRB_BATCH_WIDTH
#define SRB_BATCH_WIDTH 4
#endif
static_assert(SRB_BATCH_WIDTH==4||SRB_BATCH_WIDTH==8,"batch width4/8 only");
#ifdef _WIN32
#include <psapi.h>
#endif

using srb_fast::Slice;
static double cpu_seconds(){
#ifdef _WIN32
    FILETIME creation,exit,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))throw std::runtime_error("CPU clock failed");
    ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;return double(k.QuadPart+u.QuadPart)*1e-7;
#else
    return double(std::clock())/CLOCKS_PER_SEC;
#endif
}
static void trim(Slice&s){
    while(s.size&&(s.data[0]==' '||s.data[0]=='\t'||s.data[0]=='\r')){++s.data;--s.size;}
    while(s.size&&(s.data[s.size-1]==' '||s.data[s.size-1]=='\t'||s.data[s.size-1]=='\r'))--s.size;
}
static bool fields(Slice line,Slice&a,Slice&b){
    const char*comma=static_cast<const char*>(std::memchr(line.data,',',line.size));if(!comma)return false;
    a={line.data,size_t(comma-line.data)};b={comma+1,line.size-a.size-1};
    if(const char*last=static_cast<const char*>(std::memchr(b.data,',',b.size)))b.size=size_t(last-b.data);
    trim(a);trim(b);return a.size&&b.size;
}
static bool equal_ignore_case(Slice s,const char*t){
    if(s.size!=std::strlen(t))return false;
    for(size_t i=0;i<s.size;++i)if((s.data[i]>='A'&&s.data[i]<='Z'?s.data[i]+32:s.data[i])!=t[i])return false;
    return true;
}
static void number(std::string&out,uint32_t value){
    if(value==0xffffffffu){out.append("-1");return;}
    char digits[16];char*end=digits+16;char*p=end;
    do{*--p=char('0'+value%10);value/=10;}while(value);out.append(p,end-p);
}
static void write_all(FILE*f,const char*data,size_t n){
    if(n&&std::fwrite(data,1,n,f)!=n)throw std::runtime_error("output write failed");
}

int main(int argc,char**argv){
    const auto started=std::chrono::steady_clock::now();const double cpu_start=cpu_seconds();
    try{
        std::string input_path,output_path;
        std::string atlas_path=(std::filesystem::path(argv[0]).parent_path()/"assets/lower_k32.bin").string();
        int local_radius=24,threads=1,core=-1;bool use_atlas=true;
        for(int i=1;i<argc;++i){const std::string arg=argv[i];
            if(arg=="-in"&&i+1<argc)input_path=argv[++i];
            else if(arg=="-out"&&i+1<argc)output_path=argv[++i];
            else if(arg=="-atlas"&&i+1<argc)atlas_path=argv[++i];
            else if(arg=="-local-radius"&&i+1<argc)local_radius=std::stoi(argv[++i]);
            else if(arg=="-radius"&&i+1<argc){int r=std::stoi(argv[++i]);if(r!=0&&r!=72)throw std::runtime_error("radius must be 0 or 72");use_atlas=r!=0;}
            else if(arg=="-threads"&&i+1<argc)threads=std::stoi(argv[++i]);
            else if(arg=="-core"&&i+1<argc)core=std::stoi(argv[++i]);
            else throw std::runtime_error("usage: estimate -in request.csv -out result.csv");
        }
        if(input_path.empty()||output_path.empty()||threads!=1)throw std::runtime_error("input/output required; single-thread only");
#ifdef _WIN32
        if(core>=0&&(core>=int(sizeof(DWORD_PTR)*8)||!SetProcessAffinityMask(GetCurrentProcess(),DWORD_PTR(1)<<core)))
            throw std::runtime_error("cannot set CPU affinity");
#else
        if(core>=0)throw std::runtime_error("use taskset for Linux CPU affinity");
#endif
        srb_v11_io::protect_same_file(input_path,output_path);
        using File=std::unique_ptr<FILE,decltype(&std::fclose)>;
        File input(std::fopen(input_path.c_str(),"rb"),&std::fclose);if(!input)throw std::runtime_error("cannot open input");
        srb_v11_hybrid::Estimator estimator(atlas_path,local_radius,use_atlas);
        File output(std::fopen(output_path.c_str(),"wb"),&std::fclose);if(!output)throw std::runtime_error("cannot open output");
        std::setvbuf(input.get(),nullptr,_IOFBF,1<<20);std::setvbuf(output.get(),nullptr,_IOFBF,1<<20);
        constexpr size_t BLOCK=4u<<20;std::vector<char>buffer(BLOCK+4096);
#if SRB_IO_OPTIMIZED
        srb_v11_io::OutputBuffer text(BLOCK);
#else
        std::string text;text.reserve(BLOCK+BLOCK/4);
#endif
        size_t carry=0;bool first=true;
        uint64_t count=0,bad=0,branch[5]={0,0,0,0,0};
        struct Pending{srb_fast::Slice from,to;srb_v11_hybrid::Estimator::PreparedRequest query;};
        std::array<Pending,SRB_BATCH_WIDTH>pending;unsigned pending_count=0;
        auto flush_batch=[&](){
            for(unsigned i=0;i<pending_count;++i){uint8_t kind=0;
                const auto&v=pending[i];const uint32_t delay=estimator.predict_prepared(v.query,kind);
#if SRB_IO_OPTIMIZED
                text.row(v.from,v.to,delay);
#else
                text.append(v.from.data,v.from.size);text.push_back(',');text.append(v.to.data,v.to.size);text.push_back(',');
                number(text,delay);text.push_back('\n');
#endif
                ++count;++branch[kind];
            }
            pending_count=0;
        };
        const auto stream_start=std::chrono::steady_clock::now();const double stream_cpu_start=cpu_seconds();
        write_all(output.get(),"From,To,Delay\n",sizeof("From,To,Delay\n")-1);
        while(true){
            const size_t n=std::fread(buffer.data()+carry,1,BLOCK,input.get());
            if(std::ferror(input.get()))throw std::runtime_error("input read failed");
            const bool eof=n<BLOCK;const size_t total=carry+n;if(!total)break;
            size_t pos=0;text.clear();
            while(pos<total){
                const char*end=static_cast<const char*>(std::memchr(buffer.data()+pos,'\n',total-pos));
                if(!end&&!eof)break;const size_t stop=end?size_t(end-buffer.data()):total;
                Slice line{buffer.data()+pos,stop-pos};pos=end?stop+1:total;if(!line.size)continue;
                if(first){first=false;
                    if(line.size>=3&&std::memcmp(line.data,"\xef\xbb\xbf",3)==0){line.data+=3;line.size-=3;}
                    Slice a,b;if(fields(line,a,b)&&equal_ignore_case(a,"from")&&equal_ignore_case(b,"to"))continue;
                }
                Slice a,b;
                if(!fields(line,a,b)||!estimator.prepare_spec(a,b,pending[pending_count].query)){++bad;continue;}
                pending[pending_count].from=a;pending[pending_count].to=b;
                if(++pending_count==SRB_BATCH_WIDTH)flush_batch();
            }
            // Every pending CSV slice must be emitted before buffer reuse.
            flush_batch();
            write_all(output.get(),text.data(),text.size());carry=total-pos;
            if(carry>4096)throw std::runtime_error("CSV row exceeds 4096 bytes");
            if(carry)std::memmove(buffer.data(),buffer.data()+pos,carry);if(eof)break;
        }
        if(std::fflush(output.get()))throw std::runtime_error("output flush failed");
        if(std::fclose(output.release()))throw std::runtime_error("output close failed");
        std::cerr<<"processed="<<count<<" bad_rows="<<bad<<" local_queries="<<branch[1]
                 <<" tail_queries="<<branch[2]<<" normalized_queries="<<branch[0]<<" identity_queries="<<branch[3]<<" lower_queries="<<branch[4]
                 <<" threads=1 total_elapsed="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()
                 <<" stream_elapsed="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-stream_start).count()
                 <<" total_cpu="<<cpu_seconds()-cpu_start<<" stream_cpu="<<cpu_seconds()-stream_cpu_start
                 <<" atlas_memory_bytes="<<estimator.atlas_memory_bytes();
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS memory{};if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)))std::cerr<<" peak_rss_bytes="<<memory.PeakWorkingSetSize;
#endif
        std::cerr<<'\n';return bad?1:0;
    }catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<'\n';return 1;}
}
