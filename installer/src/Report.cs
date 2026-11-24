// "Report a bug": opens a pre-filled GitHub issue form on the public releases repo
// (.github/ISSUE_TEMPLATE/bug_report.yml and feedback.yml there). The player writes the report
// and sends it with their own GitHub account; the installer only fills in the versions and a short,
// cleaned part of the mod's log, and shows exactly that before anything opens.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace BwVrSetup
{
    public static class Report
    {
        public const string DefaultRepo = "jmanoclay/Black-and-White-VR";
        const int MaxUrl = 7500;  // browsers and GitHub cope with about 8 KB of URL
        const int MaxLog = 2400;

        public static string Repo
        {
            get
            {
                string f = Settings.Current.EffectiveFeed;
                return f.StartsWith("github:", StringComparison.OrdinalIgnoreCase) ? f.Substring(7).Split('#')[0] : DefaultRepo;
            }
        }

        public static string Versions(GameInfo g)
        {
            var sb = new StringBuilder();
            sb.AppendLine("Installer: " + MainForm.AppVersion);
            if (g != null && g.Version != GameVersion.Missing)
            {
                InstallRecord rec;
                var ms = Mod.State(g.Dir, out rec);
                sb.AppendLine("VR mod: " + (ms == ModState.Installed ? rec.version : ms == ModState.DevBuild ? "developer build" : "not installed"));
                sb.AppendLine("Game: " + Known.Describe(g.Version) + (g.Dir.IndexOf("Program Files", StringComparison.OrdinalIgnoreCase) >= 0 ? " (under Program Files)" : ""));
                string hmd = HeadsetFromLog(g);
                if (hmd != null) sb.AppendLine("Headset: " + hmd);
            }
            else sb.AppendLine("Game: not found");
            sb.AppendLine("SteamVR: " + (SteamVr.Installed ? (SteamVr.Running ? "running" : "installed") : "not installed"));
            sb.AppendLine("Windows: " + WindowsVersion());
            return Clean(sb.ToString().TrimEnd());
        }

        static string WindowsVersion()
        {
            try
            {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                {
                    string name = k.GetValue("ProductName") as string ?? "Windows";
                    string build = k.GetValue("CurrentBuildNumber") as string ?? "";
                    int b; if (int.TryParse(build, out b) && b >= 22000) name = name.Replace("Windows 10", "Windows 11");
                    return name + " " + (k.GetValue("DisplayVersion") as string ?? "") + " (build " + build + ")";
                }
            }
            catch { return Environment.OSVersion.VersionString; }
        }

        static string LogPath(GameInfo g) { return g == null || g.Dir == null ? null : Path.Combine(g.BwvrDir, @"logs\bwvr.log"); }

        static string[] LogLines(GameInfo g)
        {
            string p = LogPath(g);
            if (p == null || !File.Exists(p)) return new string[0];
            using (var s = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var r = new StreamReader(s)) return r.ReadToEnd().Replace("\r", "").Split('\n');
        }

        static string HeadsetFromLog(GameInfo g)
        {
            try
            {
                foreach (var l in LogLines(g))
                {
                    int i = l.IndexOf("hmd profile: ", StringComparison.Ordinal);
                    if (i >= 0) { string s = l.Substring(i + 13); int semi = s.IndexOf(';'); return semi > 0 ? s.Substring(0, semi) : s; }
                }
            }
            catch { }
            return null;
        }

        // Crash lines, then the last lines, cleaned and kept short.
        public static string LogExcerpt(GameInfo g)
        {
            string[] lines;
            try { lines = LogLines(g); } catch (Exception e) { return "(the mod's log couldn't be read: " + e.Message + ")"; }
            if (lines.Length == 0) return "(no mod log yet)";
            var keep = new List<string>();
            foreach (var l in lines) if (l.Contains("CRASH")) keep.Add(l);
            if (keep.Count > 12) keep.RemoveRange(12, keep.Count - 12);
            if (keep.Count > 0) keep.Add("...");
            int from = Math.Max(0, lines.Length - 30);
            for (int i = from; i < lines.Length; i++) if (lines[i].Trim().Length > 0) keep.Add(lines[i]);
            string text = Clean(string.Join("\n", keep));
            if (text.Length > MaxLog) text = "...\n" + text.Substring(text.Length - MaxLog);
            return text;
        }

        // Takes out what could identify the player: their user name and profile path, the PC's
        // name, and anything shaped like an email address.
        public static string Clean(string s)
        {
            if (s == null) return "";
            string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (!string.IsNullOrEmpty(profile)) s = Regex.Replace(s, Regex.Escape(profile), "%USERPROFILE%", RegexOptions.IgnoreCase);
            foreach (var name in new[] { Environment.UserName, Environment.MachineName })
                if (!string.IsNullOrEmpty(name) && name.Length >= 3)
                    s = Regex.Replace(s, @"(?<![A-Za-z0-9])" + Regex.Escape(name) + @"(?![A-Za-z0-9])", "<name>", RegexOptions.IgnoreCase);
            s = Regex.Replace(s, @"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}", "<email>");
            return s;
        }

        // The new-issue address with the form's fields filled in.
        public static string Url(bool bug, string title, string versions, string log)
        {
            string baseUrl = "https://github.com/" + Repo + "/issues/new?template=" + (bug ? "bug_report.yml" : "feedback.yml");
            Func<string, string> build = l =>
            {
                var u = new StringBuilder(baseUrl);
                u.Append("&title=").Append(Uri.EscapeDataString((bug ? "[Bug] " : "[Idea] ") + (title ?? "").Trim()));
                u.Append("&versions=").Append(Uri.EscapeDataString(versions));
                if (bug && !string.IsNullOrEmpty(l)) u.Append("&log=").Append(Uri.EscapeDataString(l));
                return u.ToString();
            };
            string url = build(log);
            while (url.Length > MaxUrl && log.Length > 200) { log = "...\n" + log.Substring(log.Length * 2 / 3); url = build(log); }
            return url;
        }
    }
}
