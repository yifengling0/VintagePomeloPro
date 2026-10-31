// SPDX-License-Identifier: MIT
using System;
internal static class ContractReference
{
    public static unsafe int Main()
    {
        delegate* unmanaged<IntPtr, int, int> run = &CoreClrContract.Run;
        return run(IntPtr.Zero, 0);
    }
}
