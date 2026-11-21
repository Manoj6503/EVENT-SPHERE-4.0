#pragma once
// EventSphere core: security helpers, database wrapper, user hierarchy, PDF builder and form validation.
// Kept in a header (no globals, no networking) so backend/tests/test_core.cpp can unit-test it.
#include "httplib.h"
#include "nlohmann/json.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <map>
#include <set>
#include <functional>
#include <filesystem>
#include <thread>
#include <cstring>
#include <ctime>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include <curl/curl.h>
#include <array>
#include <algorithm>
#include <cctype>
using namespace std; using json=nlohmann::json; using namespace httplib;
struct HttpErr{int code;string msg;};
// ---- security helpers ----
string hex(const unsigned char*d,size_t n){static const char*h="0123456789abcdef";string s;for(size_t i=0;i<n;i++){s+=h[d[i]>>4];s+=h[d[i]&15];}return s;}
string sha256hex(const string&s){unsigned char o[32];unsigned n=0;EVP_Digest(s.data(),s.size(),o,&n,EVP_sha256(),nullptr);return hex(o,32);} // sessions are stored hashed
string rnd(int n){vector<unsigned char>b(n);RAND_bytes(b.data(),n);return hex(b.data(),n);}
string hashPw(const string&pw,const string&salt){unsigned char o[32];PKCS5_PBKDF2_HMAC(pw.c_str(),pw.size(),(const unsigned char*)salt.data(),salt.size(),100000,EVP_sha256(),32,o);return salt+"$"+hex(o,32);}
bool checkPw(const string&pw,const string&st){string h=hashPw(pw,st.substr(0,st.find('$')));return h.size()==st.size()&&CRYPTO_memcmp(h.data(),st.data(),h.size())==0;} // constant-time compare
// ---- RAII database wrapper (parameterized queries only) ----
class Db{sqlite3*d=nullptr;public:
 explicit Db(const string&p){if(sqlite3_open(p.c_str(),&d))throw runtime_error("cannot open db");}
 ~Db(){sqlite3_close(d);} Db(const Db&)=delete; Db&operator=(const Db&)=delete;
 void exec(const string&sql){char*e=nullptr;if(sqlite3_exec(d,sql.c_str(),0,0,&e)){string m=e;sqlite3_free(e);throw runtime_error(m);}}
 json q(const string&sql,const vector<string>&p={}){sqlite3_stmt*s;if(sqlite3_prepare_v2(d,sql.c_str(),-1,&s,0))throw runtime_error(sqlite3_errmsg(d));
  for(size_t i=0;i<p.size();i++)sqlite3_bind_text(s,i+1,p[i].c_str(),-1,SQLITE_TRANSIENT);
  json r=json::array();int rc;
  while((rc=sqlite3_step(s))==SQLITE_ROW){json o;for(int c=0;c<sqlite3_column_count(s);c++){string n=sqlite3_column_name(s,c);int t=sqlite3_column_type(s,c);
   if(t==SQLITE_INTEGER)o[n]=sqlite3_column_int64(s,c);else if(t==SQLITE_NULL)o[n]=nullptr;else o[n]=string((const char*)sqlite3_column_text(s,c));}r.push_back(o);}
  sqlite3_finalize(s);if(rc!=SQLITE_DONE)throw runtime_error(sqlite3_errmsg(d));return r;}
 long long lastId(){return sqlite3_last_insert_rowid(d);}
 // consistent online copy of the whole database (SQLite backup API), safe while the server is running
 void backupTo(const string&path){sqlite3*dst=nullptr;if(sqlite3_open(path.c_str(),&dst)){sqlite3_close(dst);throw runtime_error("cannot open backup file");}
  sqlite3_backup*b=sqlite3_backup_init(dst,"main",d,"main");if(!b){string m=sqlite3_errmsg(dst);sqlite3_close(dst);throw runtime_error(m);}
  sqlite3_backup_step(b,-1);int rc=sqlite3_backup_finish(b);sqlite3_close(dst);if(rc!=SQLITE_OK)throw runtime_error("backup failed");}
};
// ---- User hierarchy: runtime polymorphism for permissions ----
struct User{long long id;string name,email,role;virtual ~User()=default;
 virtual bool canReview()const{return false;} virtual bool canManageEvents()const{return false;} virtual bool canRegister()const{return false;}};
