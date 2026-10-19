#pragma once
#include "srb_fast.hpp"

// All 496 architecture port names have a fixed grammar. Decode that grammar to
// one unique array slot, retaining the existing architecture port IDs.
class DirectPortParser {
public:
    DirectPortParser(){
        std::fill(ids_,ids_+496,-1);
        for(int i=0;i<srb_fast_data::kPortCount;++i){const char* n=srb_fast_data::kSortedPortNames[i];int key=slot({n,std::strlen(n)});
            if(key<0||ids_[key]>=0)throw std::runtime_error("port grammar is not bijective");ids_[key]=srb_fast_data::kSortedPortIds[i];}
        for(int id:ids_)if(id<0)throw std::runtime_error("port grammar coverage incomplete");
    }
    bool parse_endpoint(srb_fast::Slice s,srb_fast::Endpoint& e)const {
        while(s.size&&(s.data[0]==' '||s.data[0]=='\t'||s.data[0]=='\r')){++s.data;--s.size;}
        while(s.size&&(s.data[s.size-1]==' '||s.data[s.size-1]=='\t'||s.data[s.size-1]=='\r'))--s.size;
        if(s.size<9||std::memcmp(s.data,"SRB_",4))return false;
        size_t p=4;int x,y;if(!integer(s,p,x)||p>=s.size||s.data[p++]!='_'||!integer(s,p,y)||p>=s.size||s.data[p++]!='/'||x>=120||y>=550)return false;
        int key=slot({s.data+p,s.size-p});if(key<0)return false;e.x=x;e.y=y;e.port=ids_[key];return true;
    }
private:
    int ids_[496];
    static bool integer(srb_fast::Slice s,size_t&p,int&v){v=0;if(p>=s.size||s.data[p]<'0'||s.data[p]>'9')return false;
        do{if(v>100000)return false;v=v*10+s.data[p++]-'0';}while(p<s.size&&s.data[p]>='0'&&s.data[p]<='9');return true;}
    static int direction(char c){switch(c){case 'E':return 0;case 'N':return 1;case 'S':return 2;case 'W':return 3;default:return -1;}}
    static int slot(srb_fast::Slice s){
        if(s.size==4||s.size==5){if((s.data[0]!='A'&&s.data[0]!='S')||s.data[1]!='_'||s.data[2]<'A'||s.data[2]>'H')return -1;
            int pin=-1;if(s.size==5){if(s.data[3]=='Q'&&(s.data[4]=='0'||s.data[4]=='1'))pin=9+s.data[4]-'0';}
            else{switch(s.data[3]){case '0':case '1':case '2':case '3':case '4':case '5':pin=s.data[3]-'0';break;case 'D':pin=6;break;case 'M':pin=7;break;case 'Y':pin=8;break;}}
            return pin<0?-1:(s.data[0]=='S'?88:0)+(s.data[2]-'A')*11+pin;
        }
        if((s.size!=6&&s.size!=7)||(s.data[0]!='I'&&s.data[0]!='Z'))return -1;
        int dir=direction(s.data[2]);if(dir<0)return -1;int sub=-1;
        if(s.size==6){int kind=s.data[1]=='D'?0:s.data[1]=='L'?1:s.data[1]=='Q'?2:-1;if(kind<0||s.data[3]!='['||s.data[5]!=']'||s.data[4]<'0'||s.data[4]>'7')return -1;sub=kind*4+dir;}
        else{if(s.data[1]!='S'||(s.data[3]!='A'&&s.data[3]!='B')||s.data[4]!='['||s.data[6]!=']'||s.data[5]<'0'||s.data[5]>'7')return -1;sub=12+dir*2+s.data[3]-'A';}
        return 176+(s.data[0]=='Z'?160:0)+sub*8+s.data[s.size-2]-'0';
    }
};
