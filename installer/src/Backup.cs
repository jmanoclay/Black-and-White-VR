// Backups in "<game>\BWVR Backups\<time> <reason>\": the game files a step is about to change
// (restorable in one go) and a zip of the saves. The installer never deletes them.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;

namespace BwVrSetup
{
    public sealed class BackupEntry
    {
        public string path; public bool existed;
    }

    public sealed class BackupManifest
    {
        public string created, reason;
        public List<BackupEntry> files = new List<BackupEntry>();
    }

    public sealed class BackupSet
    {
        public string Dir;
        public BackupManifest Manifest;

        public static string Root(string gameDir) { return Path.Combine(gameDir, "BWVR Backups"); }

        public static BackupSet Create(string gameDir, string reason)
        {
            string name = DateTime.Now.ToString("yyyy-MM-dd HHmmss") + " " + reason;
            var b = new BackupSet { Dir = Path.Combine(Root(gameDir), name), Manifest = new BackupManifest { created = DateTime.Now.ToString("s"), reason = reason } };
            Directory.CreateDirectory(Path.Combine(b.Dir, "files"));
            return b;
        }

        // Copies the files (relative to the game folder) as they are now; missing ones are noted
        // so a restore removes them again.
        public void Add(string gameDir, IEnumerable<string> rels)
        {
            foreach (var rel in rels)
            {
                if (Manifest.files.Exists(f => string.Equals(f.path, rel, StringComparison.OrdinalIgnoreCase))) continue;
                string src = Sys.Inside(gameDir, rel);
                bool exists = File.Exists(src);
                if (exists) Sys.CopyFile(src, Path.Combine(Dir, "files", rel));
                Manifest.files.Add(new BackupEntry { path = rel, existed = exists });
            }
            Json.Write(Path.Combine(Dir, "backup.json"), Manifest);
        }

        public static List<BackupSet> List(string gameDir)
        {
            var r = new List<BackupSet>();
            string root = Root(gameDir);
            if (!Directory.Exists(root)) return r;
            foreach (var d in Directory.GetDirectories(root))
            {
                string m = Path.Combine(d, "backup.json");
                if (!File.Exists(m)) continue;
                try { r.Add(new BackupSet { Dir = d, Manifest = Json.Read<BackupManifest>(m) }); } catch { }
            }
            r.Sort((a, b) => string.CompareOrdinal(b.Manifest.created, a.Manifest.created));
            return r;
        }

        public void Restore(string gameDir, Action<string> log)
        {
            Sys.EnsureGameNotRunning();
            foreach (var f in Manifest.files)
            {
                string dst = Sys.Inside(gameDir, f.path);
                if (f.existed) Sys.CopyFile(Path.Combine(Dir, "files", f.path), dst);
                else if (File.Exists(dst)) { File.SetAttributes(dst, FileAttributes.Normal); File.Delete(dst); }
            }
            log("restored " + Manifest.files.Count + " file(s) from \"" + Path.GetFileName(Dir) + "\"");
        }

        // The saves: <game>\Profiles, and Windows' redirected copy when the game is under Program Files.
        public static string ZipSaves(string gameDir, string intoDir, Action<string> log)
        {
            var sources = new List<KeyValuePair<string, string>>();
            string prof = Path.Combine(gameDir, "Profiles");
            if (Directory.Exists(prof)) sources.Add(new KeyValuePair<string, string>("Profiles", prof));
            string vs = VirtualStoreDir(gameDir);
            if (vs != null && Directory.Exists(Path.Combine(vs, "Profiles")))
                sources.Add(new KeyValuePair<string, string>("VirtualStore Profiles", Path.Combine(vs, "Profiles")));
            if (sources.Count == 0) { log("no saves to back up yet"); return null; }
            Directory.CreateDirectory(intoDir);
            string zip = Path.Combine(intoDir, "saves.zip");
            int n = 0;
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
                foreach (var s in sources)
                    foreach (var f in Directory.GetFiles(s.Value, "*", SearchOption.AllDirectories))
                    {
                        z.CreateEntryFromFile(f, s.Key + "/" + Sys.Rel(s.Value, f).Replace('\\', '/'), CompressionLevel.Optimal);
                        n++;
                    }
            log("saves backed up: " + n + " file(s) to " + zip);
            return zip;
        }

        public static string VirtualStoreDir(string gameDir)
        {
            try
            {
                string full = Path.GetFullPath(gameDir);
                string root = Path.GetPathRoot(full);
                string vs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "VirtualStore", full.Substring(root.Length));
                return Directory.Exists(vs) ? vs : null;
            }
            catch { return null; }
        }
    }
}