struct Student:User{bool canRegister()const override{return true;}};
struct Organizer:User{bool canManageEvents()const override{return true;}};
struct Admin:User{bool canReview()const override{return true;} bool canManageEvents()const override{return true;}};
unique_ptr<User> makeUser(const json&j){unique_ptr<User>u;string r=j["role"];
 if(r=="admin")u=make_unique<Admin>();else if(r=="organizer")u=make_unique<Organizer>();else u=make_unique<Student>();
 u->id=j["id"];u->name=j["name"];u->email=j["email"];u->role=r;return u;}

string lowerStr(string s){for(auto&c:s)c=(char)tolower((unsigned char)c);return s;}
// real calendar check (the old code only tested that the text was 10 characters long)
bool validDate(const string&s){if(s.size()!=10||s[4]!='-'||s[7]!='-')return false;for(int i=0;i<10;i++)if(i!=4&&i!=7&&!isdigit((unsigned char)s[i]))return false;
 int y=stoi(s.substr(0,4)),m=stoi(s.substr(5,2)),d=stoi(s.substr(8,2));if(y<2000||m<1||m>12||d<1)return false;
 static const int dm[]={31,28,31,30,31,30,31,31,30,31,30,31};bool leap=(y%4==0&&y%100!=0)||y%400==0;return d<=dm[m-1]+(m==2&&leap?1:0);}
// reads the sid cookie exactly (the old code matched any cookie whose name merely contained "sid=")
string getSid(const string&c){size_t p=0;while((p=c.find("sid=",p))!=string::npos){if(p==0||c[p-1]==' '||c[p-1]==';'){string t=c.substr(p+4,64);size_t e=t.find(';');if(e!=string::npos)t=t.substr(0,e);return t;}p+=4;}return "";}
string genv(const char*k,const char*d=""){const char*v=getenv(k);return string(v?v:d);}
string b64(const string&s){string o(4*((s.size()+2)/3)+1,'\0');int n=EVP_EncodeBlock((unsigned char*)&o[0],(const unsigned char*)s.data(),s.size());o.resize(n);string r;for(size_t i=0;i<o.size();i+=76)r+=o.substr(i,76)+"\r\n";return r;}
string pdfEsc(const string&s){string o;for(unsigned char c:s){if(c>126||c<32)o+='?';else{if(c=='('||c==')'||c=='\\')o+='\\';o+=(char)c;}}return o;}
string unb64(const string&in){string c;for(char ch:in)if(isalnum((unsigned char)ch)||ch=='+'||ch=='/'||ch=='=')c+=ch;if(c.empty()||c.size()%4)return "";
 string o(c.size()/4*3,'\0');int n=EVP_DecodeBlock((unsigned char*)&o[0],(const unsigned char*)c.data(),c.size());if(n<0)return "";
 size_t pad=(c.back()=='=')+(c.size()>1&&c[c.size()-2]=='=');if((size_t)n<pad)return "";o.resize(n-pad);return o;}
// Reads width/height/components from a JPEG header; false if the bytes are not a usable JPEG
bool jpegInfo(const string&j,int&w,int&h,int&comp){auto B=[&](size_t i){return (int)(unsigned char)j[i];};if(j.size()<12||B(0)!=0xFF||B(1)!=0xD8)return false;size_t i=2;
 while(i+9<j.size()){if(B(i)!=0xFF){i++;continue;}int m=B(i+1);if(m==0xFF){i++;continue;}if(m==0xD8||m==0x01||(m>=0xD0&&m<=0xD7)){i+=2;continue;}
  if(m>=0xC0&&m<=0xCF&&m!=0xC4&&m!=0xC8&&m!=0xCC){h=(B(i+5)<<8)|B(i+6);w=(B(i+7)<<8)|B(i+8);comp=B(i+9);return w>0&&h>0&&w<=8000&&h<=8000&&(comp==1||comp==3);}
  i+=2+((B(i+2)<<8)|B(i+3));}return false;}
