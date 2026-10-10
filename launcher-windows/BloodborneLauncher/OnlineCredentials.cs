using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace BloodborneLauncher;

internal static class OnlineCredentials
{
    [StructLayout(LayoutKind.Sequential)] struct Blob { public int Size; public IntPtr Data; }
    [DllImport("crypt32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern bool CryptProtectData(ref Blob input, string description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("crypt32.dll", SetLastError=true)]
    static extern bool CryptUnprotectData(ref Blob input, IntPtr description, IntPtr entropy,
        IntPtr reserved, IntPtr prompt, int flags, out Blob output);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);

    static string PathFor(string root, string identity) => Path.Combine(root, "user", "accounts",
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity))) + ".bin");
    static byte[] Transform(byte[] bytes, bool encrypt)
    {
        var source = new Blob { Size=bytes.Length, Data=Marshal.AllocHGlobal(bytes.Length) };
        Blob result = default;
        try
        {
            Marshal.Copy(bytes, 0, source.Data, bytes.Length);
            bool ok = encrypt ? CryptProtectData(ref source, "Bloodborne shadNet", IntPtr.Zero, IntPtr.Zero,
                IntPtr.Zero, 1, out result) : CryptUnprotectData(ref source, IntPtr.Zero, IntPtr.Zero,
                IntPtr.Zero, IntPtr.Zero, 1, out result);
            if (!ok) throw new IOException("Windows no pudo proteger o leer la cuenta guardada.");
            if (result.Size < 0 || result.Size > 65536) throw new IOException("Cuenta guardada no válida.");
            byte[] output = new byte[result.Size];
            Marshal.Copy(result.Data, output, 0, output.Length);
            return output;
        }
        finally
        {
            Marshal.Copy(new byte[bytes.Length], 0, source.Data, bytes.Length);
            Marshal.FreeHGlobal(source.Data);
            if (result.Data != IntPtr.Zero)
            {
                if (result.Size is >=0 and <=65536)
                    Marshal.Copy(new byte[result.Size], 0, result.Data, result.Size);
                LocalFree(result.Data);
            }
        }
    }
    internal static void Save(string root, string identity, string secret)
    {
        byte[] plain = Encoding.UTF8.GetBytes(secret);
        try
        {
            byte[] protectedBytes = Transform(plain, true);
            string path = PathFor(root, identity);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllBytes(path + ".tmp", protectedBytes);
            File.Move(path + ".tmp", path, true);
        }
        finally { CryptographicOperations.ZeroMemory(plain); }
    }
    internal static string Load(string root, string identity)
    {
        string path = PathFor(root, identity);
        if (!File.Exists(path)) return "";
        if (new FileInfo(path).Length > 65536) throw new IOException("Cuenta guardada no válida.");
        byte[] plain = Transform(File.ReadAllBytes(path), false);
        try { return Encoding.UTF8.GetString(plain); }
        finally { CryptographicOperations.ZeroMemory(plain); }
    }
    internal static void Forget(string root, string identity)
    {
        string path = PathFor(root, identity);
        if (File.Exists(path)) File.Delete(path);
    }
}
