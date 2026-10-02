#include "reaction-suggestion.hpp"
#include "arrow-path.hpp"
#include "curved-arrow.hpp"
#include "placement-runtime.hpp"
#include <xmllite.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <numeric>

namespace cd {
namespace {
using Microsoft::WRL::ComPtr;
// ChemDraw CDXML coordinates are points; CDAtom::WriteCDX divides world units
// by this same native constant. Camera scale and DPI never enter the chemistry.
double documentUnits() {return *reinterpret_cast<const double*>(base+0x899eb0);}
struct Xml {
    std::string name,text;
    std::map<std::string,std::string> a;
    std::vector<Xml> children;
    std::string get(const char* key,const char* fallback="") const {
        auto i=a.find(key);return i==a.end()?fallback:i->second;
    }
};
std::string narrow(const wchar_t* p,UINT n) {
    if(!p||!n)return {};
    int bytes=WideCharToMultiByte(CP_UTF8,0,p,int(n),nullptr,0,nullptr,nullptr);
    std::string s(size_t(bytes),'\0');WideCharToMultiByte(CP_UTF8,0,p,int(n),s.data(),bytes,nullptr,nullptr);return s;
}
std::wstring wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    std::wstring w(size_t(n),L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),w.data(),n);return w;
}
Xml parse(std::string s) {
    // The native exporter emits an external DTD declaration. No network access
    // or external entities are needed to read the explicit exported properties.
    auto d=s.find("<!DOCTYPE");if(d!=std::string::npos) {
        auto end=s.find('>',d);if(end==std::string::npos)throw std::runtime_error("CDXML DTD");s.erase(d,end-d+1);
    }
    ComPtr<IStream> stream;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)))throw std::bad_alloc();
    ULONG written{};if(FAILED(stream->Write(s.data(),ULONG(s.size()),&written))||written!=s.size())throw std::runtime_error("CDXML stream");
    LARGE_INTEGER zero{};stream->Seek(zero,STREAM_SEEK_SET,nullptr);
    ComPtr<IXmlReader> reader;
    if(FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr)))throw std::runtime_error("CDXML reader");
    reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit);
    reader->SetProperty(XmlReaderProperty_MaxElementDepth,64);
    reader->SetInput(stream.Get());
    Xml document;std::vector<Xml*> stack{&document};XmlNodeType type{};HRESULT status{};size_t count{};
    while((status=reader->Read(&type))==S_OK) {
        const wchar_t* p{};UINT n{};
        if(type==XmlNodeType_Element) {
            if(++count>50000)throw std::runtime_error("CDXML size");
            Xml node;reader->GetLocalName(&p,&n);node.name=narrow(p,n);
            const bool empty=reader->IsEmptyElement()!=FALSE;
            if(reader->MoveToFirstAttribute()==S_OK)do {
                reader->GetLocalName(&p,&n);auto key=narrow(p,n);reader->GetValue(&p,&n);node.a.emplace(std::move(key),narrow(p,n));
            } while(reader->MoveToNextAttribute()==S_OK);
            reader->MoveToElement();stack.back()->children.push_back(std::move(node));
            if(!empty)stack.push_back(&stack.back()->children.back());
        } else if(type==XmlNodeType_EndElement) {
            if(stack.size()<2)throw std::runtime_error("CDXML nesting");stack.pop_back();
        } else if(type==XmlNodeType_Text||type==XmlNodeType_CDATA||type==XmlNodeType_Whitespace) {
            reader->GetValue(&p,&n);stack.back()->text+=narrow(p,n);
        }
    }
    if(FAILED(status)||document.children.size()!=1||document.children[0].name!="CDXML")throw std::runtime_error("CDXML document");
    return std::move(document.children[0]);
}
std::string escape(const std::string& s) {
    std::string r;for(char c:s)switch(c) {case '&':r+="&amp;";break;case '<':r+="&lt;";break;case '>':r+="&gt;";break;case '"':r+="&quot;";break;default:r+=c;}return r;
}
void write(const Xml& x,std::ostream& out) {
    out<<'<'<<x.name;for(const auto& [k,v]:x.a)out<<' '<<k<<"=\""<<escape(v)<<'"';
    if(x.text.empty()&&x.children.empty()){out<<"/>";return;}
    out<<'>'<<escape(x.text);for(const auto& child:x.children)write(child,out);out<<"</"<<x.name<<'>';
}
std::string number(double value) {std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(12)<<value;return s.str();}
int integer(const std::string& s,int fallback=0) {
    if(s.empty())return fallback;size_t consumed{};int v=std::stoi(s,&consumed);if(consumed!=s.size())throw std::runtime_error("CDXML integer");return v;
}
Point point(const std::string& s) {
    std::istringstream in(s);in.imbue(std::locale::classic());Point p{};
    if(!(in>>p.x>>p.y)||!std::isfinite(p.x+p.y))throw std::runtime_error("CDXML position");return p;
}
std::string text(const Xml& x) {std::string s=x.text;for(const auto& c:x.children)s+=text(c);return s;}
uint64_t hash(const std::string& s) {uint64_t h=1469598103934665603ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
struct Arrow {
    int id{},source{-1},target{-1},head{},tail{};
    Point start{},end{};
    bool reaction{},incomplete{},curved{};
};
struct Step {int arrow{};std::set<int> reactants;bool products{};};
struct BondInk {double gap{},width{};};
struct Input {
    std::string xml;
    std::vector<Arrow> arrows;
    std::vector<Step> steps;
    std::map<int,int> nativeOrders;
    std::map<int,BondInk> nativeInk;
    double units{},length{},width{};
    double spacing{},minimumGapWidths{};
    RectD paper{};
    uint64_t token{},pageIdentity{};
    HWND window{};
};
uint64_t contentKey(const Input& input) {
    std::string bytes=input.xml;
    auto add=[&](const auto& value){bytes.append(reinterpret_cast<const char*>(&value),sizeof(value));};
    for(const auto& a:input.arrows) {
        add(a.id);add(a.source);add(a.target);add(a.head);add(a.tail);add(a.start);add(a.end);add(a.reaction);add(a.incomplete);add(a.curved);
    }
    for(const auto& [id,order]:input.nativeOrders){add(id);add(order);}
    for(const auto& [id,ink]:input.nativeInk){add(id);add(ink.gap);add(ink.width);}
    add(input.spacing);add(input.minimumGapWidths);
    for(const auto& step:input.steps){add(step.arrow);add(step.products);for(int id:step.reactants)add(id);int end=-1;add(end);}
    return hash(bytes);
}
struct Product {int arrow{};std::string xml;std::shared_ptr<ReactionPreview> preview;std::string reason;};
struct Result {uint64_t token{},pageIdentity{},key{};std::vector<Product> products;std::string acceptedXml;};
struct Job {Input input;Result result;std::atomic<bool> ready{},cancelled{};};
std::mutex jobsMutex;std::condition_variable jobsEvent;
std::shared_ptr<Job> pendingJob;
std::once_flag workerOnce;
struct State {
    uint64_t token{1},identity{},lastCapture{},lastMutation{},dismissedKey{};
    bool dirty{true};
    std::shared_ptr<Job> job;
    std::shared_ptr<const Result> result;
    std::shared_ptr<const ReactionPreview> preview;
    std::map<int,uint64_t> arrowKeys;
};
std::unordered_map<Obj,State> states; // UI-only, bounded; never passed to worker.
thread_local unsigned observing{};
struct Observe {
    alignas(8) std::byte undo[16]{};
    explicit Observe(Obj doc) {++observing;fn<void(*)(void*,Obj)>(0x501fd0)(undo,doc);}
    ~Observe(){fn<void(*)(void*)>(0x5023f0)(undo);--observing;}
};
using Unary=void(*)(Obj);
Unary oldAtomModified{},oldBondModified{};
void atomModified(Obj o){oldAtomModified(o);reactionObjectChanged(o);}
void bondModified(Obj o){oldBondModified(o);reactionObjectChanged(o);}
uint64_t arrowKey(Obj arrow) {
    std::string bytes;
    auto add=[&](const auto& value){bytes.append(reinterpret_cast<const char*>(&value),sizeof(value));};
    for(size_t offset:{size_t(0x1f0),size_t(0x1f4),size_t(0x120),size_t(0x124)})add(at<int>(arrow,offset));
    add(at<double>(arrow,0x130));
    Point tail{},head{};if(arrowWorldEndpoints(arrow,tail,head)){add(tail);add(head);}
    add(standardArrowAttachments(arrow));add(mechanismArrowAttachments(arrow));return hash(bytes);
}
struct Node {Node *left,*parent,*right;uint8_t color,nil,pad[6];Obj object;};
template<class F> void each(Obj page,F action) {
    Node* head=at<Node*>(page,0xc8);size_t count=at<size_t>(page,0xd0);
    if(!head||count>16000)throw std::runtime_error("Reaction page size");
    for(Node* n=head->left;n!=head&&count--;) {
        action(n->object);
        if(!n->right->nil){n=n->right;while(!n->left->nil)n=n->left;}
        else{Node* p=n->parent;while(p!=head&&n==p->right){n=p;p=p->parent;}n=p;}
    }
}
Input capture(Obj doc,uint64_t token) {
    Obj page=mainPage(doc);Input input;input.token=token;input.pageIdentity=at<uint64_t>(page,0xb0);
    Obj port=at<Obj>(doc,0x258);input.window=port?portWindow(port):nullptr;
    input.units=documentUnits();
    input.paper=at<RectD>(doc,0x230);
    input.paper={input.paper.t/input.units,input.paper.l/input.units,input.paper.b/input.units,input.paper.r/input.units};
    auto setting=[&](int id,double fallback){Obj value=fn<Obj(*)(Obj,int)>(0x7128f0)(page,id);return value?fn<double(*)(Obj)>(0x37be40)(value)/input.units:fallback;};
    input.length=setting(0x805,14.4);input.width=setting(0x807,.6);
    Obj spacing=fn<Obj(*)(Obj,int)>(0x7128f0)(page,0x804);
    input.spacing=spacing?fn<double(*)(Obj)>(0x37be40)(spacing):.18;
    input.minimumGapWidths=*reinterpret_cast<const double*>(base+0x8973b8)*.5;
    Observe off(doc);
    each(page,[&](Obj o) {
        if(!o||at<uint8_t>(o,0x30)||!at<uint8_t>(o,0x34))return;
        if(at<uintptr_t>(o,0)==base+0x8babc0) {
            // Native benzene retains explicit Kekule orders even when its
            // aromatic export represents every edge with Order="1.5".
            const int id=at<int>(o,0x24);
            input.nativeOrders[id]=int(fn<unsigned(*)(Obj)>(0xa2300)(o));
            input.nativeInk[id]={fn<double(*)(Obj)>(0x2ede60)(o)/input.units,
                fn<double(*)(Obj)>(0x2ed660)(o)/input.units};return;
        }
        if(at<uintptr_t>(o,0)!=base+0x8b2cf0)return;
        Arrow a;a.id=at<int>(o,0x24);a.source=at<int>(o,0x1f0);a.target=at<int>(o,0x1f4);
        a.head=at<int>(o,0x120);a.tail=at<int>(o,0x124);
        if(!arrowWorldEndpoints(o,a.start,a.end))return;
        a.start.x/=input.units;a.start.y/=input.units;a.end.x/=input.units;a.end.y/=input.units;
        // The shim's ordinary arrows can have attachment IDs too. Their
        // explicit palette role takes precedence over IsElectronPushArrow.
        a.curved=std::abs(at<double>(o,0x130))>1e-7;
        // Palette ownership is not semantic reaction ownership. Curved full
        // arrows drawn with the ordinary palette also represent electron flow.
        a.reaction=!a.curved&&!mechanismArrowAttachments(o)&&
            (standardArrowAttachments(o)||(a.source<0&&a.target<0));
        // Only single forward full heads are executable paired-electron arrows.
        // Fishhooks, reversible and bidirectional arrows are not silently paired.
        a.incomplete=!a.reaction&&(a.head!=2||a.tail!=1);
        if(a.reaction&&(a.head!=2||a.tail!=1||std::abs(at<double>(o,0x130))>1e-7))return;
        input.arrows.push_back(a);
    });
    bool mechanism=false,reaction=false;
    for(const auto& a:input.arrows){reaction|=a.reaction;mechanism|=!a.reaction;}
    if(!mechanism||!reaction)return input;
    Obj selection=fn<Obj(*)(Obj)>(0x147490)(doc);
    Obj interpreter=fn<Obj(*)(Obj)>(0x256ef0)(selection);
    const Vec* steps=interpreter?fn<const Vec*(*)(Obj)>(0x494200)(interpreter):nullptr;
    if(steps&&steps->first&&steps->last>=steps->first&&size_t(steps->last-steps->first)<=0xd8*64&&size_t(steps->last-steps->first)%0xd8==0) {
        for(auto p=steps->first;p!=steps->last;p+=0xd8) {
            Step s;Obj arrow=fn<Obj(*)(Obj)>(0x095d00)(p);if(!arrow)continue;s.arrow=at<int>(arrow,0x24);
            const Vec* reactants=fn<const Vec*(*)(Obj)>(0x017150)(p);
            const Vec* products=fn<const Vec*(*)(Obj)>(0x022750)(p);
            if(reactants&&reactants->first&&reactants->last>=reactants->first&&size_t(reactants->last-reactants->first)<=sizeof(Obj)*4096)
                for(auto q=reinterpret_cast<Obj*>(reactants->first);q!=reinterpret_cast<Obj*>(reactants->last);++q)if(*q)s.reactants.insert(at<int>(*q,0x24));
            s.products=products&&products->first!=products->last;input.steps.push_back(std::move(s));
        }
    }
    alignas(8) std::byte generator[8]{};
    fn<std::string*(*)(Obj,std::string*,Obj)>(0x10e6f0)(generator,&input.xml,selection);
    if(input.xml.size()>8*1024*1024)throw std::runtime_error("Reaction CDXML size");
    return input;
}
struct Atom {Xml node;int id{},fragment{},element{},charge{},hydrogens{};Point p{};bool stereo{};};
struct Bond {Xml node;int id{},a{},b{},order{};bool stereo{};};
struct Graph {std::vector<Atom> atoms;std::vector<Bond> bonds;std::map<int,size_t> atomIDs,bondIDs;std::map<int,std::set<int>> ancestors;};
void readGraph(const Xml& x,Graph& graph,int fragment=0,std::set<int> ancestors={}) {
    int id=integer(x.get("id"));
    if(x.name=="group"||x.name=="fragment")ancestors.insert(id);
    if(x.name=="fragment")fragment=id;
    if(x.name=="n") {
        Atom a;a.node=x;a.id=id;a.fragment=fragment;a.element=integer(x.get("Element"),6);a.charge=integer(x.get("Charge"));
        a.hydrogens=integer(x.get("NumHydrogens"),-1);a.p=point(x.get("p"));
        a.stereo=x.get("AS")!=""&&x.get("AS")!="U"&&x.get("AS")!="N";
        graph.atomIDs.emplace(id,graph.atoms.size());graph.ancestors[id]=ancestors;graph.atoms.push_back(std::move(a));
    } else if(x.name=="b") {
        Bond b;b.node=x;b.id=id;b.a=integer(x.get("B"));b.b=integer(x.get("E"));
        const auto order=x.get("Order","1");b.order=order=="1"?1:order=="2"?2:order=="3"?3:-1;
        b.stereo=x.get("Display")!=""&&x.get("Display")!="Solid";
        graph.bondIDs.emplace(id,graph.bonds.size());graph.bonds.push_back(std::move(b));
    } else for(const auto& c:x.children)readGraph(c,graph,fragment,ancestors);
}
int valence(int element,int charge) {
    switch(element) {
    case 1:return charge==0?1:(std::abs(charge)==1?0:-1);
    case 5:return charge==0?3:(charge==-1?4:-1);
    case 6:return charge==0?4:(std::abs(charge)==1?3:-1);
    case 7:return charge==0?3:(charge==1?4:(charge==-1?2:-1));
    case 8:return charge==0?2:(charge==1?3:(charge==-1?1:-1));
    case 9:case 17:case 35:case 53:return charge==0?1:(charge==-1?0:-1);
    default:return -1;
    }
}
const char* symbol(int element) {
    switch(element){case 1:return "H";case 5:return "B";case 6:return "C";case 7:return "N";case 8:return "O";case 9:return "F";case 17:return "Cl";case 35:return "Br";case 53:return "I";default:return "?";}
}
bool supported(const Atom& a) {
    if(valence(a.element,a.charge)<0||!a.fragment||a.node.get("NodeType","Element")!="Element"||
        (a.node.get("Radical")!=""&&a.node.get("Radical")!="None")||a.node.get("GenericNickname")!=""||a.node.get("AtomList")!=""||a.node.get("Attachments")!="")return false;
    // Preserve real element labels, but do not interpret contracted groups,
    // query atoms, residues or formula labels as a single electron donor.
    for(const auto& child:a.node.children)if(child.name!="t"&&child.name!="objecttag")return false;
    return true;
}
Point plus(Point a,Point b){return {a.x+b.x,a.y+b.y,0};}
Point minus(Point a,Point b){return {a.x-b.x,a.y-b.y,0};}
Point times(Point a,double x){return {a.x*x,a.y*x,0};}
double dot(Point a,Point b){return a.x*b.x+a.y*b.y;}
struct Bounds {
    double l{INFINITY},t{INFINITY},r{-INFINITY},b{-INFINITY};
    void add(Point p){l=std::min(l,p.x);r=std::max(r,p.x);t=std::min(t,p.y);b=std::max(b,p.y);}
};
void stripIDs(Xml& x) {x.a.erase("id");x.a.erase("Z");for(auto& c:x.children)stripIDs(c);}
double segmentDistance(Point p,Point a,Point b,double& fraction) {
    Point d=minus(b,a);double squared=dot(d,d);
    fraction=squared>1e-10?std::clamp(dot(minus(p,a),d)/squared,0.0,1.0):0;
    Point q=minus(p,plus(a,times(d,fraction)));return std::hypot(q.x,q.y);
}
void resolveMechanismEndpoints(Input& input,const Graph& graph) {
    // No live arrow attachments change. Geometry is a fallback for unlinked
    // curved arrows, not an override for explicit atom-to-atom electron flow.
    for(auto& arrow:input.arrows)if(!arrow.reaction&&!arrow.incomplete) {
        auto bondAt=[&](Point p,bool donor,int exclude,int shared=-1) {
            int best=-1;double score=INFINITY,second=INFINITY;
            for(const auto& bond:graph.bonds) {
                if(bond.id==exclude||(donor&&bond.order<2)||bond.order<1||bond.order>3||
                    !graph.atomIDs.contains(bond.a)||!graph.atomIDs.contains(bond.b))continue;
                if(shared>=0&&bond.a!=shared&&bond.b!=shared)continue;
                double t{};double d=segmentDistance(p,graph.atoms[graph.atomIDs.at(bond.a)].p,graph.atoms[graph.atomIDs.at(bond.b)].p,t);
                if(t<.12||t>.88||d>input.length*.45)continue;
                if(d<score){second=score;score=d;best=bond.id;}else second=std::min(second,d);
            }
            return second-score>input.length*.08?best:-1;
        };
        auto atomAt=[&](Point p) {
            int best=-1;double score=INFINITY,second=INFINITY;
            for(const auto& a:graph.atoms) {
                double d=std::hypot(p.x-a.p.x,p.y-a.p.y);if(d>input.length*.25)continue;
                if(d<score){second=score;score=d;best=a.id;}else second=std::min(second,d);
            }
            return second-score>input.length*.08?best:-1;
        };
        if(!graph.atomIDs.contains(arrow.source)&&!graph.bondIDs.contains(arrow.source)) {
            arrow.source=bondAt(arrow.start,true,-1);if(arrow.source<0)arrow.source=atomAt(arrow.start);
        }
        const bool sourceBond=graph.bondIDs.contains(arrow.source);
        if(!graph.atomIDs.contains(arrow.target)&&!graph.bondIDs.contains(arrow.target)) {
            arrow.target=bondAt(arrow.end,false,sourceBond?arrow.source:-1);if(arrow.target<0)arrow.target=atomAt(arrow.end);
        }
        if(sourceBond&&graph.atomIDs.contains(arrow.target)) {
            const auto& from=graph.bonds[graph.bondIDs.at(arrow.source)];
            if(arrow.target!=from.a&&arrow.target!=from.b) {
                int bond=bondAt(arrow.end,false,arrow.source,arrow.target);
                if(bond>=0) {
                    const auto& to=graph.bonds[graph.bondIDs.at(bond)];
                    if(to.a==from.a||to.a==from.b||to.b==from.a||to.b==from.b)arrow.target=bond;
                }
            }
        }
        arrow.incomplete=arrow.source<0||arrow.target<0;
    }
}
std::vector<int> alternateBondPath(const std::map<int,std::vector<int>>& neighbors,int begin,int end) {
    // The shortest alternate path identifies the smallest ring containing
    // this bond. This runs only on the worker's owned product graph.
    std::vector<int> queue{begin};std::map<int,int> previous{{begin,begin}};
    for(size_t i=0;i<queue.size()&&!previous.contains(end);++i) {
        auto found=neighbors.find(queue[i]);if(found==neighbors.end())continue;
        for(int next:found->second) {
            if((queue[i]==begin&&next==end)||(queue[i]==end&&next==begin)||previous.contains(next))continue;
            previous[next]=queue[i];queue.push_back(next);
        }
    }
    if(!previous.contains(end))return {};
    std::vector<int> path;for(int id=end;;id=previous.at(id)){path.push_back(id);if(id==begin)break;}
    std::reverse(path.begin(),path.end());return path;
}
Product predict(const Input& input,const Xml& root,const Graph& original,const Arrow& reaction) {
    Product result;result.arrow=reaction.id;
    result.reason="No unambiguous reactant association";
    Graph g=original;
    Point u=minus(reaction.end,reaction.start);double length=std::hypot(u.x,u.y);
    if(length<input.length*.5)return result;u=times(u,1/length);Point n{-u.y,u.x,0};
    const Step* step=nullptr;for(const auto& s:input.steps)if(s.arrow==reaction.id)step=&s;
    if(step&&step->products){result.reason="Native step already has products";return result;}
    std::set<int> fragments;
    for(const auto& a:g.atoms) {
        bool selected=step&&std::any_of(g.ancestors[a.id].begin(),g.ancestors[a.id].end(),[&](int id){return step->reactants.contains(id);});
        if(!step)selected=dot(minus(a.p,reaction.start),u)<-input.length*.25&&
            std::abs(dot(minus(a.p,reaction.start),n))<input.length*8;
        if(selected)fragments.insert(a.fragment);
    }
    if(fragments.empty())return result;
    // Fragment association must hold for the entire fragment, not only one
    // leftmost atom. This prevents borrowing an unrelated nearby molecule.
    for(const auto& a:g.atoms)if(fragments.contains(a.fragment)&&!step&&dot(minus(a.p,reaction.start),u)>input.length*.25)return result;
    if(!step) {
        std::set<int> participating;
        for(const auto& arrow:input.arrows)if(!arrow.reaction) {
            Point mid=times(plus(arrow.start,arrow.end),.5);
            const double along=dot(minus(mid,reaction.start),u),across=std::abs(dot(minus(mid,reaction.start),n));
            if(along>0||across>input.length*8)continue;
            const double score=-along+across*2;
            bool nearest=true;
            for(const auto& other:input.arrows)if(other.reaction&&other.id!=reaction.id) {
                Point direction=minus(other.end,other.start);double len=std::hypot(direction.x,direction.y);if(len<1e-7)continue;
                direction=times(direction,1/len);Point cross{-direction.y,direction.x,0};
                double x=dot(minus(mid,other.start),direction),y=std::abs(dot(minus(mid,other.start),cross));
                if(x<=0&&y<input.length*8&&-x+2*y<=score+input.length*.25){nearest=false;break;}
            }
            if(!nearest)continue;
            for(int endpoint:{arrow.source,arrow.target}) {
                if(auto atom=g.atomIDs.find(endpoint);atom!=g.atomIDs.end())participating.insert(g.atoms[atom->second].fragment);
                if(auto bond=g.bondIDs.find(endpoint);bond!=g.bondIDs.end()) {
                    const auto& b=g.bonds[bond->second];
                    if(g.atomIDs.contains(b.a))participating.insert(g.atoms[g.atomIDs[b.a]].fragment);
                    if(g.atomIDs.contains(b.b))participating.insert(g.atoms[g.atomIDs[b.b]].fragment);
                }
            }
        }
        for(auto i=fragments.begin();i!=fragments.end();)if(!participating.contains(*i))i=fragments.erase(i);else ++i;
        if(fragments.empty())return result;
    }
    std::map<int,size_t> atoms;std::map<int,size_t> bonds;
    result.reason="Unsupported atom metadata or unresolved aromatic bond order";
    for(size_t i=0;i<g.atoms.size();++i)if(fragments.contains(g.atoms[i].fragment)) {
        if(!supported(g.atoms[i]))return result;atoms.emplace(g.atoms[i].id,i);
    }
    for(size_t i=0;i<g.bonds.size();++i) {
        auto& b=g.bonds[i];if(!atoms.contains(b.a)||!atoms.contains(b.b))continue;
        if(b.order<1||b.order>3)return result;bonds.emplace(b.id,i);
    }
    if(atoms.empty()||atoms.size()>2048)return result;
    auto pair=[](int a,int b){return std::pair<int,int>{std::min(a,b),std::max(a,b)};};
    using Key=std::pair<int,int>;
    std::map<Key,int> orders,changes;std::map<int,int> charge,oldSums;
    for(const auto& [id,i]:bonds){const auto& b=g.bonds[i];Key k=pair(b.a,b.b);if(orders.contains(k))return result;orders[k]=b.order;oldSums[b.a]+=b.order;oldSums[b.b]+=b.order;}
    for(const auto& [id,i]:atoms) {
        auto& a=g.atoms[i];if(a.hydrogens<0)a.hydrogens=valence(a.element,a.charge)-oldSums[id];
        if(a.hydrogens<0||a.hydrogens>4||oldSums[id]+a.hydrogens!=valence(a.element,a.charge))return result;
    }
    size_t arrows{};std::set<int> touched;std::set<std::pair<int,int>> seenArrows;
    result.reason="Unresolved or unsupported mechanism endpoint";
    for(const auto& a:input.arrows) {
        if(a.reaction)continue;
        const bool sourceAtom=atoms.contains(a.source),sourceBond=bonds.contains(a.source);
        const bool targetAtom=atoms.contains(a.target),targetBond=bonds.contains(a.target);
        if(!sourceAtom&&!sourceBond) {
            if(targetAtom||targetBond)return result;
            // An unlinked mechanism drawn among these reactants is incomplete,
            // not a license to ignore part of the requested electron movement.
            Point mid=times(plus(a.start,a.end),.5);
            if(a.incomplete&&dot(minus(mid,reaction.start),u)<0&&std::abs(dot(minus(mid,reaction.start),n))<input.length*8)return result;
            continue;
        }
        if(a.incomplete||(!targetAtom&&!targetBond)||!seenArrows.emplace(a.source,a.target).second)return result;
        ++arrows;
        if(sourceAtom)charge[a.source]+=2;
        else {const auto& b=g.bonds[bonds[a.source]];--changes[pair(b.a,b.b)];++charge[b.a];++charge[b.b];touched.insert(b.a);touched.insert(b.b);}
        if(targetBond) {
            const auto& b=g.bonds[bonds[a.target]];
            if(sourceAtom&&a.source!=b.a&&a.source!=b.b)return result;
            if(sourceBond) {const auto& from=g.bonds[bonds[a.source]];if(from.a!=b.a&&from.a!=b.b&&from.b!=b.a&&from.b!=b.b)return result;}
            ++changes[pair(b.a,b.b)];--charge[b.a];--charge[b.b];touched.insert(b.a);touched.insert(b.b);
        } else if(sourceAtom) {
            if(a.source==a.target)return result;
            ++changes[pair(a.source,a.target)];--charge[a.source];--charge[a.target];touched.insert(a.source);touched.insert(a.target);
        } else {
            const auto& b=g.bonds[bonds[a.source]];
            if(a.target!=b.a&&a.target!=b.b)return result; // External bond-to-atom migration has two possible origins.
            charge[a.target]-=2;
        }
    }
    if(!arrows)return result;
    result.reason="Simultaneous electron movement has unsupported valence or stereochemistry";
    for(const auto& [k,delta]:changes)orders[k]+=delta;
    std::map<int,int> sums;
    bool changed=false;for(const auto& [k,order]:orders) {
        if(order<0||order>3)return result;if(order){sums[k.first]+=order;sums[k.second]+=order;}
        changed|=changes[k]!=0;
    }
    if(!changed){result.reason="Mechanism does not change bond orders";return result;}
    int totalCharge{};
    for(const auto& [id,i]:atoms) {
        auto& a=g.atoms[i];totalCharge+=charge[id];a.charge+=charge[id];
        if(sums[id]+a.hydrogens!=valence(a.element,a.charge))return result;
        if(touched.contains(id)&&(a.stereo||a.node.get("EnhancedStereoType")!=""))return result;
    }
    if(totalCharge)return result;
    for(const auto& [id,i]:bonds)if(g.bonds[i].stereo&&(touched.contains(g.bonds[i].a)||touched.contains(g.bonds[i].b)))return result;
    // Retain each original fragment's internal layout. For intermolecular
    // formation, translate entire fragments to a normal bond length; never run
    // the native cleanup algorithm or move the user's reactants for a preview.
    std::map<int,Point> shifts;std::set<int> placed;
    for(const auto& [id,i]:atoms)if(!placed.contains(g.atoms[i].fragment)) {
        int seed=g.atoms[i].fragment;placed.insert(seed);shifts[seed]={};
        bool progress=true;while(progress){progress=false;
            for(const auto& [k,order]:orders)if(order>0) {
                auto& a=g.atoms[atoms[k.first]];auto& b=g.atoms[atoms[k.second]];
                if(a.fragment==b.fragment||placed.contains(a.fragment)==placed.contains(b.fragment))continue;
                Atom* fixed=&a;Atom* moved=&b;if(!placed.contains(a.fragment))std::swap(fixed,moved);
                Point ray=minus(moved->p,fixed->p);double distance=std::hypot(ray.x,ray.y);
                if(distance<1e-7)ray=u;else ray=times(ray,1/distance);
                shifts[moved->fragment]=minus(plus(plus(fixed->p,shifts[fixed->fragment]),times(ray,input.length)),moved->p);
                placed.insert(moved->fragment);progress=true;
            }
        }
    }
    for(const auto& [id,i]:atoms)g.atoms[i].p=plus(g.atoms[i].p,shifts[g.atoms[i].fragment]);
    // Split actual product connectivity. Cleaved fragments are independent
    // editable native fragments, including leaving groups and counterions.
    std::map<int,int> parent;for(const auto& [id,i]:atoms)parent[id]=id;
    auto find=[&](int id){while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id;};
    for(const auto& [k,order]:orders)if(order>0)parent[find(k.second)]=find(k.first);
    std::map<int,std::vector<int>> components;for(const auto& [id,i]:atoms)components[find(id)].push_back(id);
    double cursor=dot(reaction.end,u)+input.length*1.5,baseline=dot(reaction.end,n);
    Bounds all;std::map<int,Point> moves;
    for(const auto& [component,ids]:components) {
        Bounds b;for(int id:ids){Point p=g.atoms[atoms[id]].p;b.add({dot(p,u),dot(p,n),0});}
        Point shift=plus(times(u,cursor-b.l),times(n,baseline-(b.t+b.b)*.5));
        for(int id:ids){auto& a=g.atoms[atoms[id]];moves[id]=minus(plus(a.p,shift),original.atoms[atoms[id]].p);a.p=plus(a.p,shift);all.add(a.p);}
        cursor+=std::max(b.r-b.l,input.length*.5)+input.length*1.5;
    }
    // Leave crowded product areas alone. Do not cover already drawn products
    // merely because the native reaction interpreter didn't assign them.
    result.reason="Product area is occupied or outside the page";
    for(const auto& a:original.atoms)if(!atoms.contains(a.id)&&a.p.x>=all.l-input.length&&a.p.x<=all.r+input.length&&a.p.y>=all.t-input.length&&a.p.y<=all.b+input.length)return result;
    // Export bounding boxes may describe the exported content, not editable
    // paper. Use the physical native page rectangle captured with the graph.
    if(valid(input.paper)&&(all.l<input.paper.l||all.t<input.paper.t||all.r>input.paper.r||all.b>input.paper.b))return result;
    auto preview=std::make_shared<ReactionPreview>();preview->identity=uint64_t(reaction.id)+1;
    preview->bounds={(all.t-input.length*.6)*input.units,(all.l-input.length*.6)*input.units,(all.b+input.length*.6)*input.units,(all.r+input.length*.6)*input.units};
    Xml productRoot=root;productRoot.children.clear();
    for(const auto& child:root.children)if(child.name=="fonttable"||child.name=="colortable")productRoot.children.push_back(child);
    productRoot.a.erase("BoundingBox");productRoot.a.erase("WindowPosition");productRoot.a.erase("WindowSize");productRoot.a.erase("WindowIsZoomed");
    Xml page;page.name="page";page.a["id"]="1";int nextID=2;std::map<int,int> newIDs;
    for(const auto& [id,i]:atoms)newIDs[id]=nextID++;
    const double fontSize=12;std::map<int,std::string> fonts;
    for(const auto& child:root.children)if(child.name=="fonttable")for(const auto& f:child.children)fonts[integer(f.get("id"))]=f.get("name","Arial");
    std::map<int,std::vector<int>> neighbors;
    for(const auto& [edge,order]:orders)if(order>0){neighbors[edge.first].push_back(edge.second);neighbors[edge.second].push_back(edge.first);}
    auto productPoint=[&](int id){return g.atoms[atoms.at(id)].p;};
    auto line=[&](Point a,Point b,double width){preview->artwork.strokes.push_back({times(a,input.units),times(b,input.units),width*input.units});};
    for(const auto& [component,ids]:components) {
        Xml fragment;fragment.name="fragment";fragment.a["id"]=std::to_string(nextID++);
        for(int id:ids) {
            const auto& a=g.atoms[atoms[id]];Xml node=a.node;stripIDs(node);
            node.a["id"]=std::to_string(newIDs[id]);node.a["p"]=number(a.p.x)+" "+number(a.p.y);
            node.a["Charge"]=std::to_string(a.charge);node.a["NumHydrogens"]=std::to_string(a.hydrogens);
            // Native node labels are literal styled atom text. Regenerate only
            // affected plain element labels; isotope and H counts stay explicit.
            std::string label;
            bool show=a.element!=6||a.charge||sums[id]==0||integer(a.node.get("Isotope"))!=0;
            if(show) {
                int isotope=integer(a.node.get("Isotope"));if(isotope)label+=std::to_string(isotope);
                label+=symbol(a.element);if(a.hydrogens){label+='H';if(a.hydrogens!=1)label+=std::to_string(a.hydrogens);}
                if(a.charge){if(std::abs(a.charge)!=1)label+=std::to_string(std::abs(a.charge));label+=a.charge>0?"+":"-";}
            }
            const bool regenerate=charge[id]!=0||node.children.empty();
            if(regenerate)node.children.clear();
            else for(auto& child:node.children)if(child.name=="t") {
                child.a["id"]=std::to_string(nextID++);
                if(child.a.contains("p")){Point p=plus(point(child.a["p"]),moves[id]);child.a["p"]=number(p.x)+" "+number(p.y);}
                child.a.erase("BoundingBox");
            }
            if(!label.empty()&&regenerate) {
                Xml t;t.name="t";t.a["p"]=node.a["p"];t.a["Justification"]="Left";t.a["LabelAlignment"]="Left";
                t.a["id"]=std::to_string(nextID++);
                Xml s;s.name="s";s.text=label;s.a["font"]=root.get("LabelFont","3");s.a["size"]=root.get("LabelSize","12");
                s.a["face"]=root.get("LabelFace","96");t.children.push_back(s);node.children.push_back(t);
            }
            for(const auto& child:node.children)if(child.name=="t"&&!text(child).empty()) {
                Point p=child.a.contains("p")?point(child.get("p")):a.p;
                const Xml* style=child.children.empty()?nullptr:&child.children.front();
                int font=integer(style?style->get("font"):root.get("LabelFont","3"));
                double size=std::stod(style?style->get("size","12"):root.get("LabelSize","12"));
                if(!std::isfinite(size)||size<=0||size>100)size=fontSize;
                preview->labels.push_back({times(p,input.units),wide(text(child)),wide(fonts.contains(font)?fonts[font]:"Arial"),size*input.units});
            }
            if(touched.contains(id))node.a.erase("AS");node.a.erase("AtomID");fragment.children.push_back(std::move(node));
        }
        for(const auto& [k,order]:orders)if(order>0&&find(k.first)==component) {
            Xml b;b.name="b";int begin=k.first,endID=k.second;
            double width=input.width,gap{};int priorOrder{};
            for(const auto& [id,i]:bonds)if(Key(pair(g.bonds[i].a,g.bonds[i].b))==k) {
                const auto& source=g.bonds[i];b=source.node;begin=source.a;endID=source.b;priorOrder=source.order;
                if(auto ink=input.nativeInk.find(id);ink!=input.nativeInk.end()) {
                    if(std::isfinite(ink->second.width)&&ink->second.width>0)width=ink->second.width;
                    if(std::isfinite(ink->second.gap)&&ink->second.gap>0)gap=ink->second.gap;
                }
                break;
            }
            // Keep B/E orientation: reversing these IDs without also flipping
            // DoublePosition reverses which side the native importer draws.
            stripIDs(b);b.a["id"]=std::to_string(nextID++);b.a["B"]=std::to_string(newIDs[begin]);b.a["E"]=std::to_string(newIDs[endID]);b.a["Order"]=std::to_string(order);b.a.erase("BS");
            Point a=productPoint(begin),end=productPoint(endID);Point ray=minus(end,a);double len=std::hypot(ray.x,ray.y);if(len<1e-5)return Product{};
            Point normal{-ray.y/len,ray.x/len,0};
            if(gap<=0)gap=std::max(input.spacing*len,input.minimumGapWidths*width);
            // Trim only the preview ink around labels; native acceptance gets
            // real atom labels and its own character-attachment bond endpoints.
            auto labeled=[&](int id){const auto& atom=g.atoms[atoms[id]];return atom.element!=6||atom.charge;};
            if(labeled(begin))a=plus(a,times(ray,std::min(.3,6/len)));
            if(labeled(endID))end=minus(end,times(ray,std::min(.3,6/len)));
            if(order==1)line(a,end,width);
            else if(order==2) {
                const auto path=alternateBondPath(neighbors,begin,endID);
                double area{};
                if(!path.empty()) {
                    auto cross=[](Point p,Point q){return p.x*q.y-p.y*q.x;};
                    const Point origin=productPoint(begin);
                    for(size_t i=path.size()-1;i>0;--i)area+=cross(minus(productPoint(path[i]),origin),minus(productPoint(path[i-1]),origin));
                }
                // A newly doubled single bond has no user-selected double
                // position. Choose ring interiors, retaining explicit positions
                // of existing double bonds. CDXML carries the same choice to
                // native acceptance, rather than recalculating another side.
                std::string position=priorOrder==2?b.get("DoublePosition"):"";
                if(position!="Left"&&position!="Right"&&position!="Center")
                    position=std::abs(area)>1e-8?(area>0?"Right":"Left"):"Center";
                b.a["DoublePosition"]=position;
                if(position=="Center") {
                    line(plus(a,times(normal,gap*.5)),plus(end,times(normal,gap*.5)),width);
                    line(minus(a,times(normal,gap*.5)),minus(end,times(normal,gap*.5)),width);
                } else {
                    const Point offset=times(normal,position=="Right"?1:-1);
                    auto inset=[&](int id,int toward,Point anchor,int ringNeighbor) {
                        if(labeled(id))return plus(anchor,times(offset,gap));
                        const Point vertex=productPoint(id),unit=times(minus(productPoint(toward),vertex),1/len);
                        int adjacent=-1;double best=-INFINITY;
                        for(int candidate:neighbors[id])if(candidate!=toward) {
                            Point v=minus(productPoint(candidate),vertex);double norm=std::hypot(v.x,v.y);if(norm<1e-8)continue;v=times(v,1/norm);
                            if(dot(v,offset)<=1e-8)continue;
                            double score=candidate==ringNeighbor?2:dot(v,unit);
                            if(score>best){best=score;adjacent=candidate;}
                        }
                        double trim{};
                        if(adjacent>=0) {
                            Point v=minus(productPoint(adjacent),vertex);v=times(v,1/std::hypot(v.x,v.y));
                            double cross=std::abs(unit.x*v.y-unit.y*v.x);
                            // Same neighboring-ray inset as native double ink
                            // (3bf9b0) and the ring drawing-tool ghost. At 120
                            // degrees this trims by gap / sqrt(3).
                            if(cross>1e-8)trim=std::clamp((1+dot(unit,v))*gap/cross,0.0,len*.45);
                        }
                        return plus(plus(anchor,times(offset,gap)),times(unit,trim));
                    };
                    line(a,end,width);
                    line(inset(begin,endID,a,path.empty()?-1:path[1]),
                        inset(endID,begin,end,path.empty()?-1:path[path.size()-2]),width);
                }
            } else {line(a,end,width);line(plus(a,times(normal,gap)),plus(end,times(normal,gap)),width);line(minus(a,times(normal,gap)),minus(end,times(normal,gap)),width);}
            fragment.children.push_back(std::move(b));
        }
        page.children.push_back(std::move(fragment));
    }
    productRoot.children.push_back(std::move(page));std::ostringstream out;out.imbue(std::locale::classic());write(productRoot,out);
    result.xml=out.str();result.preview=std::move(preview);result.reason="Preview ready";return result;
}
Result calculate(const Input& input) {
    Result result;result.token=input.token;result.pageIdentity=input.pageIdentity;
    if(input.xml.empty())return result;
    Xml root=parse(input.xml);Graph graph;readGraph(root,graph);
    for(auto& bond:graph.bonds)if(bond.node.get("Order")=="1.5") {
        auto native=input.nativeOrders.find(bond.id);
        if(native!=input.nativeOrders.end()&&(native->second==1||native->second==2)) {
            bond.order=native->second;bond.node.a["Order"]=std::to_string(bond.order);
        }
    }
    Input resolved=input;resolveMechanismEndpoints(resolved,graph);
    result.key=contentKey(input);
    std::ostringstream report;report<<"Reaction suggestions: atoms="<<graph.atoms.size()<<" bonds="<<graph.bonds.size()<<" arrows="<<input.arrows.size()<<"\r\n";
    for(const auto& arrow:resolved.arrows)report<<"Arrow "<<arrow.id<<(arrow.reaction?" reaction":" mechanism")<<": source="<<arrow.source<<" target="<<arrow.target<<" head="<<arrow.head<<" tail="<<arrow.tail<<(arrow.incomplete?" incomplete":"")<<"\r\n";
    for(const auto& a:input.arrows)if(a.reaction) {
        Product product=predict(resolved,root,graph,a);
        report<<"Reaction "<<a.id<<": "<<product.reason<<"\r\n";
        if(product.preview)result.products.push_back(std::move(product));
    }
    writeReactionStatus(report.str().c_str());
    if(!result.products.empty()) {
        // All suggestions share one import document and one ReadCDX call.
        // Numeric object IDs are remapped across reaction steps before native
        // fresh-ID allocation; font/color table IDs retain their shared meaning.
        Xml combined=parse(result.products.front().xml);combined.children.erase(
            std::remove_if(combined.children.begin(),combined.children.end(),[](const Xml& x){return x.name=="page";}),combined.children.end());
        Xml page;page.name="page";page.a["id"]="1";int next=2;
        for(const auto& product:result.products) {
            Xml owned=parse(product.xml);
            for(auto& source:owned.children)if(source.name=="page") {
                std::map<int,int> ids;
                auto collect=[&](auto&& self,const Xml& x)->void {
                    if(x.a.contains("id"))ids[integer(x.get("id"))]=next++;
                    for(const auto& child:x.children)self(self,child);
                };
                for(const auto& fragment:source.children)collect(collect,fragment);
                auto remap=[&](auto&& self,Xml& x)->void {
                    for(const char* property:{"id","B","E"})if(x.a.contains(property))x.a[property]=std::to_string(ids.at(integer(x.a[property])));
                    for(auto& child:x.children)self(self,child);
                };
                for(auto& fragment:source.children){remap(remap,fragment);page.children.push_back(std::move(fragment));}
            }
        }
        combined.children.push_back(std::move(page));std::ostringstream out;write(combined,out);result.acceptedXml=out.str();
    }
    return result;
}
void runWorker() noexcept {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);(void)apartment;
    for(;;) {
        std::shared_ptr<Job> job;
        {std::unique_lock lock(jobsMutex);jobsEvent.wait(lock,[]{return bool(pendingJob);});job=std::move(pendingJob);}
        if(job->cancelled.load())continue;
        try {job->result=calculate(job->input);}
        catch(const std::exception& e){job->result={job->input.token,job->input.pageIdentity};std::string error="Reaction suggestion calculation: ";error+=e.what();error+="\r\n";writeReactionStatus(error.c_str());}
        catch(...){job->result={job->input.token,job->input.pageIdentity};writeReactionStatus("Reaction suggestion calculation failed.\r\n");}
        job->ready.store(true,std::memory_order_release);
        if(!job->cancelled.load()&&IsWindow(job->input.window))PostMessageW(job->input.window,WM_NULL,0,0);
    }
}
void publish(State& state) {
    state.preview.reset();if(!state.result||state.dismissedKey==state.result->key||state.result->products.empty())return;
    auto preview=std::make_shared<ReactionPreview>();preview->identity=state.result->key;
    bool first=true;
    for(const auto& product:state.result->products) {
        const auto& p=*product.preview;
        preview->artwork.strokes.insert(preview->artwork.strokes.end(),p.artwork.strokes.begin(),p.artwork.strokes.end());
        preview->labels.insert(preview->labels.end(),p.labels.begin(),p.labels.end());
        preview->bounds=first?p.bounds:RectD{std::min(preview->bounds.t,p.bounds.t),std::min(preview->bounds.l,p.bounds.l),std::max(preview->bounds.b,p.bounds.b),std::max(preview->bounds.r,p.bounds.r)};first=false;
    }
    preview->labels.push_back({{preview->bounds.l,preview->bounds.b+documentUnits()*12,0},L"Ctrl+Enter: accept suggestion   Esc: dismiss",L"Arial",documentUnits()*9});
    state.preview=std::move(preview);
}
bool accept(Obj doc,State& state) {
    if(!state.result||state.result->token!=state.token||state.dirty||state.result->products.empty())return false;
    Obj page=mainPage(doc);if(!page||at<uint64_t>(page,0xb0)!=state.result->pageIdentity)return false;
    // Parse every owned CDXML product before opening a native transaction.
    // ReadCDX uses paste mode 3, the same fresh-ID/exact-position path as
    // native CopyStructure, and records ordinary native creation commands.
    std::vector<Obj> documents;
    struct Documents {std::vector<Obj>& objects;~Documents(){for(Obj o:objects)if(o)vf<void(*)(Obj,unsigned)>(o,0x40)(o,1);}} cleanup{documents};
    alignas(16) std::byte scrap[0x30]{};fn<Obj(*)(Obj)>(0xe16f0)(scrap);
    struct Scrap {Obj object;~Scrap(){fn<void(*)(Obj)>(0xe1a50)(object);}} scrapGuard{scrap};
    {
        std::istringstream stream(state.result->acceptedXml);stream.imbue(std::locale::classic());Obj object{};
        fn<Obj*(*)(Obj,Obj*,std::istream*)>(0xe2850)(scrap,&object,&stream);
        if(!object)return false;documents.push_back(object);
    }
    auto held=state.result;
    state.dismissedKey=held->key;state.preview.reset();
    fn<void(*)(Obj)>(0x502c40)(doc);fn<void(*)(Obj)>(0x502820)(doc);
    struct Undo {Obj doc;~Undo(){fn<void(*)(Obj)>(0x502c40)(doc);}} undo{doc};
    Point offset{};std::string error;
    fn<short(*)(Obj,Obj,int,int,Obj,const Point*,bool*,const std::string*)>(0x39c120)(page,documents.front(),1,3,nullptr,&offset,nullptr,&error);
    reactionPageChanged(page);bumpGeneration();queueGpuDrawingCommit(doc);
    return true;
}
}
void installReactionSuggestions() {
    hook(0x2c8d10,atomModified,oldAtomModified);hook(0x2efc00,bondModified,oldBondModified);
}
ReactionCameraScope::ReactionCameraScope() noexcept {++observing;}
ReactionCameraScope::~ReactionCameraScope(){--observing;}
void reactionPageChanged(Obj page) noexcept {
    if (!patchEnabled(17)) return;
    if(!onUI()||!page||observing)return;
    auto found=states.find(page);if(found==states.end())return;
    auto& state=found->second;state.dirty=true;++state.token;state.lastMutation=GetTickCount64();
    if(state.job)state.job->cancelled.store(true);state.preview.reset();
    Obj doc=at<Obj>(page,8),port=doc?at<Obj>(doc,0x258):nullptr;
    if(port)SetTimer(portWindow(port),reactionSuggestionTimer,300,nullptr);
}
void reactionObjectChanged(Obj object) noexcept {
    if (!patchEnabled(17)) return;
    if(!onUI()||!object||observing)return;
    Obj page=at<Obj>(object,0x60);auto found=states.find(page);if(found==states.end())return;
    if(at<uintptr_t>(object,0)==base+0x8b2cf0) {
        try {
            auto key=arrowKey(object);int id=at<int>(object,0x24);auto previous=found->second.arrowKeys.find(id);
            if(previous!=found->second.arrowKeys.end()&&previous->second==key)return;
            found->second.arrowKeys[id]=key;
        } catch(...){return;}
    }
    reactionPageChanged(page);
}
void forgetReactionPage(Obj page) noexcept {
    auto found=states.find(page);if(found!=states.end()){if(found->second.job)found->second.job->cancelled.store(true);states.erase(found);}
}
void idleReactionSuggestions(Obj doc) noexcept {
    if (!patchEnabled(17)) return;
    if(!onUI()||!doc||observing||trackingDepth||GetCapture()||placementChemistryPending(doc)||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000))return;
    Obj page=mainPage(doc);if(!page)return;
    try {
        if(!states.contains(page)&&states.size()>=8){auto old=states.begin();if(old->second.job)old->second.job->cancelled.store(true);states.erase(old);}
        auto& state=states[page];const auto identity=at<uint64_t>(page,0xb0);if(state.identity&&state.identity!=identity){state=State{};}state.identity=identity;
        if(state.job&&state.job->ready.load(std::memory_order_acquire)) {
            if(!state.job->cancelled.load()&&state.job->result.token==state.token) {
                state.result=std::make_shared<Result>(std::move(state.job->result));publish(state);
            }
            state.job.reset();
        }
        const auto now=GetTickCount64();
        if(!state.dirty)return;
        if(now-state.lastMutation<250||now-state.lastCapture<500) {
            Obj port=at<Obj>(doc,0x258);if(port)SetTimer(portWindow(port),reactionSuggestionTimer,300,nullptr);
            return;
        }
        state.lastCapture=now;
        Input input=capture(doc,state.token);state.dirty=false;
        {Observe observe(doc);each(page,[&](Obj o){if(o&&at<uintptr_t>(o,0)==base+0x8b2cf0)state.arrowKeys[at<int>(o,0x24)]=arrowKey(o);});}
        if(input.xml.empty()){state.result.reset();state.preview.reset();writeReactionStatus("No full reaction arrow and mechanism arrow pair found.\r\n");return;}
        const auto key=contentKey(input);
        if(state.result&&state.result->key==key){auto copy=std::make_shared<Result>(*state.result);copy->token=state.token;state.result=std::move(copy);publish(state);return;}
        std::call_once(workerOnce,[]{std::thread(runWorker).detach();});
        auto job=std::make_shared<Job>();job->input=std::move(input);state.job=job;
        {std::lock_guard lock(jobsMutex);if(pendingJob)pendingJob->cancelled.store(true);pendingJob=job;}jobsEvent.notify_one();
    } catch(const std::exception& e) {
        std::string error="Reaction suggestion snapshot: ";error+=e.what();error+="\r\n";writeReactionStatus(error.c_str());
        auto found=states.find(page);if(found!=states.end()){found->second.preview.reset();found->second.dirty=false;}
    } catch(...) {auto found=states.find(page);if(found!=states.end()){found->second.preview.reset();found->second.dirty=false;}writeReactionStatus("Reaction suggestion snapshot failed.\r\n");}
}
std::shared_ptr<const ReactionPreview> reactionSuggestionPreview(Obj doc) noexcept {
    if (!patchEnabled(17)) return nullptr;
    auto found=states.find(mainPage(doc));return found==states.end()?nullptr:found->second.preview;
}
bool reactionSuggestionShortcut(Obj doc,UINT message,WPARAM key,LPARAM flags) noexcept {
    if (!patchEnabled(17)) return false;
    if(!onUI()||!doc||trackingDepth||GetCapture()||message!=WM_KEYDOWN)return false;
    auto found=states.find(mainPage(doc));if(found==states.end()||!found->second.preview)return false;
    if(key==VK_ESCAPE){found->second.dismissedKey=found->second.result->key;found->second.preview.reset();return true;}
    if(key!=VK_RETURN||!(GetKeyState(VK_CONTROL)&0x8000)||(GetKeyState(VK_MENU)&0x8000))return false;
    GhostTool tool{};if(fn<int(*)()>(0x4f4ba0)()!=0&&!readGhostTool(mainPage(doc),tool))return false;
    if(flags&(1LL<<30))return true;
    try{return accept(doc,found->second);}catch(...){reactionPageChanged(mainPage(doc));return true;}
}
}
