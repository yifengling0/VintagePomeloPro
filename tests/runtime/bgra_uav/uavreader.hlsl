RWTexture2D<float4> input : register(u0); RWStructuredBuffer<float4> output : register(u1); [numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) { output[id.x+id.y*4]=input[id.xy]; }
