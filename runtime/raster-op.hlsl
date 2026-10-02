#define D2D_INPUT_COUNT 2
#define D2D_INPUT0_SIMPLE
#define D2D_INPUT1_SIMPLE
#define D2D_ENTRY main
#include <d2d1effecthelpers.hlsli>
cbuffer Constants : register(b0) { float4 options; };
D2D_PS_ENTRY(main) {
    float4 dest=D2DGetInput(0),ink=D2DGetInput(1);
    uint3 d=(uint3)round(saturate(dest.a>0?dest.rgb/dest.a:0)*255);
    uint3 p=(uint3)round(saturate(ink.a>0?ink.rgb/ink.a:0)*255);
    uint truth=(uint)options.x;
    uint3 result=0;
    [unroll] for(uint bit=0;bit<8;bit++) {
        uint3 index=(((p>>bit)&1)<<1)|((d>>bit)&1);
        result|=((truth>>index)&1)<<bit;
    }
    float alpha=lerp(dest.a,1,ink.a);
    return float4(lerp(dest.rgb,float3(result)/255,ink.a),alpha);
}