// Helvetica / Helvetica-Bold advance widths (1/1000 em, ASCII 32..126) so centred text is really centred
double textW(const string&t,bool bold){static const int R[95]={278,278,355,556,556,889,667,191,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,278,278,584,584,584,556,1015,667,667,722,722,667,611,778,722,278,500,667,556,833,722,778,667,778,722,667,611,722,667,944,667,667,611,278,278,278,469,556,333,556,556,500,556,556,278,556,556,222,222,500,222,833,556,556,556,556,333,500,278,556,500,722,500,500,500,334,260,334,584},B[95]={278,333,474,556,556,889,722,238,333,333,389,584,278,333,278,278,556,556,556,556,556,556,556,556,556,556,333,333,584,584,584,611,975,722,722,722,722,667,611,778,722,278,556,722,611,833,722,778,667,778,722,667,611,722,667,944,667,667,611,333,278,333,584,556,333,556,611,556,611,556,333,611,611,278,278,556,278,889,611,611,611,611,389,556,333,611,556,778,556,556,500,389,280,389,584};double w=0;for(unsigned char c:t)w+=(c<32||c>126)?556:(bold?B[c-32]:R[c-32]);return w/1000.0;}
// Builds a one-page landscape PDF certificate by hand (no external PDF library needed).
// The signature block only ever shows what the admin configured: authority name, designation and (optionally) the uploaded signature image.
string makePdf(const string&name,const string&ev,const string&date,const string&cid,const string&college,const string&issued,const string&authName,const string&authTitle,const string&sigB64){
 ostringstream c;string jpg=unb64(sigB64);int iw=0,ih=0,ic=0;bool img=!jpg.empty()&&jpegInfo(jpg,iw,ih,ic);
 auto T=[&](const string&t,double cx,double y,double sz,const char*f,double r,double g,double b){bool bd=f[1]=='2';double fit=textW(t,bd);if(fit*sz>640)sz=640/fit;double w=fit*sz;c<<"BT /"<<f<<" "<<sz<<" Tf "<<r<<" "<<g<<" "<<b<<" rg "<<(cx-w/2)<<" "<<y<<" Td ("<<pdfEsc(t)<<") Tj ET\n";};
 c<<"0.43 0.16 0.85 rg 24 507 794 64 re f\n0.31 0.27 0.9 RG 4 w 24 24 794 547 re S\n0.93 0.28 0.6 RG 1.5 w 34 34 774 527 re S\n";
 T(college,421,532,22,"F2",1,1,1);
 T("CERTIFICATE OF PARTICIPATION",421,458,34,"F2",0.06,0.1,0.24);
 T("This is to certify that",421,415,16,"F1",0.3,0.3,0.4);
 T(name,421,362,36,"F2",0.43,0.16,0.85);
 T("has successfully participated in",421,318,16,"F1",0.3,0.3,0.4);
 T(ev,421,272,26,"F2",0.06,0.1,0.24);
 T("held on "+date,421,238,16,"F1",0.3,0.3,0.4);
 if(img){double s=min(180.0/iw,62.0/ih),dw=iw*s,dh=ih*s;c<<"q "<<dw<<" 0 0 "<<dh<<" "<<(220-dw/2)<<" 128 cm /Im1 Do Q\n";}
 c<<"0.2 w 0 0 0 RG 110 125 m 330 125 l S 512 125 m 732 125 l S\n";
 T(authName,220,108,13,"F2",0.06,0.1,0.24);T(authTitle,220,92,11,"F1",0.3,0.3,0.4);
 T(issued,622,132,13,"F2",0.06,0.1,0.24);T("Date of Issue",622,108,12,"F1",0.3,0.3,0.4);
 T("Certificate ID: "+cid,421,58,11,"F1",0.4,0.4,0.5);
 string s=c.str(),out="%PDF-1.4\n";vector<size_t> off;
 auto obj=[&](const string&b){off.push_back(out.size());out+=to_string(off.size())+" 0 obj\n"+b+"\nendobj\n";};
 obj("<</Type/Catalog/Pages 2 0 R>>");obj("<</Type/Pages/Kids[3 0 R]/Count 1>>");
 obj(string("<</Type/Page/Parent 2 0 R/MediaBox[0 0 842 595]/Resources<</Font<</F1 4 0 R/F2 5 0 R>>")+(img?"/XObject<</Im1 7 0 R>>":"")+">>/Contents 6 0 R>>");
 obj("<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>");obj("<</Type/Font/Subtype/Type1/BaseFont/Helvetica-Bold/Encoding/WinAnsiEncoding>>");
 obj("<</Length "+to_string(s.size())+">>\nstream\n"+s+"endstream");
 if(img)obj("<</Type/XObject/Subtype/Image/Width "+to_string(iw)+"/Height "+to_string(ih)+"/ColorSpace/"+(ic==1?"DeviceGray":"DeviceRGB")+"/BitsPerComponent 8/Filter/DCTDecode/Length "+to_string(jpg.size())+">>\nstream\n"+jpg+"\nendstream");
 size_t x=out.size();out+="xref\n0 "+to_string(off.size()+1)+"\n0000000000 65535 f \n";char buf[32];for(size_t o:off){snprintf(buf,32,"%010zu 00000 n \n",o);out+=buf;}
 out+="trailer\n<</Size "+to_string(off.size()+1)+"/Root 1 0 R>>\nstartxref\n"+to_string(x)+"\n%%EOF\n";return out;}
