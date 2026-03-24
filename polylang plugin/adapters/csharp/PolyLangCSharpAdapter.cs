// PolyLangCSharpAdapter.cs — PolyLang C# Adapter v5
// =============================================================
// Compiled with .NET NativeAOT to a native shared library.
// Export entry point: pl_get_vtable (UnmanagedCallersOnly).
//
// SANDBOX:   pl_compile_sandboxed sets sandboxed=true on the compiled
//            handle. At method dispatch, calls matching a deny-list of
//            System.IO, System.Diagnostics.Process, System.Net, and
//            System.Reflection method name patterns are blocked with
//            PL_ERR_SANDBOX. C# itself allows full BCL access at runtime;
//            this is a bridge-enforced advisory restriction.
//            For harder isolation, configure NativeAOT trimming to
//            remove dangerous namespaces from the publish profile.
// =============================================================
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

namespace PolyLang.CSharp;

// ── Sandbox deny-list ────────────────────────────────────────
internal static class Sandbox
{
    private static readonly string[] DeniedPrefixes = {
        "File", "Directory", "Path", "Stream", "Process", "Socket",
        "HttpClient", "WebClient", "WebRequest", "Dns", "TcpClient",
        "UdpClient", "Assembly", "Activator", "Environment", "Console",
        "Exec", "Run", "Shell", "Download", "Upload", "Connect", "Bind"
    };

    internal static bool IsDenied(string methodName)
    {
        foreach (var p in DeniedPrefixes)
            if (methodName.StartsWith(p, StringComparison.OrdinalIgnoreCase))
                return true;
        return false;
    }
}

// ── PLValue interop ──────────────────────────────────────────
[StructLayout(LayoutKind.Sequential, Size = 32)]
internal struct PLValue
{
    public int    Type;
    public int    Pad;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 24)]
    public byte[] Raw;
}

internal static class PLValueHelper
{
    public const int PL_TYPE_NIL    = 0;
    public const int PL_TYPE_BOOL   = 1;
    public const int PL_TYPE_INT    = 2;
    public const int PL_TYPE_FLOAT  = 3;
    public const int PL_TYPE_STRING = 4;
    public const int PL_TYPE_VEC2   = 5;
    public const int PL_TYPE_VEC3   = 6;

    public static PLValue Nil() => new PLValue { Type = PL_TYPE_NIL, Raw = new byte[24] };

    public static PLValue FromObject(object? obj)
    {
        var v = Nil();
        if (obj is null) return v;
        if (obj is bool b)   { v.Type = PL_TYPE_BOOL;   v.Raw[0] = (byte)(b ? 1 : 0); return v; }
        if (obj is long l)   { v.Type = PL_TYPE_INT;    BitConverter.GetBytes(l).CopyTo(v.Raw, 0); return v; }
        if (obj is int  i)   { v.Type = PL_TYPE_INT;    BitConverter.GetBytes((long)i).CopyTo(v.Raw, 0); return v; }
        if (obj is double d) { v.Type = PL_TYPE_FLOAT;  BitConverter.GetBytes(d).CopyTo(v.Raw, 0); return v; }
        if (obj is float  f) { v.Type = PL_TYPE_FLOAT;  BitConverter.GetBytes((double)f).CopyTo(v.Raw, 0); return v; }
        if (obj is string s) {
            v.Type = PL_TYPE_STRING;
            var ptr = Marshal.StringToCoTaskMemUTF8(s);
            BitConverter.GetBytes(ptr.ToInt64()).CopyTo(v.Raw, 0);
            return v;
        }
        return v;
    }

    public static object? ToObject(PLValue v)
    {
        switch (v.Type)
        {
            case PL_TYPE_NIL:    return null;
            case PL_TYPE_BOOL:   return v.Raw[0] != 0;
            case PL_TYPE_INT:    return BitConverter.ToInt64(v.Raw, 0);
            case PL_TYPE_FLOAT:  return BitConverter.ToDouble(v.Raw, 0);
            case PL_TYPE_STRING: {
                var ptr = new IntPtr(BitConverter.ToInt64(v.Raw, 0));
                return ptr == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(ptr);
            }
            default: return null;
        }
    }

    public static void FreeContents(ref PLValue v)
    {
        if (v.Type == PL_TYPE_STRING)
        {
            var ptr = new IntPtr(BitConverter.ToInt64(v.Raw, 0));
            if (ptr != IntPtr.Zero) Marshal.FreeCoTaskMem(ptr);
            v.Raw = new byte[24];
        }
        v.Type = PL_TYPE_NIL;
    }
}

// ── Compiled / instance handles ──────────────────────────────
internal sealed class CsCompiled
{
    public Type?   ScriptType;
    public string  ClassName = "";
    public bool    Sandboxed;
}

