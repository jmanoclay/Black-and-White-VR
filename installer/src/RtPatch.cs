// Applies a Pocket Soft RTPatch file (.rtp) with the patchw32.dll that ships inside Lionhead's
// own patch, the engine their installer would have called. The DLL is 32-bit, hence the x86 exe.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace BwVrSetup
{
    public static class RtPatch
    {
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        delegate IntPtr Callback(uint id, IntPtr parm);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        delegate uint ApplyFn([MarshalAs(UnmanagedType.LPStr)] string cmdLine, Callback cb, int waitFlag);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll")]
        static extern bool FreeLibrary(IntPtr module);

        // Patches the files in gameDir. Log gets the engine's messages; progress gets 0..100.
        public static void Apply(string patchDll, string rtpFile, string gameDir, Action<string> log, Action<int> progress)
        {
            IntPtr mod = LoadLibraryW(patchDll);
            if (mod == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "loading " + patchDll);
            IntPtr empty = Marshal.StringToHGlobalAnsi("");
            IntPtr yes = Marshal.StringToHGlobalAnsi("Y");
            try
            {
                IntPtr fn = GetProcAddress(mod, "RTPatchApply32@12");
                if (fn == IntPtr.Zero) throw new EntryPointNotFoundException("RTPatchApply32 not in " + patchDll);
                var apply = (ApplyFn)Marshal.GetDelegateForFunctionPointer(fn, typeof(ApplyFn));
                var sb = new StringBuilder();
                Callback cb = (id, parm) =>
                {
                    if (Environment.GetEnvironmentVariable("BWVR_RTP_TRACE") == "1")
                        log("[cb " + id + "] " + (parm == IntPtr.Zero ? "" : SafeStr(parm)));
                    switch (id)
                    {
                        case 1: case 2: case 3: case 4: case 9: case 12: // warning, error, information, text, progress text, banner
                        {
                            string s = parm == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(parm);
                            sb.Append(s);
                            string all = sb.ToString();
                            int nl;
                            while ((nl = all.IndexOf('\n')) >= 0)
                            {
                                string line = all.Substring(0, nl).TrimEnd('\r');
                                if (line.Trim().Length > 0) log((id == 2 ? "error: " : id == 1 ? "warning: " : "") + line.Trim());
                                all = all.Substring(nl + 1);
                            }
                            sb.Clear().Append(all);
                            return empty;
                        }
                        case 5: // percent complete, as 0..0x8000
                            if (parm != IntPtr.Zero && progress != null)
                                progress((int)Math.Min(100, (Marshal.ReadInt32(parm) & 0xffff) * 100L / 0x8000));
                            return empty;
                        case 0xb: // "is this the right place?" - yes
                            return yes;
                        case 0xa: case 0xf: case 0x11: // search, location change, password, disk change: no
                            log("patch engine asked for input (" + id + "): stopping");
                            return IntPtr.Zero;
                        default:
                            return empty;
                    }
                };
                string cmd = "\"" + gameDir.TrimEnd('\\') + "\" \"" + rtpFile + "\"";
                uint rc = apply(cmd, cb, 1);
                GC.KeepAlive(cb);
                if (sb.Length > 0) log(sb.ToString().Trim());
                if (rc != 0) throw new IOException("the patch engine stopped with code " + rc + " (" + Describe(rc) + ")");
            }
            finally
            {
                Marshal.FreeHGlobal(empty); Marshal.FreeHGlobal(yes);
                FreeLibrary(mod);
            }
        }

        static string SafeStr(IntPtr p)
        {
            try { string s = Marshal.PtrToStringAnsi(p); return s.Length > 200 ? s.Substring(0, 200) : s; }
            catch { return "?"; }
        }

        static string Describe(uint rc)
        {
            switch (rc)
            {
                case 1: return "invalid command line";
                case 2: return "patch file not found or unreadable";
                case 4: return "out of memory";
                case 9: return "a file to patch is in use or read-only";
                case 0x13: return "no files needed patching";
                case 0x20: return "cancelled";
                default: return "see the log";
            }
        }
    }
}
