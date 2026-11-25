#ifndef WINE_D3D_POLICY_H
#define WINE_D3D_POLICY_H

namespace winehua {

// The managed DXVK9 payload is PE32. Allow the builtin when no compatible
// native image is available or a game explicitly requests WineD3D.
inline const char* DxvkDllOverrides(bool modern26, bool d3d9Native = false)
{
    if (modern26)
        return d3d9Native ? "ddraw=b;d3d8=b;d3d9=n,b;d3d11=n;dxgi=n;vulkan-1=b"
                          : "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b";
    return d3d9Native
        ? "ddraw=b;d3d8=b;d3d9=n,b;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b"
        : "ddraw=b;d3d8=b;d3d9=b;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b";
}

} // namespace winehua
#endif
