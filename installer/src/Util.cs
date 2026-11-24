using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Web.Script.Serialization;

namespace BwVrSetup
{
    public static class Log
    {
        public static event Action<string> Line;
        static readonly object gate = new object();
        public static string FilePath { get { return Path.Combine(AppPaths.Data, "installer.log"); } }

        public static void Write(string s)
        {
            string line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + s;
            lock (gate)
            {
                try { File.AppendAllText(FilePath, line + Environment.NewLine); } catch { }
            }
            var h = Line;
            if (h != null) h(s);
        }
    }

    public static class AppPaths
    {
        public static string Data
        {
            get
            {
                string d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BWVR");
                Directory.CreateDirectory(d);
                return d;
            }
        }
        public static string Downloads { get { var d = Path.Combine(Data, "downloads"); Directory.CreateDirectory(d); return d; } }
        public static string Packages { get { var d = Path.Combine(Data, "packages"); Directory.CreateDirectory(d); return d; } }
        public static string Work
        {
            get
            {
                var d = Path.Combine(Data, "work", DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Process.GetCurrentProcess().Id);
                Directory.CreateDirectory(d);
                return d;
            }
        }
    }

    public static class Hash
    {
        public static string Sha256(string file)
        {
            using (var s = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var h = SHA256.Create())
                return Hex(h.ComputeHash(s));
        }
        public static string Sha256(byte[] b)
        {
            using (var h = SHA256.Create()) return Hex(h.ComputeHash(b));
        }
        static string Hex(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            foreach (byte x in b) sb.Append(x.ToString("x2"));
            return sb.ToString();
        }
    }

    public static class Json
    {
        static JavaScriptSerializer S() { return new JavaScriptSerializer { MaxJsonLength = 64 << 20 }; }
        public static T Read<T>(string file) { return S().Deserialize<T>(File.ReadAllText(file)); }
        public static T Parse<T>(string text) { return S().Deserialize<T>(text); }
        public static object ParseAny(string text) { return S().DeserializeObject(text); }
        public static void Write(string file, object o)
        {
            string tmp = file + ".tmp";
            File.WriteAllText(tmp, Pretty(S().Serialize(o)));
            if (File.Exists(file)) File.Replace(tmp, file, null); else File.Move(tmp, file);
        }

        // JavaScriptSerializer writes one line; indent it so people can read the file.
        static string Pretty(string j)
        {
            var sb = new StringBuilder(); int ind = 0; bool q = false;
            for (int i = 0; i < j.Length; i++)
            {
                char c = j[i];
                if (c == '"' && (i == 0 || j[i - 1] != '\\')) q = !q;
                if (q) { sb.Append(c); continue; }
                if (c == '{' || c == '[') { sb.Append(c).Append('\n').Append(' ', (++ind) * 2); }
                else if (c == '}' || c == ']') { sb.Append('\n').Append(' ', (--ind) * 2).Append(c); }
                else if (c == ',') { sb.Append(c).Append('\n').Append(' ', ind * 2); }
                else if (c == ':') sb.Append(": ");
                else sb.Append(c);
            }
            return sb.ToString();
        }
    }

    public static class Sys
    {
        public static bool GameRunning()
        {
            var p = Process.GetProcessesByName("runblack");
            foreach (var x in p) x.Dispose();
            return p.Length > 0;
        }

        // Every change to the game folder goes through this first.
        public static void EnsureGameNotRunning()
        {
            if (GameRunning()) throw new InvalidOperationException("Black & White is running. Close the game first, then try again.");
        }

        public static bool IsAdmin()
        {
            using (var id = WindowsIdentity.GetCurrent())
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
        }

        public static bool CanWrite(string dir)
        {
            try
            {
                string probe = Path.Combine(dir, ".bwvr-write-test-" + Guid.NewGuid().ToString("N"));
                File.WriteAllText(probe, "");
                File.Delete(probe);
                return true;
            }
            catch { return false; }
        }

        public static void CopyFile(string from, string to)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(to));
            if (File.Exists(to)) File.SetAttributes(to, FileAttributes.Normal);
            File.Copy(from, to, true);
        }

        public static void OpenUrl(string url)
        {
            try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch (Exception e) { Log.Write("couldn't open " + url + ": " + e.Message); }
        }

        public static string Rel(string root, string path)
        {
            root = Path.GetFullPath(root).TrimEnd('\\') + "\\";
            path = Path.GetFullPath(path);
            return path.StartsWith(root, StringComparison.OrdinalIgnoreCase) ? path.Substring(root.Length) : path;
        }

        // A relative path from a package or manifest, refused if it points outside the folder.
        public static string Inside(string root, string rel)
        {
            string full = Path.GetFullPath(Path.Combine(root, rel));
            string r = Path.GetFullPath(root).TrimEnd('\\') + "\\";
            if (!full.StartsWith(r, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("path outside the game folder: " + rel);
            return full;
        }
    }

    public static class Ver
    {
        // "0.3.1", "v0.3.1-beta" -> comparable; unknown parts count as 0.
        public static int Compare(string a, string b)
        {
            int[] x = Parts(a), y = Parts(b);
            for (int i = 0; i < 4; i++) if (x[i] != y[i]) return x[i].CompareTo(y[i]);
            return 0;
        }
        static int[] Parts(string v)
        {
            var r = new int[4];
            if (string.IsNullOrEmpty(v)) return r;
            v = v.TrimStart('v', 'V');
            int dash = v.IndexOfAny(new[] { '-', '+', ' ' });
            if (dash >= 0) v = v.Substring(0, dash);
            var p = v.Split('.');
            for (int i = 0; i < p.Length && i < 4; i++) int.TryParse(p[i], out r[i]);
            return r;
        }
    }

    public static class Dict
    {
        public static string Str(IDictionary<string, object> d, string k)
        {
            object v; return d != null && d.TryGetValue(k, out v) && v != null ? v.ToString() : null;
        }
    }
}
