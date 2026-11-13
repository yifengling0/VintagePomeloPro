// SPDX-License-Identifier: MIT
using System;
using System.Collections.Concurrent;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

public static class CoreClrContract
{
    private sealed class Node { public int Value = 123; }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int ReadNode(Node node) => node.Value;
    [DllImport("libc.so.6", CallingConvention = CallingConvention.Cdecl)]
    private static extern unsafe void qsort(int* data, nuint count, nuint size,
        delegate* unmanaged<int*, int*, int> compare);
    [UnmanagedCallersOnly]
    private static unsafe int Compare(int* left, int* right)
    {
        var item = new Item();
        item.Dispose();
        return (*left).CompareTo(*right);
    }
    private static readonly ConcurrentDictionary<WeakReference<Item>, byte> Registered = new();
    private sealed class Item : IDisposable
    {
        private readonly WeakReference<Item> reference;
        public Item() { reference = new(this); Registered.TryAdd(reference, 0); }
        public void Dispose() { Registered.TryRemove(reference, out _); GC.SuppressFinalize(this); }
        ~Item() { Registered.TryRemove(reference, out _); }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static int Arithmetic(int x) => unchecked((x * 31 + 17) ^ (x >> 2));

    [UnmanagedCallersOnly]
    public static unsafe int Run(IntPtr unused, int ignored)
    {
        try
        {
            Console.Error.WriteLine("CORECLR_CONTRACT phase=entered version=" + Environment.Version);
            int checksum = 0;
            for (int i = 0; i < 100000; ++i) checksum ^= Arithmetic(i);
            Console.Error.WriteLine("CORECLR_CONTRACT phase=arithmetic passed=1 checksum=" + checksum);
            for (int i = 0; i < 128; ++i)
            {
                var method = new DynamicMethod("contract", typeof(int), Type.EmptyTypes);
                var il = method.GetILGenerator(); il.Emit(OpCodes.Ldc_I4, i); il.Emit(OpCodes.Ret);
                if (((Func<int>)method.CreateDelegate(typeof(Func<int>)))() != i) return 2;
            }
            Console.Error.WriteLine("CORECLR_CONTRACT phase=dynamic-jit passed=1");
            bool nullCaught = false;
            try { checksum ^= ReadNode(null!); }
            catch (NullReferenceException) { nullCaught = true; }
            if (!nullCaught) return 3;
            Console.Error.WriteLine("CORECLR_CONTRACT phase=implicit-null-signal passed=1");
            int[] values = new int[2048];
            for (int i = 0; i < values.Length; ++i) values[i] = values.Length - i;
            if (!OperatingSystem.IsWindows())
            {
                fixed (int* data = values) qsort(data, (nuint)values.Length, sizeof(int), &Compare);
                for (int i = 0; i < values.Length; ++i) if (values[i] != i + 1) return 4;
                Console.Error.WriteLine("CORECLR_CONTRACT phase=unmanaged-callback passed=1");
            }
            Parallel.For(0, 4, thread =>
            {
                for (int i = 0; i < 5000; ++i)
                {
                    var item = new Item();
                    if ((i & 1) == 0) item.Dispose();
                    if ((i & 1023) == 0) GC.Collect();
                }
            });
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
            Console.Error.WriteLine("CORECLR_CONTRACT phase=dictionary-gc passed=1 remaining=" + Registered.Count);
            Console.Error.WriteLine("CORECLR_CONTRACT_RESULT passed=1");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("CORECLR_CONTRACT_RESULT passed=0 type=" + error.GetType().FullName);
            return 1;
        }
    }
}
