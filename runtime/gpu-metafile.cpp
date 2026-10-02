#include "gpu-scene.hpp"
namespace cd {
namespace {
constexpr DWORD emrDrawEscape=105,emrExtEscape=106,emrSmallText=108,emrNamedEscape=110;
using Record=std::vector<BYTE>;
Record copy(const EMR* r) { const BYTE* first=reinterpret_cast<const BYTE*>(r);return {first,first+r->nSize}; }
Record moveRecord(LONG x,LONG y) {
    EMRMOVETOEX r{};r.emr={EMR_MOVETOEX,sizeof(r)};r.ptl={x,y};return copy(&r.emr);
}
bool paint(DWORD type) {
    switch(type) {
    case EMR_POLYBEZIER:case EMR_POLYGON:case EMR_POLYLINE:case EMR_POLYBEZIERTO:case EMR_POLYLINETO:
    case EMR_POLYPOLYLINE:case EMR_POLYPOLYGON:case EMR_SETPIXELV:case EMR_ANGLEARC:
    case EMR_ELLIPSE:case EMR_RECTANGLE:case EMR_ROUNDRECT:case EMR_ARC:case EMR_CHORD:case EMR_PIE:
    case EMR_LINETO:case EMR_ARCTO:case EMR_POLYDRAW:case EMR_FILLPATH:case EMR_STROKEANDFILLPATH:case EMR_STROKEPATH:
    case EMR_FILLRGN:case EMR_FRAMERGN:case EMR_INVERTRGN:case EMR_PAINTRGN:
    case EMR_BITBLT:case EMR_STRETCHBLT:case EMR_MASKBLT:case EMR_PLGBLT:case EMR_SETDIBITSTODEVICE:case EMR_STRETCHDIBITS:
    case EMR_EXTTEXTOUTA:case EMR_EXTTEXTOUTW:case EMR_POLYTEXTOUTA:case EMR_POLYTEXTOUTW:
    case EMR_POLYBEZIER16:case EMR_POLYGON16:case EMR_POLYLINE16:case EMR_POLYBEZIERTO16:case EMR_POLYLINETO16:
    case EMR_POLYPOLYLINE16:case EMR_POLYPOLYGON16:case EMR_POLYDRAW16:case EMR_ALPHABLEND:
    case EMR_TRANSPARENTBLT:case EMR_GRADIENTFILL:return true;
    default:return false;
    }
}
void consumePosition(std::vector<Record>& state,const EMR* r) {
    if(r->iType==EMR_LINETO) { auto p=reinterpret_cast<const EMRLINETO*>(r)->ptl;state.push_back(moveRecord(p.x,p.y)); }
    else if(r->iType==EMR_POLYLINETO||r->iType==EMR_POLYBEZIERTO) {
        auto p=reinterpret_cast<const EMRPOLYLINE*>(r);if(p->cptl) { auto end=p->aptl[p->cptl-1];state.push_back(moveRecord(end.x,end.y)); }
    } else if(r->iType==EMR_POLYLINETO16||r->iType==EMR_POLYBEZIERTO16) {
        auto p=reinterpret_cast<const EMRPOLYLINE16*>(r);if(p->cpts) { auto end=p->apts[p->cpts-1];state.push_back(moveRecord(end.x,end.y)); }
    } else if(r->iType==EMR_FILLPATH||r->iType==EMR_STROKEPATH||r->iType==EMR_STROKEANDFILLPATH) {
        EMR abort={EMR_ABORTPATH,sizeof(EMR)};state.push_back(copy(&abort));
    } else if(r->iType==EMR_ARCTO||r->iType==EMR_POLYDRAW||r->iType==EMR_POLYDRAW16) {
        throw std::runtime_error("Metafile current-position compatibility operation");
    }
}
std::shared_ptr<const SceneMetafile> fragment(const SceneMetafile& original,const Record& header,
        const std::vector<Record>& state,const Record& eof) {
    auto result=std::make_shared<SceneMetafile>();result->bounds=original.bounds;result->bytes=header;
    for(const auto& r:state) result->bytes.insert(result->bytes.end(),r.begin(),r.end());
    result->bytes.insert(result->bytes.end(),eof.begin(),eof.end());
    auto h=reinterpret_cast<ENHMETAHEADER*>(result->bytes.data());h->nBytes=DWORD(result->bytes.size());h->nRecords=DWORD(state.size()+2);
    return result;
}
uint32_t reduceRop(DWORD rop,bool& pattern) {
    uint32_t truth=(rop>>16)&255,nibble{};pattern=false;
    if(truth==0||truth==0x55||truth==0xaa||truth==0xff) { pattern=true;return truth&15; }
    if((truth&15)==(truth>>4)) {
        // P-independent: the shader's ink input is the source bitmap.
        return truth&15;
    }
    if(((truth&3)==((truth>>2)&3))&&(((truth>>4)&3)==((truth>>6)&3))) {
        pattern=true;for(unsigned p=0;p<2;++p) for(unsigned d=0;d<2;++d)
            nibble|=((truth>>(4*p+d))&1)<<(2*p+d);
        return nibble;
    }
    throw std::runtime_error("Three-operand GDI raster operation requires compatibility rendering");
}
}
std::vector<SceneCommand> translateGpuMetafile(const SceneCommand& input,int width,int height,uint64_t& serial) {
    const auto& original=*input.metafile;auto& bytes=original.bytes;
    if(bytes.size()<sizeof(ENHMETAHEADER)) throw std::runtime_error("Incomplete metafile");
    auto header=reinterpret_cast<const ENHMETAHEADER*>(bytes.data());
    if(header->iType!=EMR_HEADER||header->nSize>bytes.size()) throw std::runtime_error("Invalid metafile header");
    Record first=copy(reinterpret_cast<const EMR*>(header)),eof;std::vector<const EMR*> records;
    for(size_t offset=header->nSize;offset+sizeof(EMR)<=bytes.size();) {
        auto r=reinterpret_cast<const EMR*>(bytes.data()+offset);
        if(r->nSize<sizeof(EMR)||r->nSize>bytes.size()-offset) throw std::runtime_error("Invalid metafile record");
        if(r->iType==EMR_EOF) { eof=copy(r);break; }records.push_back(r);offset+=r->nSize;
    }
    if(eof.empty()) throw std::runtime_error("Missing metafile terminator");
    bool requiresTranslation{};
    for(auto r:records) {
        if(r->iType==emrExtEscape||r->iType==emrDrawEscape||r->iType==emrNamedEscape||r->iType==EMR_MASKBLT||r->iType==EMR_PLGBLT||r->iType==EMR_INVERTRGN||r->iType==EMR_EXTFLOODFILL||r->iType==emrSmallText)
            throw std::runtime_error("Metafile compatibility operation");
        if(r->iType==EMR_SETROP2&&reinterpret_cast<const EMRSETROP2*>(r)->iMode!=R2_COPYPEN) requiresTranslation=true;
        if(r->iType==EMR_BITBLT&&reinterpret_cast<const EMRBITBLT*>(r)->dwRop!=SRCCOPY) requiresTranslation=true;
        if(r->iType==EMR_STRETCHBLT&&reinterpret_cast<const EMRSTRETCHBLT*>(r)->dwRop!=SRCCOPY) requiresTranslation=true;
        if(r->iType==EMR_STRETCHDIBITS&&reinterpret_cast<const EMRSTRETCHDIBITS*>(r)->dwRop!=SRCCOPY) requiresTranslation=true;
    }
    for(auto r:records) if(r->iType==EMR_GDICOMMENT&&r->nSize>=sizeof(EMRGDICOMMENT)+3) {
        auto comment=reinterpret_cast<const EMRGDICOMMENT*>(r);
        if(comment->cbData>=4&&*reinterpret_cast<const DWORD*>(comment->Data)==0x2b464d45)
            return {input}; // EMF+ object/comment streams remain intact; Direct2D selects the dual stream.
    }
    if(!requiresTranslation) return {input};
    std::vector<SceneCommand> output;std::vector<Record> state;std::vector<size_t> painted;
    int rop2=R2_COPYPEN;bool insidePath{};std::vector<int> savedRops;
    auto flush=[&]() {
        if(painted.empty()) return;
        auto command=input;command.metafile=fragment(original,first,state,eof);output.push_back(std::move(command));
        for(auto index:painted) {
            std::vector<Record> updates;consumePosition(updates,reinterpret_cast<const EMR*>(state[index].data()));
            state[index]=updates.empty()?Record{}:std::move(updates.front());
        }painted.clear();
        state.erase(std::remove_if(state.begin(),state.end(),[](const Record& r){return r.empty();}),state.end());
    };
    for(const EMR* r:records) {
        if(r->iType==EMR_SAVEDC) savedRops.push_back(rop2);
        if(r->iType==EMR_RESTOREDC) {
            auto relative=reinterpret_cast<const EMRRESTOREDC*>(r)->iRelative;
            if(relative>=0||size_t(-relative)>savedRops.size()) throw std::runtime_error("Metafile absolute DC restore");
            rop2=savedRops[savedRops.size()-size_t(-relative)];savedRops.resize(savedRops.size()-size_t(-relative));
        }
        if(r->iType==EMR_BEGINPATH) insidePath=true;
        if(r->iType==EMR_ENDPATH) insidePath=false;
        if(r->iType==EMR_ABORTPATH) insidePath=false;
        if(r->iType==EMR_SETROP2) {
            rop2=reinterpret_cast<const EMRSETROP2*>(r)->iMode;
            EMRSETROP2 replacement=*reinterpret_cast<const EMRSETROP2*>(r);replacement.iMode=R2_COPYPEN;
            state.push_back(copy(&replacement.emr));continue;
        }
        if(r->iType==EMR_SETTEXTALIGN&&(reinterpret_cast<const EMRSETTEXTALIGN*>(r)->iMode&TA_UPDATECP))
            throw std::runtime_error("Metafile text current-position operation");
        if(r->iType==emrExtEscape||r->iType==emrDrawEscape||r->iType==emrNamedEscape||r->iType==EMR_MASKBLT||r->iType==EMR_PLGBLT||r->iType==EMR_INVERTRGN||r->iType==EMR_EXTFLOODFILL||r->iType==emrSmallText)
            throw std::runtime_error("Metafile driver/region compatibility operation");
        bool drawing=paint(r->iType)&&!insidePath;uint32_t truth=12;Record replacement=copy(r);bool temporaryState{};
        if(drawing) {
            if(r->iType==EMR_BITBLT||r->iType==EMR_STRETCHBLT||r->iType==EMR_STRETCHDIBITS) {
                DWORD rop{};if(r->iType==EMR_BITBLT) rop=reinterpret_cast<const EMRBITBLT*>(r)->dwRop;
                else if(r->iType==EMR_STRETCHBLT) rop=reinterpret_cast<const EMRSTRETCHBLT*>(r)->dwRop;
                else rop=reinterpret_cast<const EMRSTRETCHDIBITS*>(r)->dwRop;
                bool pattern{};truth=reduceRop(rop,pattern);
                if(pattern) {
                    EMRRECTANGLE rectangle{};rectangle.emr={EMR_RECTANGLE,sizeof(rectangle)};
                    if(r->iType==EMR_STRETCHDIBITS) {
                        auto b=reinterpret_cast<const EMRSTRETCHDIBITS*>(r);rectangle.rclBox={b->xDest,b->yDest,b->xDest+b->cxDest,b->yDest+b->cyDest};
                    } else { auto b=reinterpret_cast<const EMRBITBLT*>(r);rectangle.rclBox={b->xDest,b->yDest,b->xDest+b->cxDest,b->yDest+b->cyDest}; }
                    EMRSELECTOBJECT nullPen{};nullPen.emr={EMR_SELECTOBJECT,sizeof(nullPen)};nullPen.ihObject=0x80000000|NULL_PEN;
                    EMR save={EMR_SAVEDC,sizeof(EMR)};state.push_back(copy(&save));state.push_back(copy(&nullPen.emr));temporaryState=true;
                    if(truth==0||truth==5||truth==10||truth==15) {
                        EMRSELECTOBJECT white{};white.emr={EMR_SELECTOBJECT,sizeof(white)};white.ihObject=0x80000000|WHITE_BRUSH;state.push_back(copy(&white.emr));
                    }
                    replacement=copy(&rectangle.emr);
                } else {
                    if(r->iType==EMR_BITBLT) reinterpret_cast<EMRBITBLT*>(replacement.data())->dwRop=SRCCOPY;
                    else if(r->iType==EMR_STRETCHBLT) reinterpret_cast<EMRSTRETCHBLT*>(replacement.data())->dwRop=SRCCOPY;
                    else reinterpret_cast<EMRSTRETCHDIBITS*>(replacement.data())->dwRop=SRCCOPY;
                }
            } else if(r->iType!=EMR_EXTTEXTOUTA&&r->iType!=EMR_EXTTEXTOUTW&&r->iType!=EMR_POLYTEXTOUTA&&r->iType!=EMR_POLYTEXTOUTW&&
                r->iType!=EMR_SETDIBITSTODEVICE&&r->iType!=EMR_ALPHABLEND&&r->iType!=EMR_TRANSPARENTBLT&&r->iType!=EMR_GRADIENTFILL)
                truth=uint32_t(rop2-1);
        }
        if(drawing&&truth!=12) {
            flush();state.push_back(replacement);auto command=input;command.metafile=fragment(original,first,state,eof);
            auto ink=makeGpuScene();ink->serial=++serial;ink->width=width;ink->height=height;ink->commands.push_back(std::move(command));
            SceneCommand composite;composite.kind=SceneCommand::RasterOp;composite.image=ink;composite.state=input.state;composite.rasterTruth=truth;
            output.push_back(std::move(composite));state.pop_back();consumePosition(state,r);
        } else { if(drawing) painted.push_back(state.size());state.push_back(std::move(replacement)); }
        if(temporaryState) { EMRRESTOREDC restore{};restore.emr={EMR_RESTOREDC,sizeof(restore)};restore.iRelative=-1;state.push_back(copy(&restore.emr)); }
    }
    flush();return output;
}
}
