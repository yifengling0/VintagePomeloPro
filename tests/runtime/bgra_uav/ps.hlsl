RWTexture2D<float4> output : register(u0); void main(float4 pos:SV_Position) { output[uint2(pos.xy)]=float4(0.8,0.4,0.2,1); }
