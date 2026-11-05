RWTexture2D<float4> output : register(u33); [numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { output[id.xy]=float4(0.8,0.4,0.2,1); }