// ---- registration forms: one template per event category (+ organizer's own extra questions) ----
json FD(const char*k,const char*l,const char*t,bool req,json opts=json::array(),int mn=0,int mx=0){json f={{"key",k},{"label",l},{"type",t},{"required",req}};if(!opts.empty())f["options"]=opts;if(mx){f["min"]=mn;f["max"]=mx;}return f;}
const json& commonFields(){static const json c=json::array({FD("roll_no","Roll / Register number","text",true),FD("department","Department","text",true),
 FD("year","Year of study","select",true,json::array({"1st year","2nd year","3rd year","4th year","5th year / PG"})),FD("phone","Mobile number","tel",true)});return c;}
const map<string,json>& formTemplates(){static const map<string,json> T=[]{map<string,json> m;json IT=json::array({"Individual","Team"});
 m["Technical"]=json::array({FD("participation","Participation type","select",true,IT),FD("team_name","Team name (if team)","text",false),FD("team_size","Team size","number",false,json::array(),1,10),
  FD("skill_level","Skill level","select",true,json::array({"Beginner","Intermediate","Advanced"})),FD("tech_known","Technologies you know","text",false),FD("portfolio","GitHub / portfolio link","text",false)});
 m["Workshop"]=json::array({FD("prior_experience","Prior experience with the topic","select",true,json::array({"None","Basic","Intermediate","Advanced"})),
  FD("laptop","Will you bring a laptop?","select",true,json::array({"Yes","No"})),FD("expectations","What do you want to learn?","textarea",false)});
 m["Cultural"]=json::array({FD("performance_type","Performance type","select",true,json::array({"Dance","Singing","Instrumental","Drama / Skit","Fine arts","Other"})),
  FD("item_title","Title of your item / performance","text",true),FD("group_size","Number of performers","number",true,json::array(),1,50),FD("duration","Duration (minutes)","number",true,json::array(),1,30),
  FD("stage_needs","Stage / sound / props required","textarea",false)});
 m["Symposium"]=json::array({FD("college","College / Institution","text",true),FD("role","Participating as","select",true,json::array({"Attendee","Paper presenter","Poster presenter"})),
  FD("paper_title","Paper / poster title (presenters only)","text",false),FD("abstract","Short abstract (presenters only)","textarea",false),FD("co_authors","Co-authors","text",false)});
 m["Sports"]=json::array({FD("team_category","Category","select",true,json::array({"Men","Women","Mixed"})),FD("participation","Participation type","select",true,IT),FD("team_name","Team name (if team)","text",false),
  FD("position","Preferred position / role","text",false),FD("jersey_size","Jersey size","select",true,json::array({"S","M","L","XL","XXL"})),FD("emergency_contact","Emergency contact number","tel",true),
  FD("fitness","I declare that I am medically fit to participate","checkbox",true)});
 m["Seminar"]=json::array({FD("heard_from","How did you hear about this seminar?","select",false,json::array({"Poster","Friend","Social media","Faculty","Other"})),FD("questions","Questions / topics you would like covered","textarea",false)});
 m["Hackathon"]=json::array({FD("team_name","Team name","text",true),FD("team_size","Team size","number",true,json::array(),1,6),FD("team_members","Team members (names and roll numbers)","textarea",true),
  FD("tech_stack","Planned tech stack","text",false),FD("hack_experience","Hackathon experience","select",true,json::array({"First hackathon","1-3 hackathons","4 or more"}))});
 m["Other"]=json::array({FD("notes","Anything the organizer should know?","textarea",false)});return m;}();return T;}
