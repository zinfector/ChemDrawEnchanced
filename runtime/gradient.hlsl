#define D2D_INPUT_COUNT 0
#define D2D_REQUIRES_SCENE_POSITION
#include <d2d1effecthelpers.hlsli>
cbuffer Constants : register(b0) {
    float4 bounds, inverse0, inverse1, center, centerColor, options;
    float4 vertices[256];
    float4 colors[256];
    float4 blend[64];
    float4 stops[64];
};
float cross2(float2 a,float2 b) { return a.x*b.y-a.y*b.x; }
float4 mixColor(float4 a,float4 b,float t) {
    if(options.w!=0) {
        float3 c=lerp(pow(saturate(a.rgb),2.2),pow(saturate(b.rgb),2.2),t);
        return float4(pow(saturate(c),1/2.2),lerp(a.a,b.a,t));
    }
    return lerp(a,b,t);
}
D2D_PS_ENTRY(main) {
    float2 scene=D2DGetScenePosition().xy;
    float2 p=float2(dot(float3(scene,1),inverse0.xyz),dot(float3(scene,1),inverse1.xyz));
    float2 direction=p-center.xy;
    float radius=1e20,inner=0,along=0;int edge=0;
    int count=(int)options.x;
    [loop] for(int i=0;i<count;i++) {
        int j=i+1==count?0:i+1;
        float2 a=vertices[i].xy-center.xy,b=vertices[j].xy-center.xy,e=b-a;
        float denominator=cross2(direction,e);
        if(abs(denominator)>1e-12) {
            float r=cross2(a,e)/denominator;
            float u=cross2(a,direction)/denominator;
            if(r>0&&u>=0&&u<=1&&r<radius) { radius=r;along=u;edge=i; }
        }
        a*=center.zw;b*=center.zw;e=b-a;
        denominator=cross2(direction,e);
        if(abs(denominator)>1e-12) {
            float r=cross2(a,e)/denominator,u=cross2(a,direction)/denominator;
            if(r>0&&u>=0&&u<=1&&(inner==0||r<inner)) inner=r;
        }
    }
    float t=radius>1e19?1:saturate((radius-1)/max(radius-inner,1e-8));
    float4 c;
    if(options.z!=0) {
        c=stops[0];
        [loop] for(int k=1;k<(int)options.y;k++) {
            if(t<=blend[k].x) {
                c=mixColor(stops[k-1],stops[k],saturate((t-blend[k-1].x)/max(blend[k].x-blend[k-1].x,1e-8)));break;
            }
            c=stops[k];
        }
    } else {
        float factor=t;
        [loop] for(int k=1;k<(int)options.y;k++) {
            if(t<=blend[k].x) {
                factor=lerp(blend[k-1].y,blend[k].y,saturate((t-blend[k-1].x)/max(blend[k].x-blend[k-1].x,1e-8)));break;
            }
            factor=blend[k].y;
        }
        c=mixColor(mixColor(colors[edge],colors[edge+1==count?0:edge+1],along),centerColor,factor);
    }
    return float4(c.rgb*c.a,c.a);
}
