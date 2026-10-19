#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace srb_v11_io {
inline constexpr char pairs[]=
    "00010203040506070809" "10111213141516171819" "20212223242526272829"
    "30313233343536373839" "40414243444546474849" "50515253545556575859"
    "60616263646566676869" "70717273747576777879" "80818283848586878889"
    "90919293949596979899";
inline char* integer(char*out,uint32_t value){
    if(value==UINT32_MAX){*out++='-';*out++='1';return out;}
    if(value<10){*out++=char('0'+value);return out;}
    if(value<100){std::memcpy(out,pairs+value*2,2);return out+2;}
    if(value<10000){const unsigned hi=value/100,lo=value-hi*100;
        if(hi<10)*out++=char('0'+hi);else{std::memcpy(out,pairs+hi*2,2);out+=2;}
        std::memcpy(out,pairs+lo*2,2);return out+2;
    }
    char temp[16];char*end=temp+sizeof(temp),*p=end;
    while(value>=100){const unsigned hi=value/100,lo=value-hi*100;p-=2;std::memcpy(p,pairs+lo*2,2);value=hi;}
    if(value<10)*--p=char('0'+value);else{p-=2;std::memcpy(p,pairs+value*2,2);}
    const size_t n=size_t(end-p);std::memcpy(out,p,n);return out+n;
}

class OutputBuffer {
public:
    // A valid endpoint is at least12 bytes. Per-row output adds at most12 bytes,
    // so 2*input bytes exceeds worst output size, including a no-LF final row.
    // The original reader permits at most4096 carried bytes per block.
    explicit OutputBuffer(size_t block):buffer_(new char[2*(block+4096)]),end_(buffer_.get()){}
    void clear(){end_=buffer_.get();}
    void row(srb_fast::Slice a,srb_fast::Slice b,uint32_t delay){
        char*p=end_;
        if(a.data+a.size+1==b.data&&a.data[a.size]==','){
            const size_t n=a.size+1+b.size;std::memcpy(p,a.data,n);p+=n;
        }else{
            std::memcpy(p,a.data,a.size);p+=a.size;*p++=',';std::memcpy(p,b.data,b.size);p+=b.size;
        }
        *p++=',';p=integer(p,delay);*p++='\n';end_=p;
    }
    const char*data()const{return buffer_.get();}
    size_t size()const{return size_t(end_-buffer_.get());}
private:
    std::unique_ptr<char[]>buffer_;char*end_;
};

inline bool strict_nul(const std::string&path){
    return path.size()==3&&(path[0]=='N'||path[0]=='n')&&(path[1]=='U'||path[1]=='u')&&(path[2]=='L'||path[2]=='l');
}
inline void protect_same_file(const std::string&input,const std::string&output){
    namespace fs=std::filesystem;
    if(fs::absolute(input).lexically_normal()==fs::absolute(output).lexically_normal())throw std::runtime_error("input and output must differ");
    // Retain original fail-closed behavior for ordinary output files, including
    // hardlink, symlink and case aliases. Only a bare Windows NUL output may
    // bypass the library's unsupported-device equivalence query.
    if(fs::exists(output)){
        std::error_code ec;const bool equal=fs::equivalent(input,output,ec);
        if(equal)throw std::runtime_error("input and output must differ");
        if(ec){
#ifdef _WIN32
            std::error_code kind_error;
            if(strict_nul(output)&&fs::is_regular_file(input,kind_error)&&!kind_error)return;
#endif
            throw fs::filesystem_error("cannot check file equivalence",input,output,ec);
        }
    }
}
}
