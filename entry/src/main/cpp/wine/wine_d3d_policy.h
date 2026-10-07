#ifndef WINE_D3D_POLICY_H
#define WINE_D3D_POLICY_H

namespace winehua {

// Both DXVK profiles retain WineD3D for DirectDraw and D3D8/9. Callers
// can still merge explicit per-game overrides after this default policy.
inline const char* DxvkDllOverrides(bool modern26)
{
    return modern26
        ? "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b"
        : "ddraw=b;d3d8=b;d3d9=b;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b";
}

} // namespace winehua
#endif
