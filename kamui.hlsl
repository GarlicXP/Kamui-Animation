// Kamui.hlsl (双向神威版)

cbuffer AnimationParams : register(b0)
{
    float Time;          // 动画进度 0.0 到 1.0
    float2 Center;       // 漩涡中心
    float AspectRatio;   // 宽高比
    bool IsClosing;      // true = 吸入关闭, false = 放出打开
    float3 padding; 
};

Texture2D WindowTexture : register(t0);
SamplerState Sampler : register(s0);

struct PS_INPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD0;
};

PS_INPUT VS(uint id : SV_VertexID) {
    PS_INPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 PS(PS_INPUT input) : SV_TARGET {
    float2 uv = input.Tex;
    float2 coord = uv - Center;
    coord.y /= AspectRatio; 

    float radius = length(coord);
    float angle = atan2(coord.y, coord.x);

    // 【核心双向逻辑】
    // 如果是打开窗口 (false)，progress 从 1.0 递减到 0.0（从无到有）
    // 如果是关闭窗口 (true)，progress 从 0.0 递增到 1.0（从有到无）
    float progress = IsClosing ? Time : (1.0 - Time);

    float pinch = pow(radius, 1.0 - (progress * 0.9)); 
    float twist = progress * 15.0; 
    angle += twist * exp(-radius * 5.0); 

    float2 newCoord = float2(cos(angle), sin(angle)) * pinch;
    newCoord.y *= AspectRatio;
    newCoord += Center;

    if (newCoord.x < 0.0 || newCoord.x > 1.0 || newCoord.y < 0.0 || newCoord.y > 1.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // 打开时透明度从 0 渐变到 1，关闭时相反
    float alpha = IsClosing ? (1.0 - pow(progress, 3.0)) : pow(1.0 - progress, 3.0);
    
    float4 color = WindowTexture.Sample(Sampler, newCoord);
    color.a *= alpha;
    color.rgb *= color.a; // 预乘 Alpha 防止黑边
    
    return color;
}