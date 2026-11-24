using System;
using System.IO;
using System.Net;

namespace BwVrSetup
{
    public static class Download
    {
        public const string UserAgent = "BWVR-Setup (Black & White VR mod installer)";

        public static string Text(string url)
        {
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = UserAgent;
            req.Accept = "application/vnd.github+json, application/json, */*";
            req.Timeout = 20000;
            using (var resp = req.GetResponse())
            using (var r = new StreamReader(resp.GetResponseStream()))
                return r.ReadToEnd();
        }

        // Downloads to a temporary name and moves it into place once complete.
        public static void File(string url, string target, Action<long, long> progress)
        {
            string tmp = target + ".part";
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.UserAgent = UserAgent;
            req.Timeout = 30000;
            req.ReadWriteTimeout = 30000;
            req.AllowAutoRedirect = true;
            using (var resp = (HttpWebResponse)req.GetResponse())
            using (var src = resp.GetResponseStream())
            using (var dst = new FileStream(tmp, FileMode.Create, FileAccess.Write))
            {
                long total = resp.ContentLength, got = 0;
                var buf = new byte[81920];
                int n;
                while ((n = src.Read(buf, 0, buf.Length)) > 0)
                {
                    dst.Write(buf, 0, n);
                    got += n;
                    if (progress != null) progress(got, total);
                }
            }
            if (System.IO.File.Exists(target)) System.IO.File.Delete(target);
            System.IO.File.Move(tmp, target);
        }

        // A patch file: the cached copy if it's the right one, else downloaded. Null if the
        // download failed (the player can then pick the file themselves).
        public static string Patch(PatchSource p, Action<string> log, Action<int> percent)
        {
            string target = Path.Combine(AppPaths.Downloads, p.FileName);
            if (IsGood(p, target)) return target;
            log("downloading " + p.Title + " from " + new Uri(p.Url).Host + " (" + (p.Size / 1048576.0).ToString("0.0") + " MB)");
            try
            {
                File(p.Url, target, (got, total) => { if (percent != null) percent((int)(got * 100 / Math.Max(1, p.Size))); });
            }
            catch (Exception e)
            {
                log("download failed: " + e.Message);
                return null;
            }
            if (!IsGood(p, target))
            {
                log("the downloaded " + p.FileName + " isn't the expected file (size or fingerprint differ); it won't be used");
                try { System.IO.File.Delete(target); } catch { }
                return null;
            }
            return target;
        }

        public static bool IsGood(PatchSource p, string file)
        {
            if (file == null || !System.IO.File.Exists(file)) return false;
            if (new FileInfo(file).Length != p.Size) return false;
            return Hash.Sha256(file) == p.Sha256;
        }

        // A file the player picked: copied into the cache when it's the right one.
        public static bool Adopt(PatchSource p, string file)
        {
            if (!IsGood(p, file)) return false;
            string target = Path.Combine(AppPaths.Downloads, p.FileName);
            if (!string.Equals(Path.GetFullPath(file), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
                System.IO.File.Copy(file, target, true);
            return true;
        }
    }
}
