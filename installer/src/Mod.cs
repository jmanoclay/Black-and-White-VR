// Installs the mod from a release package (made by installer/package.ps1):
//   bwvr-mod.json            {"name", "version", "date"}
//   files\...                copied into the game folder, replacing what's there
//   defaults\...             copied only when missing (bwvr\bwvr.ini: the player's settings are never overwritten)
// What was written is recorded in <game>\bwvr\install.json; uninstall removes exactly that, and
// an update removes what the old version had and the new one doesn't.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;

namespace BwVrSetup
{
    public sealed class InstalledFile { public string path, sha256; }

    public sealed class InstallRecord
    {
        public string version, installedAt, installer, package, backup;
        public List<InstalledFile> files = new List<InstalledFile>();
        public List<string> defaults = new List<string>();
        public List<string> restoreOnUninstall = new List<string>(); // files from before the mod, in "backup"
    }

    public sealed class PackageInfo
    {
        public string name, version, date;
    }

    public enum ModState { None, Installed, DevBuild, OtherDdraw }

    public static class Mod
    {
        public static string RecordPath(string gameDir) { return Path.Combine(gameDir, @"bwvr\install.json"); }

        public static InstallRecord Record(string gameDir)
        {
            try { var p = RecordPath(gameDir); return File.Exists(p) ? Json.Read<InstallRecord>(p) : null; }
            catch (Exception e) { Log.Write("install.json unreadable: " + e.Message); return null; }
        }

        public static ModState State(string gameDir, out InstallRecord rec)
        {
            rec = Record(gameDir);
            if (rec != null) return ModState.Installed;
            bool ddraw = File.Exists(Path.Combine(gameDir, "ddraw.dll"));
            // tools\deploy.ps1 puts these in with its ddraw.dll (bwvr\bwvr.ini alone can be left from an uninstall).
            if (ddraw && (File.Exists(Path.Combine(gameDir, "openvr_api.dll")) || Directory.Exists(Path.Combine(gameDir, @"bwvr\input"))))
                return ModState.DevBuild;
            return ddraw ? ModState.OtherDdraw : ModState.None;
        }

        public static PackageInfo ReadPackage(string zip)
        {
            using (var z = ZipFile.OpenRead(zip))
            {
                var e = Entry(z, "bwvr-mod.json");
                if (e == null) throw new InvalidDataException(Path.GetFileName(zip) + " isn't a Black & White VR mod package (no bwvr-mod.json)");
                using (var r = new StreamReader(e.Open()))
                {
                    var p = Json.Parse<PackageInfo>(r.ReadToEnd());
                    if (string.IsNullOrEmpty(p.version)) throw new InvalidDataException("the package has no version");
                    if (Entry(z, "files/ddraw.dll") == null) throw new InvalidDataException("the package has no ddraw.dll");
                    return p;
                }
            }
        }

        // Keeps a copy of the package so Repair works offline.
        public static string Cache(string zip, PackageInfo p)
        {
            string dst = Path.Combine(AppPaths.Packages, "bwvr-mod-" + p.version + ".zip");
            if (!string.Equals(Path.GetFullPath(zip), dst, StringComparison.OrdinalIgnoreCase)) File.Copy(zip, dst, true);
            return dst;
        }

        public static void Install(string gameDir, string zip, Action<string> log)
        {
            Sys.EnsureGameNotRunning();
            var pkg = ReadPackage(zip);
            zip = Cache(zip, pkg);
            InstallRecord old;
            var state = State(gameDir, out old);
            var rec = new InstallRecord
            {
                version = pkg.version, installedAt = DateTime.Now.ToString("s"),
                installer = typeof(Mod).Assembly.GetName().Version.ToString(), package = zip,
            };
            using (var z = ZipFile.OpenRead(zip))
            {
                var newFiles = new List<string>();
                foreach (var e in z.Entries)
                    if (Name(e).StartsWith("files/") && !Name(e).EndsWith("/")) newFiles.Add(Name(e).Substring(6).Replace('/', '\\'));

                // Back up what is about to be replaced: always for a first install (a developer
                // build or another ddraw.dll may be there), else the files the update removes.
                var backupList = new List<string>(newFiles);
                if (old != null) foreach (var f in old.files) backupList.Add(f.path);
                backupList.Add(@"bwvr\bwvr.ini"); // the player's settings, in case a step below changes them
                var backup = BackupSet.Create(gameDir, old == null ? "before VR mod install" : "before VR mod " + pkg.version);
                backup.Add(gameDir, backupList);
                rec.backup = old != null && old.backup != null ? old.backup : backup.Dir;
                if (old != null) rec.restoreOnUninstall = old.restoreOnUninstall;
                else if (state == ModState.OtherDdraw) rec.restoreOnUninstall.Add("ddraw.dll");
                if (state == ModState.OtherDdraw) log("another ddraw.dll was in the game folder: kept in \"" + backup.Dir + "\" and put back on uninstall");
                if (state == ModState.DevBuild) log("a developer build of the mod was in place: replaced (a copy is in the backup)");

                if (old != null)
                    foreach (var f in old.files)
                        if (!newFiles.Exists(n => string.Equals(n, f.path, StringComparison.OrdinalIgnoreCase)))
                        {
                            string p = Sys.Inside(gameDir, f.path);
                            if (File.Exists(p)) { File.SetAttributes(p, FileAttributes.Normal); File.Delete(p); log("removed " + f.path + " (not in " + pkg.version + ")"); }
                        }

                foreach (var e in z.Entries)
                {
                    string name = Name(e);
                    if (name.EndsWith("/")) continue;
                    bool isDefault = name.StartsWith("defaults/");
                    if (!isDefault && !name.StartsWith("files/")) continue;
                    string rel = name.Substring(isDefault ? 9 : 6).Replace('/', '\\');
                    string dst = Sys.Inside(gameDir, rel);
                    if (isDefault)
                    {
                        rec.defaults.Add(rel);
                        if (File.Exists(dst)) continue; // the player's settings stay
                    }
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    if (File.Exists(dst)) File.SetAttributes(dst, FileAttributes.Normal);
                    e.ExtractToFile(dst, true);
                    if (!isDefault) rec.files.Add(new InstalledFile { path = rel, sha256 = Hash.Sha256(dst) });
                }
            }
            Directory.CreateDirectory(Path.Combine(gameDir, "bwvr"));
            if (Ver.Compare(pkg.version, "0.2.1") >= 0) MigrateSettings(gameDir, log); // (older versions want their old settings)
            Json.Write(RecordPath(gameDir), rec);
            log("Black & White VR " + pkg.version + " installed (" + rec.files.Count + " files)");
        }