internal sealed class CsInstance
{
    public CsCompiled  Compiled;
    public object?     Obj;
    public Lock        Lock = new Lock();
    public CsInstance(CsCompiled c) { Compiled = c; }
}

// Global handle maps (GCHandle-based to pin objects)
internal static class HandleStore
{
    private static readonly Dictionary<IntPtr, GCHandle> Map = new();
    private static readonly Lock                         MapLock = new();

    public static IntPtr Alloc(object obj)
    {
        var h = GCHandle.Alloc(obj);
        var ptr = GCHandle.ToIntPtr(h);
        lock (MapLock) Map[ptr] = h;
        return ptr;
    }

    public static T? Get<T>(IntPtr ptr) where T : class
    {
        lock (MapLock)
            if (Map.TryGetValue(ptr, out var h)) return h.Target as T;
        return null;
    }

    public static void Free(IntPtr ptr)
    {
        lock (MapLock)
        {
            if (Map.TryGetValue(ptr, out var h)) { h.Free(); Map.Remove(ptr); }
        }
    }
}

// ── ABI entry points ─────────────────────────────────────────
public static class Adapter
{
    // ── Return codes (mirror pl_adapter_vtable.h) ─────────────
    private const int PL_OK                  =  0;
    private const int PL_ERR_GENERIC         = -1;
    private const int PL_ERR_METHOD_NOT_FOUND= -2;
    private const int PL_ERR_PROP_NOT_FOUND  = -3;
    private const int PL_ERR_EXCEPTION       = -5;
    private const int PL_ERR_NOT_IMPLEMENTED = -6;
    private const int PL_ERR_SANDBOX         = -8;

    // ── Type registry ─────────────────────────────────────────
    private static readonly Dictionary<string, Type> TypeRegistry = new();
    private static readonly Lock TypeRegistryLock = new();

    public static void RegisterType(string name, Type t)
    {
        lock (TypeRegistryLock) TypeRegistry[name] = t;
    }