// Organizer-defined extra questions: sanitised and given server-generated keys (x1, x2, ...)
string normalizeCustom(const string&raw){json in=raw.empty()?json::array():json::parse(raw,nullptr,false);if(in.is_discarded()||!in.is_array())throw HttpErr{400,"Invalid custom questions"};
 if(in.size()>8)throw HttpErr{400,"At most 8 custom questions are allowed"};json out=json::array();int n=0;
 for(auto&q:in){if(!q.is_object())continue;string l=q.value("label",string()),t=q.value("type",string("text"));while(!l.empty()&&l.back()==' ')l.pop_back();if(l.empty())continue;
  if(l.size()>80)throw HttpErr{400,"Custom question text is too long"};
  if(t!="text"&&t!="textarea"&&t!="select"&&t!="number"&&t!="tel"&&t!="checkbox")throw HttpErr{400,"Invalid custom question type"};
  json f={{"key","x"+to_string(++n)},{"label",l},{"type",t},{"required",q.value("required",false)==true}};
  if(t=="select"){json o=json::array();if(q.contains("options")&&q["options"].is_array())for(auto&x:q["options"])if(x.is_string()&&!x.get<string>().empty()&&x.get<string>().size()<=40)o.push_back(x);
   if(o.size()<2||o.size()>12)throw HttpErr{400,"Dropdown questions need 2-12 options"};f["options"]=o;}
  out.push_back(f);}return out.dump();}
// Final field list for an event: common student details + category (or chosen) template + custom questions + declaration
json formFields(const string&category,const string&formType,const string&custom){auto&T=formTemplates();string k=formType.empty()?category:formType;auto it=T.find(k);if(it==T.end())it=T.find("Other");
 json all=commonFields();for(auto&f:it->second)all.push_back(f);json c=json::parse(custom.empty()?"[]":custom,nullptr,false);if(c.is_array())for(auto&f:c)all.push_back(f);
 all.push_back(FD("declaration","I confirm that the details above are correct","checkbox",true));return all;}
// Validates a student's submitted answers against the event's form; returns [{key,label,value}] in form order
json validateAnswers(const json&fields,const json&ans){json out=json::array();
 for(auto&f:fields){string k=f["key"],lab=f["label"],t=f["type"];bool req=f.value("required",false);string v;
  if(ans.is_object()&&ans.contains(k)){auto&x=ans[k];if(x.is_string())v=x.get<string>();else if(x.is_boolean())v=x.get<bool>()?"Yes":"";else if(x.is_number())v=x.dump();}
  size_t a=v.find_first_not_of(" \t\r\n"),z=v.find_last_not_of(" \t\r\n");v=a==string::npos?"":v.substr(a,z-a+1);
  if(t=="checkbox")v=(v=="Yes"||v=="true")?"Yes":"";
  if(v.empty()){if(req)throw HttpErr{400,lab+" is required"};continue;}
  if(v.size()>(t=="textarea"?600u:150u))throw HttpErr{400,lab+" is too long"};
  if(t=="select"){bool ok=false;for(auto&o:f["options"])if(o==v)ok=true;if(!ok)throw HttpErr{400,lab+": please choose one of the listed options"};}
  else if(t=="number"){char*e=nullptr;long n=strtol(v.c_str(),&e,10);if(*e||v.size()>9)throw HttpErr{400,lab+" must be a whole number"};
   if(f.contains("max")&&(n<f["min"].get<long>()||n>f["max"].get<long>()))throw HttpErr{400,lab+" must be between "+to_string(f["min"].get<int>())+" and "+to_string(f["max"].get<int>())};v=to_string(n);}
  else if(t=="tel"){int d=0;for(char ch:v){if(isdigit((unsigned char)ch))d++;else if(!strchr("+- ()",ch))throw HttpErr{400,lab+" must be a valid phone number"};}if(d<7||d>15)throw HttpErr{400,lab+" must be a valid phone number"};}
  out.push_back({{"key",k},{"label",lab},{"value",v}});}
 return out;}