        // Files that are missing or changed since install.
        public static List<string> Verify(string gameDir, InstallRecord rec)
        {
            var bad = new List<string>();
            foreach (var f in rec.files)
            {
                string p = Sys.Inside(gameDir, f.path);
                if (!File.Exists(p) || Hash.Sha256(p) != f.sha256) bad.Add(f.path);
            }
            return bad;
        }

        public static void Repair(string gameDir, Action<string> log)
        {
            var rec = Record(gameDir);
            if (rec == null) throw new InvalidOperationException("the mod isn't installed");
            var bad = Verify(gameDir, rec);
            if (bad.Count == 0) { log("all " + rec.files.Count + " mod files are as installed"); return; }
            if (rec.package == null || !File.Exists(rec.package))
                throw new FileNotFoundException("the mod's package isn't on this PC any more; use Update or install from a file");
            Sys.EnsureGameNotRunning();
            using (var z = ZipFile.OpenRead(rec.package))
                foreach (var rel in bad)
                {
                    var e = Entry(z, "files/" + rel);
                    if (e == null) continue;
                    string dst = Sys.Inside(gameDir, rel);
                    Directory.CreateDirectory(Path.GetDirectoryName(dst));
                    if (File.Exists(dst)) File.SetAttributes(dst, FileAttributes.Normal);
                    e.ExtractToFile(dst, true);
                    log("repaired " + rel);
                }
        }

        // removeSettings: also move the rest of bwvr\ (settings, logs, captures) to the Recycle Bin.
        public static void Uninstall(string gameDir, bool removeSettings, Action<string> log)
        {
            Sys.EnsureGameNotRunning();
            var rec = Record(gameDir);
            if (rec == null) throw new InvalidOperationException("the mod isn't installed by this installer");
            foreach (var f in rec.files)
            {
                string p = Sys.Inside(gameDir, f.path);
                if (File.Exists(p)) { File.SetAttributes(p, FileAttributes.Normal); File.Delete(p); }
            }
            // Put back what was there before the first install (another ddraw.dll, say).
            foreach (var rel in rec.restoreOnUninstall)
            {
                string from = rec.backup == null ? null : Path.Combine(rec.backup, "files", rel);
                if (from != null && File.Exists(from)) { Sys.CopyFile(from, Sys.Inside(gameDir, rel)); log("put back the original " + rel); }
                else log("the original " + rel + " wasn't found in the backup");
            }
            File.Delete(RecordPath(gameDir));
            string bwvr = Path.Combine(gameDir, "bwvr");
            if (removeSettings && Directory.Exists(bwvr))
            {
                Microsoft.VisualBasic.FileIO.FileSystem.DeleteDirectory(bwvr,
                    Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs, Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
                log("VR settings, logs and captures moved to the Recycle Bin");
            }
            else RemoveEmptyDirs(bwvr);
            log("Black & White VR " + rec.version + " uninstalled; the game is as it was before the mod");
        }

        // Packages up to 0.2.0 installed a bwvr.ini with stereo off and the test head pose
        // ([stereo] enabled=0, [vr] backend=mock): development defaults, not anything a player could
        // choose in the game. From 0.2.1 the mod starts in VR by itself, but an explicit enabled=0
        // would keep it flat, so exactly those two values are switched (the file is in the install's
        // backup); every other setting stays as the player left it.
        static void MigrateSettings(string gameDir, Action<string> log)
        {
            string ini = Path.Combine(gameDir, @"bwvr\bwvr.ini");
            if (!File.Exists(ini)) return;
            if (Ini.Get(ini, "stereo", "enabled") == "0" && Ini.Get(ini, "vr", "backend") == "mock")
            {
                Ini.Set(ini, "stereo", "enabled", "1");
                Ini.Set(ini, "vr", "backend", "openvr");
                log("settings: VR now starts with the game ([stereo] enabled=1, [vr] backend=openvr; the old test values from the first releases)");
            }
        }

        // Entry names with forward slashes (zips made on Windows can carry backslashes).
        static string Name(ZipArchiveEntry e) { return e.FullName.Replace('\\', '/'); }

        static ZipArchiveEntry Entry(ZipArchive z, string name)
        {
            name = name.Replace('\\', '/');
            foreach (var e in z.Entries) if (string.Equals(Name(e), name, StringComparison.OrdinalIgnoreCase)) return e;
            return null;
        }

        static void RemoveEmptyDirs(string dir)
        {
            if (!Directory.Exists(dir)) return;
            foreach (var d in Directory.GetDirectories(dir)) RemoveEmptyDirs(d);
            if (Directory.GetFileSystemEntries(dir).Length == 0) Directory.Delete(dir);
        }
    }
}