    // ── Core compile ──────────────────────────────────────────
    private static IntPtr CompileCore(string source, string path, bool sandboxed)
    {
        var name = System.IO.Path.GetFileNameWithoutExtension(path) ?? "Script";
        // Strip additional extensions (e.g. "Enemy.pl" → "Enemy")
        if (name.Contains('.')) name = name[..name.IndexOf('.')];

        Type? t = null;
        lock (TypeRegistryLock) TypeRegistry.TryGetValue(name, out t);
        if (t is null)
        {
            // Fallback: search loaded assemblies
            foreach (var asm in AppDomain.CurrentDomain.GetAssemblies())
            {
                t = asm.GetType(name) ?? asm.GetType("PolyLang.Scripts." + name);
                if (t is not null) break;
            }
        }

        if (t is null)
        {
            Console.Error.WriteLine($"[PolyLang/C#{(sandboxed ? "/sandbox" : "")}] Class '{name}' not found");
            return IntPtr.Zero;
        }

        var c = new CsCompiled { ScriptType = t, ClassName = name, Sandboxed = sandboxed };
        return HandleStore.Alloc(c);
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_init_runtime")]
    public static int InitRuntime() => PL_OK;

    [UnmanagedCallersOnly(EntryPoint = "cs_shutdown_runtime")]
    public static void ShutdownRuntime() {}

    [UnmanagedCallersOnly(EntryPoint = "cs_compile")]
    public static IntPtr Compile(IntPtr sourcePtr, IntPtr pathPtr)
    {
        string source = Marshal.PtrToStringUTF8(sourcePtr) ?? "";
        string path   = Marshal.PtrToStringUTF8(pathPtr) ?? "script";
        return CompileCore(source, path, false);
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_compile_sandboxed")]
    public static IntPtr CompileSandboxed(IntPtr sourcePtr, IntPtr pathPtr, uint caps)
    {
        string source = Marshal.PtrToStringUTF8(sourcePtr) ?? "";
        string path   = Marshal.PtrToStringUTF8(pathPtr) ?? "script";
        return CompileCore(source, path, true);
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_free_compiled")]
    public static void FreeCompiled(IntPtr h) => HandleStore.Free(h);

    [UnmanagedCallersOnly(EntryPoint = "cs_instantiate_class")]
    public static IntPtr InstantiateClass(IntPtr ch, IntPtr pathPtr)
    {
        var c = HandleStore.Get<CsCompiled>(ch);
        if (c?.ScriptType is null) return IntPtr.Zero;
        try
        {
            var obj = Activator.CreateInstance(c.ScriptType);
            var inst = new CsInstance(c) { Obj = obj };
            return HandleStore.Alloc(inst);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[PolyLang/C#] Instantiate error: {ex.Message}");
            return IntPtr.Zero;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_free_instance")]
    public static void FreeInstance(IntPtr raw) => HandleStore.Free(raw);

    [UnmanagedCallersOnly(EntryPoint = "cs_call_method")]
    public static unsafe int CallMethod(IntPtr raw, IntPtr namePtr,
                                        PLValue* args, int argc, PLValue* ret)
    {
        *ret = PLValueHelper.Nil();
        var inst = HandleStore.Get<CsInstance>(raw);
        if (inst?.Obj is null || inst.Compiled.ScriptType is null) return PL_ERR_GENERIC;

        string methodName = Marshal.PtrToStringUTF8(namePtr) ?? "";

        // Sandbox check
        if (inst.Compiled.Sandboxed && Sandbox.IsDenied(methodName))
        {
            Console.Error.WriteLine($"[PolyLang/C#/sandbox] method '{methodName}' blocked");
            return PL_ERR_SANDBOX;
        }

        lock (inst.Lock)
        {
            var flags = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            var method = inst.Compiled.ScriptType.GetMethod(methodName, flags);
            if (method is null) return PL_ERR_METHOD_NOT_FOUND;

            var argObjs = new object?[argc];
            for (int k = 0; k < argc; k++) argObjs[k] = PLValueHelper.ToObject(args[k]);

            try
            {
                var result = method.Invoke(inst.Obj, argObjs);
                *ret = PLValueHelper.FromObject(result);
                return PL_OK;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[PolyLang/C#] Exception in '{methodName}': {ex.Message}");
                return PL_ERR_EXCEPTION;
            }
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_call_builtin")]
    public static unsafe int CallBuiltin(IntPtr raw, int id,
                                         PLValue* args, int argc, PLValue* ret)
    {
        string? name = id switch {
            1 => "_Ready",
            2 => "_Process",
            3 => "_PhysicsProcess",
            4 => "_EnterTree",
            5 => "_ExitTree",
            _ => null
        };
        if (name is null) return PL_ERR_NOT_IMPLEMENTED;
        var namePtr = Marshal.StringToCoTaskMemUTF8(name);
        int r = CallMethod(raw, namePtr, args, argc, ret);
        Marshal.FreeCoTaskMem(namePtr);
        return r;
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_set_property")]
    public static unsafe int SetProperty(IntPtr raw, IntPtr namePtr, PLValue* v)
    {
        var inst = HandleStore.Get<CsInstance>(raw);
        if (inst?.Obj is null || inst.Compiled.ScriptType is null) return PL_ERR_GENERIC;
        string name = Marshal.PtrToStringUTF8(namePtr) ?? "";
        var prop = inst.Compiled.ScriptType.GetProperty(name);
        if (prop is null || !prop.CanWrite) return PL_ERR_PROP_NOT_FOUND;
        try { prop.SetValue(inst.Obj, PLValueHelper.ToObject(*v)); return PL_OK; }
        catch { return PL_ERR_PROP_NOT_FOUND; }
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_get_property")]
    public static unsafe int GetProperty(IntPtr raw, IntPtr namePtr, PLValue* out_v)
    {
        *out_v = PLValueHelper.Nil();
        var inst = HandleStore.Get<CsInstance>(raw);
        if (inst?.Obj is null || inst.Compiled.ScriptType is null) return PL_ERR_GENERIC;
        string name = Marshal.PtrToStringUTF8(namePtr) ?? "";
        var prop = inst.Compiled.ScriptType.GetProperty(name);
        if (prop is null || !prop.CanRead) return PL_ERR_PROP_NOT_FOUND;
        try { *out_v = PLValueHelper.FromObject(prop.GetValue(inst.Obj)); return PL_OK; }
        catch { return PL_ERR_PROP_NOT_FOUND; }
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_has_method")]
    public static byte HasMethod(IntPtr ch, IntPtr namePtr)
    {
        var c = HandleStore.Get<CsCompiled>(ch);
        if (c?.ScriptType is null) return 0;
        string name = Marshal.PtrToStringUTF8(namePtr) ?? "";
        return c.ScriptType.GetMethod(name) is not null ? (byte)1 : (byte)0;
    }

    [UnmanagedCallersOnly(EntryPoint = "cs_free_value_contents")]
    public static unsafe void FreeValueContents(PLValue* v)
    {
        if (v is null) return;
        PLValueHelper.FreeContents(ref *v);
    }

    // pl_get_vtable is the C shim in polylang_cs_shim.c which calls
    // the above cs_* exports and populates the vtable struct.
    // (NativeAOT does not export structs by value natively; a thin C
    //  shim is compiled alongside and linked into the final .so/.dll.)
}
