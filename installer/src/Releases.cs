// Where mod releases and their patch notes come from.
//   github:owner/repo   GitHub Releases: the tag is the version, the release text the patch notes,
//                       the asset bwvr-mod-<version>.zip the package (its SHA-256 from GitHub's
//                       asset digest or a <zip>.sha256 asset), JakeypooBWVRModInstaller.exe and
//                       installer.json (its version and SHA-256) the installer; older releases name it
//                       BWVR-Setup-<version>.exe.
//   folder:<path>       a folder with releases.json (installer/package.ps1 writes it) and the zips.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace BwVrSetup
{
    public sealed class Release
    {
        public string Version, Title, Date, Notes, PageUrl;
        public bool Prerelease;
        public string ZipUrl, ZipSha256; public long ZipSize;
        public string SetupVersion, SetupUrl;   // a newer installer: its version, and its release page
        public string SetupDownload, SetupSha256; // the exe itself and its SHA-256, for updating in place
    }

    public sealed class FeedRelease // releases.json entry
    {
        public string version, title, date, notes, zip, sha256, setup, setupVersion, setupSha256;
        public bool prerelease;
        public long size;
    }

    public static class Releases
    {
        public const string InstallerName = "JakeypooBWVRModInstaller.exe";

        public static List<Release> Fetch(string feed)
        {
            List<Release> r;
            if (feed.StartsWith("folder:", StringComparison.OrdinalIgnoreCase)) r = FromFolder(feed.Substring(7));
            else if (feed.StartsWith("github:", StringComparison.OrdinalIgnoreCase)) r = FromGitHub(feed.Substring(7));
            else throw new ArgumentException("unknown release feed " + feed);
            r.Sort((a, b) => -Ver.Compare(a.Version, b.Version));
            return r;
        }

        static List<Release> FromFolder(string dir)
        {
            var list = new List<Release>();
            foreach (var e in Json.Read<List<FeedRelease>>(Path.Combine(dir, "releases.json")))
                list.Add(new Release
                {
                    Version = e.version, Title = e.title ?? e.version, Date = e.date, Notes = e.notes, Prerelease = e.prerelease,
                    ZipUrl = e.zip == null ? null : Path.Combine(dir, e.zip), ZipSha256 = e.sha256, ZipSize = e.size,
                    SetupVersion = e.setupVersion, SetupUrl = e.setup == null ? null : dir,
                    SetupDownload = e.setup == null ? null : Path.Combine(dir, e.setup), SetupSha256 = e.setupSha256,
                });
            return list;
        }

        static List<Release> FromGitHub(string repo)
        {
            var list = new List<Release>();
            string text;
            // "owner/name#id": read by the id (a repo's id survives renames); the name is for messages.
            int hash = repo.IndexOf('#');
            string api = hash >= 0 ? "https://api.github.com/repositories/" + repo.Substring(hash + 1) : "https://api.github.com/repos/" + repo;
            if (hash >= 0) repo = repo.Substring(0, hash);
            try { text = Download.Text(api + "/releases?per_page=50"); }
            catch (System.Net.WebException e) when (e.Response is System.Net.HttpWebResponse h && h.StatusCode == System.Net.HttpStatusCode.NotFound)
            {
                throw new InvalidOperationException("No releases are published at github.com/" + repo + " yet.");
            }
            var arr = Json.ParseAny(text) as object[];
            if (arr == null) return list;
            foreach (var o in arr)
            {
                var d = o as Dictionary<string, object>;
                if (d == null || (d.ContainsKey("draft") && (bool)d["draft"])) continue;
                var r = new Release
                {
                    Version = (Dict.Str(d, "tag_name") ?? "").TrimStart('v', 'V'),
                    Title = Dict.Str(d, "name"), Notes = Dict.Str(d, "body"), PageUrl = Dict.Str(d, "html_url"),
                    Date = (Dict.Str(d, "published_at") ?? "").Split('T')[0],
                    Prerelease = d.ContainsKey("prerelease") && (bool)d["prerelease"],
                };
                string shaUrl = null, newExe = null, newSha = null, infoUrl = null;
                var assets = d.ContainsKey("assets") ? d["assets"] as object[] : null;
                if (assets != null)
                    foreach (var ao in assets)
                    {
                        var a = ao as Dictionary<string, object>;
                        string name = Dict.Str(a, "name") ?? "", url = Dict.Str(a, "browser_download_url");
                        if (Regex.IsMatch(name, @"^bwvr-mod-.*\.zip$", RegexOptions.IgnoreCase))
                        {
                            r.ZipUrl = url;
                            long.TryParse(Dict.Str(a, "size"), out r.ZipSize);
                            string dg = Dict.Str(a, "digest");
                            if (dg != null && dg.StartsWith("sha256:")) r.ZipSha256 = dg.Substring(7).ToLowerInvariant();
                        }
                        else if (Regex.IsMatch(name, @"^bwvr-mod-.*\.zip\.sha256$", RegexOptions.IgnoreCase)) shaUrl = url;
                        else if (string.Equals(name, InstallerName, StringComparison.OrdinalIgnoreCase))
                        {
                            newExe = url;
                            string sd = Dict.Str(a, "digest");
                            if (sd != null && sd.StartsWith("sha256:")) newSha = sd.Substring(7).ToLowerInvariant();
                        }
                        else if (string.Equals(name, "installer.json", StringComparison.OrdinalIgnoreCase)) infoUrl = url;
                        // Up to installer 1.0.4 the version was in the name (still read: a bridge copy).
                        var m = Regex.Match(name, @"^BWVR-Setup-(\d+(?:\.\d+)+)\.exe$", RegexOptions.IgnoreCase);
                        if (m.Success && newExe == null)
                        {
                            r.SetupVersion = m.Groups[1].Value; r.SetupUrl = r.PageUrl ?? url; r.SetupDownload = url;
                            string sd = Dict.Str(a, "digest");
                            if (sd != null && sd.StartsWith("sha256:")) r.SetupSha256 = sd.Substring(7).ToLowerInvariant();
                        }
                    }
                // JakeypooBWVRModInstaller.exe: one name for all versions; installer.json says which.
                if (newExe != null && infoUrl != null)
                {
                    try
                    {
                        var info = Json.ParseAny(Download.Text(infoUrl)) as Dictionary<string, object>;
                        string v = Dict.Str(info, "version"), sha = (Dict.Str(info, "sha256") ?? "").ToLowerInvariant();
                        if (v != null && (newSha == null || sha == newSha))
                        {
                            r.SetupVersion = v; r.SetupUrl = r.PageUrl ?? newExe; r.SetupDownload = newExe;
                            r.SetupSha256 = newSha ?? (sha.Length == 64 ? sha : null);
                        }
                    }
                    catch (Exception e) { Log.Write("installer.json of " + r.Version + " unreadable: " + e.Message); }
                }
                if (r.ZipSha256 == null && shaUrl != null)
                {
                    try { r.ZipSha256 = Download.Text(shaUrl).Trim().Split(' ', '\t', '\n')[0].ToLowerInvariant(); } catch { }
                }
                if (r.Title == null) r.Title = r.Version;
                list.Add(r);
            }
            return list;
        }

        // The release's package on this PC, checked against its SHA-256.
        public static string GetPackage(Release r, Action<string> log, Action<int> percent)
        {
            if (r.ZipUrl == null) throw new InvalidOperationException("release " + r.Version + " has no mod package");
            if (string.IsNullOrEmpty(r.ZipSha256)) throw new InvalidOperationException("release " + r.Version + " has no SHA-256 for its package, so it can't be checked");
            string target = Path.Combine(AppPaths.Packages, "bwvr-mod-" + r.Version + ".zip");
            if (File.Exists(target) && Hash.Sha256(target) == r.ZipSha256) return target;
            if (File.Exists(r.ZipUrl)) File.Copy(r.ZipUrl, target, true);
            else
            {
                log("downloading Black & White VR " + r.Version + "…");
                Download.File(r.ZipUrl, target, (got, total) => { if (percent != null && total > 0) percent((int)(got * 100 / total)); });
            }
            if (Hash.Sha256(target) != r.ZipSha256)
            {
                File.Delete(target);
                throw new InvalidDataException("the downloaded package doesn't match its SHA-256; nothing was installed");
            }
            return target;
        }
    }
}
