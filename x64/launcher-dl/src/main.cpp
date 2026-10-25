// SPDX-License-Identifier: GPL-2.0-only
// Steam content client (anonymous, refresh-token or SteamClient QR login). No Valve binary loading or game API emulation.
#include "pb.h"
#include "vdf.h"
#include "net.h"
#include "crypto.h"
#include <windows.h>
#include <wincrypt.h>
#include <zlib.h>
extern "C" {
#include "LzmaDec.h"
size_t ZSTD_decompress(void*,size_t,const void*,size_t);
unsigned ZSTD_isError(size_t);
}
#include <algorithm>
#include <deque>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void need(bool b, const std::string& s) { if (!b) throw std::runtime_error(s); }
static uint32_t le(const std::string& s, size_t p) {
    need(p <= s.size() && s.size()-p >= 4, "truncated uint32");
    uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)(unsigned char)s[p+i]<<(8*i); return v;
}
static void put32(std::string& s,uint32_t v) { for(int i=0;i<4;i++) s+=(char)(v>>(8*i)); }
static Pb proto(const std::string& s) { Pb p(s); need(p.ok,"malformed protobuf"); return p; }
static std::string inflateData(const std::string& s, size_t expected, int window) {
    need(expected <= 128*1024*1024,"inflate exceeds limit");
    std::string out(expected,'\0'); z_stream z{};
    z.next_in=(Bytef*)s.data(); z.avail_in=s.size(); z.next_out=(Bytef*)out.data(); z.avail_out=out.size();
    need(inflateInit2(&z,window)==Z_OK,"inflate init");
    int r=inflate(&z,Z_FINISH); inflateEnd(&z);
    need(r==Z_STREAM_END && z.total_out==expected,"inflate size/stream mismatch"); return out;
}
static std::string unzip(const std::string& s) {
    need(s.size()>=30 && le(s,0)==0x04034b50,"expected ZIP local header");
    unsigned method=(unsigned char)s[8] | (unsigned char)s[9]<<8;
    size_t n=le(s,18),orig=le(s,22),off=30+(unsigned char)s[26]+((unsigned char)s[27]<<8)+(unsigned char)s[28]+((unsigned char)s[29]<<8);
    need(off<=s.size() && n<=s.size()-off,"truncated ZIP");
    std::string out;
    if(method==0) {out=s.substr(off,n); need(out.size()==orig,"ZIP size");}
    else {need(method==8,"unsupported ZIP method"); out=inflateData(s.substr(off,n),orig,-15);}
    need(crc32(0,(const Bytef*)out.data(),out.size())==le(s,14),"ZIP CRC mismatch"); return out;
}
static void* allocLz(ISzAllocPtr,size_t n){return malloc(n);} static void freeLz(ISzAllocPtr,void* p){free(p);}
static std::string decompress(const std::string& s, size_t expected) {
    if(s.compare(0,4,"VSZa")==0) {
        need(s.size()>=23 && s.substr(s.size()-3)=="zsv","bad VSZa framing");
        need(expected<=16*1024*1024 && le(s,s.size()-11)==expected,"VSZa size");
        std::string out(expected,'\0');
        size_t n=ZSTD_decompress(out.data(),out.size(),s.data()+8,s.size()-23);
        need(!ZSTD_isError(n)&&n==expected,"Zstd failed");
        need(crc32(0,(const Bytef*)out.data(),out.size())==le(s,s.size()-15),"VSZa CRC mismatch");return out;
    }
    if(s.compare(0,3,"VZa")==0) {
        need(s.size()>=22 && s.substr(s.size()-2)=="zv","bad VZip framing");
        need(le(s,s.size()-6)==expected && expected<=16*1024*1024,"VZip size");
        std::string out(expected,'\0'); SizeT n=expected, in=s.size()-22;
        ISzAlloc a{allocLz,freeLz}; ELzmaStatus status;
        int r=LzmaDecode((Byte*)out.data(),&n,(const Byte*)s.data()+12,&in,(const Byte*)s.data()+7,5,LZMA_FINISH_END,&status,&a);
        need(r==SZ_OK && n==expected,"LZMA failed");
        need(crc32(0,(const Bytef*)out.data(),out.size())==le(s,s.size()-10),"VZip CRC mismatch"); return out;
    }
    need(s.compare(0,4,"PK\3\4")==0,"unsupported chunk compression header="+hexLower(s.substr(0,8)));
    return unzip(s); // Unknown formats fail closed; never accept undecoded bytes.
}
struct Auth {
    std::string account, token;
    uint64_t steamid=0;
    ~Auth(){ if(!token.empty()) SecureZeroMemory(token.data(),token.size()); }
};
static void readAuth(const char* path, Auth& a) {
    std::ifstream f(std::filesystem::u8path(path),std::ios::binary);
    need(bool(f),"open private SteamClient token input");
    std::string platform,id;
    need(bool(std::getline(f,platform))&&bool(std::getline(f,a.account))&&
         bool(std::getline(f,id))&&bool(std::getline(f,a.token)),"invalid private token input");
    need(platform=="SteamClient"&&!a.account.empty()&&a.account.size()<128&&
         !id.empty()&&id.find_first_not_of("0123456789")==std::string::npos&&
         !a.token.empty()&&a.token.size()<16384,"invalid SteamClient token fields");
    a.steamid=std::stoull(id);
}
struct Msg {uint32_t id; Pb header,body;};
class Client {
    std::unique_ptr<WebSocket> ws=makeWebSocket();
    uint64_t sid=(uint64_t(1)<<56)|(uint64_t(10)<<52),job=1;
    uint32_t session=0;
    std::deque<Msg> queue;
    ULONGLONG beat=0;
    bool logged=false;
    void unpack(const std::string& s, int depth=0) {
        need(depth<8,"nested Multi limit"); uint32_t id=le(s,0);
        // Legacy account notifications may accompany a real-user logon. They
        // are not responses used by this downloader; required replies still
        // must be protobuf and pass their job/EResult/content checks.
        if(!(id&0x80000000)){
            need(id==782||id==798,"unsupported legacy CM message id="+std::to_string(id));
            std::cout<<"CM ignored legacy notification id="<<id<<std::endl;return;
        }
        id&=0x7fffffff;
        size_t n=le(s,4); need(n<=s.size()-8,"header overflow");
        Pb h=proto(s.substr(8,n)),b=proto(s.substr(8+n));
        if(id==1){
            std::string bytes=b.s(2); if(b.u(1)) bytes=inflateData(bytes,b.u(1),31);
            size_t i=0; while(i<bytes.size()){size_t k=le(bytes,i);i+=4;need(k<=bytes.size()-i,"Multi overflow");unpack(bytes.substr(i,k),depth+1);i+=k;} return;
        }
        if(id==751) {sid=h.u(1,sid);session=h.u(2,session);}
        queue.push_back({id,std::move(h),std::move(b)});
    }
public:
    void send(uint32_t id,const PbW& body,uint64_t source=0,const std::string& method="") {
        PbW h; bool nonAuthed=id==9804||id==9805;
        h.f64(1,nonAuthed?0:sid).u(2,nonAuthed?0:session);
        if(id==9804)h.u(32,1);
        if(source)h.f64(10,source);
        if(!method.empty())h.s(12,method);
        std::string s; put32(s,id|0x80000000);put32(s,h.b.size());s+=h.b;s+=body.b;need(ws->send(s),"WebSocket send failed");
    }
    void pump(int timeoutMs=1000) {
        if(logged&&GetTickCount64()-beat>10000){send(703,PbW());beat=GetTickCount64();}
        std::string s; int r=ws->recv(s,timeoutMs);need(r>=0,"CM connection closed");if(r)unpack(s);
    }
    Msg wait(uint32_t id,uint64_t target=0) {
        ULONGLONG start=GetTickCount64();
        for(;;){for(auto i=queue.begin();i!=queue.end();++i)if(i->id==id && (!target||i->header.u(11)==target)){Msg m=std::move(*i);queue.erase(i);return m;}
            need(GetTickCount64()-start<60000,"CM response timeout id="+std::to_string(id));pump();}
    }
    Msg request(uint32_t id,uint32_t reply,const PbW& b,const std::string& method="") {uint64_t j=job++;send(id,b,j,method);return wait(reply,j);}
    void connect(Http& http) {
        HttpResult r; need(http.get("api.steampowered.com","/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets&format=vdf",r)&&r.status==200,"CM directory: "+r.err);
        Kv root; need(VdfParser(r.body).parse(root),"CM directory VDF"); auto list=root.child("serverlist");need(list,"missing CM list");
        bool connected=false; std::string err;
        for(auto& x:list->kids){std::string ep=x.str("endpoint");size_t colon=ep.rfind(':');if(colon==std::string::npos||ep.substr(colon)!=":443")continue;
            std::cout<<"CM wss://"<<ep<<"/cmsocket/"<<std::endl;
            if(ws->connect(ep.substr(0,colon),443,"/cmsocket/",err)){connected=true;break;}ws=makeWebSocket();}
        need(connected,"CM connect: "+err);
    }
    void login(Auth* auth=nullptr) {
        if(auth) sid=auth->steamid ? auth->steamid : ((uint64_t(1)<<56)|(uint64_t(1)<<52)|uint64_t(1)<<32);
        PbW b;b.u(1,65580).u(7,20).s(6,"english").u(102,1);
        if(auth) b.s(50,auth->account).s(108,auth->token).u(8,0).u(100,0x423533);
        send(5514,b);beat=GetTickCount64();
        Msg m=wait(751);logged=m.body.u(1)==1;need(logged,"logon EResult="+std::to_string(m.body.u(1)));if(auth) { auth->steamid=sid; std::cout<<"SteamClient logged on"<<std::endl; }
        else std::cout<<"ANONYMOUS logged on"<<std::endl;
    }
    void qr(Auth& auth) {
        send(9805,PbW().u(1,65580));
        PbW device;device.s(1,"Xbox Developer Mode launcher").u(2,1).u(3,20);
        PbW begin;begin.s(1,"Xbox Developer Mode launcher").u(2,1).s(3,device.b).s(4,"Client");
        auto m=request(9804,147,begin,"Authentication.BeginAuthSessionViaQR#1");
        need(m.header.u(13)==1,"QR begin denied EResult="+std::to_string(m.header.u(13)));
        uint64_t client=m.body.u(1);std::string requestId=m.body.s(3);
        auto show=[](const std::string& url){
            need(url.rfind("https://s.team/q/",0)==0&&url.size()<512&&
                 url.find_first_of("\r\n") == std::string::npos,"invalid QR challenge URL");
            std::cout<<url<<std::endl;
        };
        need(client&&!requestId.empty(),"QR response incomplete");show(m.body.s(2));
        float interval=5;auto field=m.body.get(4);
        if(field&&field->wt==5){uint32_t bits=field->v;std::memcpy(&interval,&bits,4);}
        need(std::isfinite(interval)&&interval>=1&&interval<=30,"invalid QR poll interval");
        ULONGLONG deadline=GetTickCount64()+180000;
        while(GetTickCount64()<deadline){
            ULONGLONG next=GetTickCount64()+DWORD(interval*1000);
            while(GetTickCount64()<next) pump(100);
            PbW poll;poll.u(1,client).s(2,requestId);
            auto status=request(9804,147,poll,"Authentication.PollAuthSessionStatus#1");
            need(status.header.u(13)==1,"QR poll denied EResult="+std::to_string(status.header.u(13)));
            std::cerr<<"QR poll acknowledged"<<std::endl;
            if(status.body.u(1))client=status.body.u(1);
            if(!status.body.s(2).empty())show(status.body.s(2));
            if(!status.body.s(3).empty()){
                auth.token=status.body.s(3);auth.account=status.body.s(6);
                need(!auth.account.empty()&&auth.token.size()<16384,"QR auth incomplete");
                return; // Token remains in memory only; never emitted or persisted.
            }
        }
        throw std::runtime_error("QR confirmation timed out");
    }
    ~Client(){ws->close();}
};
static std::string get(Http& h,const std::string& path) {
    HttpResult r;need(h.get("steampipe.akamaized.net",path,r),"CDN HTTPS: "+r.err);need(r.status==200,"CDN HTTP "+std::to_string(r.status));return r.body;
}
static std::string base64(const std::string& s) {
    DWORD n=0;need(CryptStringToBinaryA(s.c_str(),s.size(),CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr),"filename base64");
    std::string r(n,'\0');need(CryptStringToBinaryA(s.c_str(),s.size(),CRYPT_STRING_BASE64,(BYTE*)r.data(),&n,nullptr,nullptr),"filename base64");r.resize(n);return r;
}
static std::filesystem::path safePath(const std::filesystem::path& root,std::string name) {
    std::replace(name.begin(),name.end(),'\\','/');need(!name.empty()&&name[0]!='/'&&name.find(':')==std::string::npos,"unsafe manifest path");
    auto p=std::filesystem::u8path(name);for(auto& c:p) {
        auto part=c.u8string();
        need(c!=".."&&c!="."&&!part.empty()&&part.back()!='.'&&part.back()!=' ',"manifest traversal/alias");
        need(part.find_first_of("<>\"|?*") == std::string::npos && std::none_of(part.begin(),part.end(),[](unsigned char b){return b<32;}),"invalid manifest filename");
    }
    auto r=root;for(auto& c:p){r/=c;need(!std::filesystem::is_symlink(std::filesystem::symlink_status(r)),"output symlink");}return r;
}
int main(int argc,char** argv) {
    std::cout.setf(std::ios::unitbuf);
    try {
        need(argc==2||argc==3||argc==4,"usage: steamdl.exe <fresh-output-directory> [--qr | --token-file <private-input>]");
        bool owned=argc>2;bool qr=argc==3&&std::string(argv[2])=="--qr";
        need(!owned||qr||(argc==4&&std::string(argv[2])=="--token-file"),"invalid authentication arguments");
        uint32_t appid=owned?3405340:1007;
        Auth auth;if(owned&&!qr)readAuth(argv[3],auth);
        auto out=std::filesystem::u8path(argv[1]);need(!std::filesystem::exists(out),"output must not exist (fresh download required)");std::filesystem::create_directories(out);
        auto http=makeHttp();Client c;c.connect(*http);
        if(qr)c.qr(auth);
        c.login(owned?&auth:nullptr);
        uint64_t appToken=0;
        if(owned){PbW r;r.u(2,appid);auto t=c.request(8905,8906,r);
            for(auto f:t.body.all(3)){auto p=proto(f->s);if(p.u(1)==appid)appToken=p.u(2);}}
        PbW app;app.u(1,appid);if(appToken)app.u(2,appToken);PbW req;req.s(2,app.b);
        auto info=c.request(8903,8904,req);std::string vdf;
        for(auto f:info.body.all(1)){Pb p=proto(f->s);if(p.u(1)==appid)vdf=p.s(5);}
        Kv root;need(VdfParser(vdf).parse(root),"appinfo missing/invalid");const Kv* depots=root.child("depots");need(depots,"appinfo depots missing");
        std::ofstream evidence(out/"manifest-sha1.tsv",std::ios::binary);need(bool(evidence),"open evidence");
        size_t files=0,depotCount=0;uint64_t bytes=0;
        for(auto& d:depots->kids){if(d.key.empty()||d.key.find_first_not_of("0123456789")!=std::string::npos)continue;
            auto config=d.child("config");if(config){auto os=config->str("oslist");if(!os.empty()&&os.find("windows")==std::string::npos)continue;}
            if(config){auto lang=config->str("language");if(!lang.empty()&&lang!="english")continue;}
            need(d.str("dlcappid").empty(),"DLC depot requires separate ownership selection");
            auto manifests=d.child("manifests");auto pub=manifests?manifests->child("public"):nullptr;if(!pub)continue;
            uint32_t depot=std::stoul(d.key);uint64_t gid=std::stoull(pub->obj?pub->str("gid"):pub->val);depotCount++;
            PbW keyReq;keyReq.u(1,depot).u(2,appid);auto kr=c.request(5438,5439,keyReq);need(kr.body.u(1)==1&&kr.body.u(2)==depot,"depot key denied");std::string key=kr.body.s(3);need(key.size()==32,"bad depot key");
            PbW codeReq;codeReq.u(1,appid).u(2,depot).u(3,gid).s(4,"public");auto code=c.request(151,147,codeReq,"ContentServerDirectory.GetManifestRequestCode#1");need(code.header.u(13)==1&&code.body.u(1),"manifest request denied");
            std::string prefix="/depot/"+d.key;
            std::cout<<"CDN https://steampipe.akamaized.net"<<prefix<<" manifest="<<gid<<std::endl;
            std::string packed=get(*http,prefix+"/manifest/"+std::to_string(gid)+"/5/"+std::to_string(code.body.u(1)));
            {std::ofstream f(out/(d.key+".manifest.download"),std::ios::binary);f.write(packed.data(),packed.size());}
            std::string raw=le(packed,0)==0x71f617d0?packed:unzip(packed);
            {std::ofstream f(out/(d.key+"-"+std::to_string(gid)+".manifest"),std::ios::binary);f.write(raw.data(),raw.size());need(bool(f),"write manifest");}
            Pb payload,metadata;size_t pos=0;bool end=false;
            while(pos<raw.size()){uint32_t magic=le(raw,pos);pos+=4;if(magic==0x32c415ab){end=true;break;}size_t n=le(raw,pos);pos+=4;need(n<=raw.size()-pos,"manifest bounds");
                if(magic==0x71f617d0)payload=proto(raw.substr(pos,n));else if(magic==0x1f4812be)metadata=proto(raw.substr(pos,n));else need(magic==0x1b81b817,"manifest section");pos+=n;}
            need(end&&payload.ok&&metadata.ok&&metadata.u(1)==depot&&metadata.u(2)==gid,"manifest identity/framing");
            auto depotRoot=out/d.key;std::filesystem::create_directories(depotRoot);
            for(auto f:payload.all(1)) {Pb file=proto(f->s);std::string name=file.s(1);
                if(metadata.u(4)){std::string decoded;need(steamSymmetricDecrypt(key,base64(name),decoded),"filename decrypt");name=decoded.substr(0,decoded.find('\0'));}
                auto path=safePath(depotRoot,name);need(!(file.u(3)&512)&&file.s(7).empty(),"manifest symlink unsupported");
                if(file.u(3)&64){std::filesystem::create_directories(path);continue;}
                uint64_t size=file.u(2);need(size<=256*1024*1024,"file size limit");std::string data(size,'\0');uint64_t covered=0;
                auto chunks=file.all(6);std::sort(chunks.begin(),chunks.end(),[](const PbField* a,const PbField* b){return proto(a->s).u(3)<proto(b->s).u(3);});
                for(auto chunk:chunks){Pb ch=proto(chunk->s);uint64_t offset=ch.u(3),n=ch.u(4);need(offset==covered&&offset<=size&&n<=size-offset&&ch.s(1).size()==20,"chunk bounds/hash");
                    std::string enc=get(*http,prefix+"/chunk/"+hexLower(ch.s(1))),compressed;
                    need(steamSymmetricDecrypt(key,enc,compressed),"chunk decrypt");auto plain=decompress(compressed,n);need(plain.size()==n&&sha1(plain)==ch.s(1),"chunk SHA-1 mismatch");data.replace(offset,n,plain);covered+=n;c.pump(0);}
                need(covered==size&&file.s(5).size()==20&&sha1(data)==file.s(5),"file SHA-1 mismatch");std::filesystem::create_directories(path.parent_path());
                {std::ofstream dst(path,std::ios::binary);dst.write(data.data(),data.size());need(bool(dst),"write content");}
                bool ok;auto diskHash=sha1File(path.wstring(),ok);need(ok&&diskHash==file.s(5)&&std::filesystem::file_size(path)==size,"disk SHA-1/size mismatch");
                evidence<<depot<<'\t'<<gid<<'\t'<<hexLower(file.s(5))<<'\t'<<size<<'\t'<<name<<'\n';need(bool(evidence),"write evidence");
                std::cout<<"SHA1 "<<hexLower(diskHash)<<" "<<size<<" "<<name<<std::endl;files++;bytes+=size;
            }
        }
        need(depotCount>0&&files>0,"no files selected");evidence.close();need(bool(evidence),"close evidence");
        std::cout<<"VERIFIED app="<<appid<<" depots="<<depotCount<<" files="<<files<<" bytes="<<bytes<<" transport=desktop-WinHTTP cdn=steampipe.akamaized.net"<<std::endl;return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
