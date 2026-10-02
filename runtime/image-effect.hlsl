#define D2D_INPUT_COUNT 1
#define D2D_INPUT0_SIMPLE
#include <d2d1effecthelpers.hlsli>
cbuffer Constants : register(b0) {
    float4 bounds, options, keyLow, keyHigh;
    float4 colorRows[5];
    float4 grayRows[5];
    float4 table[256];
    float4 remapFrom[64],remapTo[64];
};
D2D_PS_ENTRY(main) {
    float4 c=D2DGetInput(0);
    c.rgb=c.a>0?c.rgb/c.a:0;
    int kind=(int)options.x;
    if(kind==0) {
        int4 index=(int4)round(saturate(c)*255);
        c=float4(table[index.r].r,table[index.g].g,table[index.b].b,table[index.a].a);
    } else if(kind==1) {
        bool gray=abs(c.r-c.g)<0.5/255&&abs(c.r-c.b)<0.5/255;
        if(options.z==2&&gray) c=c.r*grayRows[0]+c.g*grayRows[1]+c.b*grayRows[2]+c.a*grayRows[3]+grayRows[4];
        else if(options.z!=1||!gray) c=c.r*colorRows[0]+c.g*colorRows[1]+c.b*colorRows[2]+c.a*colorRows[3]+colorRows[4];
    } else if(kind==2) c.rgb=pow(saturate(c.rgb),1/max(options.y,0.0001));
    else if(kind==3) c.rgb=step(options.y,c.rgb);
    else if(kind==4) { if(all(c.rgb>=keyLow.rgb)&&all(c.rgb<=keyHigh.rgb)) c=0; }
    else if(kind==5) {
        [loop] for(int i=0;i<(int)options.y;i++) if(all(abs(c-remapFrom[i])<0.5/255)) { c=remapTo[i];break; }
    }
    c=saturate(c);return float4(c.rgb*c.a,c.a);
}
