// Unpacks the Microsoft cabinet inside Lionhead's patch .exe files (a self-extractor with the
// InstallShield disk set inside), with Windows' own cabinet code (setupapi).
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace BwVrSetup
{
    public static class MsCab
    {
        delegate uint FileCallback(IntPtr context, uint notification, IntPtr param1, IntPtr param2);

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool SetupIterateCabinetW(string cabinetFile, uint reserved, FileCallback callback, IntPtr context);

        const uint SPFILENOTIFY_FILEINCABINET = 0x11;
        const uint SPFILENOTIFY_NEEDNEWCABINET = 0x12;
        const uint FILEOP_DOIT = 1, FILEOP_SKIP = 2;

        // Copies the embedded cabinet out of an .exe (from its "MSCF" header) and unpacks it.
        public static void ExtractFromExe(string exe, string outDir)
        {
            byte[] b = File.ReadAllBytes(exe);
            int at = -1;
            for (int i = 0; i + 36 < b.Length; i++)
            {
                if (b[i] == 'M' && b[i + 1] == 'S' && b[i + 2] == 'C' && b[i + 3] == 'F'
                    && BitConverter.ToUInt32(b, i + 4) == 0)
                {
                    uint size = BitConverter.ToUInt32(b, i + 8);
                    if (size > 0 && i + size <= b.Length && b[i + 24] == 3 && b[i + 25] == 1) { at = i; break; }
                }
            }
            if (at < 0) throw new InvalidDataException(Path.GetFileName(exe) + " has no cabinet inside");
            uint len = BitConverter.ToUInt32(b, at + 8);
            Directory.CreateDirectory(outDir);
            string cab = Path.Combine(outDir, "_outer.cab");
            using (var f = new FileStream(cab, FileMode.Create)) f.Write(b, at, (int)len);
            Extract(cab, outDir);
            File.Delete(cab);
        }

        public static void Extract(string cab, string outDir)
        {
            string error = null;
            FileCallback cb = (ctx, n, p1, p2) =>
            {
                if (n == SPFILENOTIFY_FILEINCABINET)
                {
                    string name = Marshal.PtrToStringUni(Marshal.ReadIntPtr(p1)).TrimStart('\\');
                    string target = Path.GetFullPath(Path.Combine(outDir, name));
                    if (!target.StartsWith(Path.GetFullPath(outDir), StringComparison.OrdinalIgnoreCase))
                        return FILEOP_SKIP;
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    // FILE_IN_CABINET_INFO_W: name ptr, size, error, date, time, attribs, then the target path.
                    char[] chars = (target + "\0").ToCharArray();
                    Marshal.Copy(chars, 0, p1 + IntPtr.Size + 14, chars.Length);
                    return FILEOP_DOIT;
                }
                if (n == SPFILENOTIFY_NEEDNEWCABINET) { error = "cabinet spans disks"; return 1; }
                return 0;
            };
            if (!SetupIterateCabinetW(cab, 0, cb, IntPtr.Zero))
                throw new IOException("unpacking " + Path.GetFileName(cab) + " failed (" + Marshal.GetLastWin32Error() + ")");
            GC.KeepAlive(cb);
            if (error != null) throw new IOException(error);
        }
    }
}
