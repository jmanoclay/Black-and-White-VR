// Brings a retail install to v1.20 + Fan Patch 1.42 without the patches' own installers (their
// InstallShield scripts fail on Windows 11):
//   1.00 -> official 1.20: its RTPatch file applied with its own patchw32.dll, then the files its
//           script copies, and the registry version. (1.20 is cumulative from 1.00; on a 1.10
//           game the 12 files 1.10 changed are first put back from the CD.)
//   1.20 -> Fan Patch 1.42: its 18 files copied out of the NSIS installer, each checked.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;

namespace BwVrSetup
{
    public enum StepKind { RestoreFromCd, Official120, Fan142 }

    public sealed class PatchStep
    {
        public StepKind Kind;
        public string Title;
        public PatchSource Source; // null for the CD step
    }

    public static class Patcher
    {
        public static List<PatchStep> Plan(GameInfo g)
        {
            var s = new List<PatchStep>();
            switch (g.Version)
            {
                case GameVersion.V110:
                    s.Add(new PatchStep { Kind = StepKind.RestoreFromCd, Title = "Put back 12 original files from your Black & White CD (patch 1.20 only updates the retail files)" });
                    goto case GameVersion.V100;
                case GameVersion.V100:
                    s.Add(new PatchStep { Kind = StepKind.Official120, Title = "Official patch v1.20 (includes everything in v1.10)", Source = Known.Patch12 });
                    goto case GameVersion.V120;
                case GameVersion.V120:
                case GameVersion.V120Other:
                    s.Add(new PatchStep { Kind = StepKind.Fan142, Title = "Fan Patch v1.42 (removes the CD protection, which Windows 11 can't run)", Source = Known.Fan142 });
                    break;
            }
            return s;
        }

        // Finds the retail CD (or a mounted image / copied folder) by its InstallShield header.
        public static string FindCd()
        {
            foreach (var d in DriveInfo.GetDrives())
            {
                try
                {
                    if (!d.IsReady) continue;
                    string h = Path.Combine(d.RootDirectory.FullName, "data1.hdr");
                    if (File.Exists(h) && LooksLikeCd(d.RootDirectory.FullName)) return d.RootDirectory.FullName;
                }
                catch { }
            }
            return null;
        }

        public static bool LooksLikeCd(string dir)
        {
            try
            {
                var c = new IsCab(Path.Combine(dir, "data1.hdr"));
                return c.Find("runblack.exe", "Program Files") != null;
            }
            catch { return false; }
        }

        // files: the downloaded patch for each step that has one. cdDir: for the CD step.
        public static void Run(GameInfo g, List<PatchStep> steps, Dictionary<string, string> files, string cdDir,
                               Action<string> log, Action<int> percent)
        {
            Sys.EnsureGameNotRunning();
            var backup = BackupSet.Create(g.Dir, "before patches");
            BackupSet.ZipSaves(g.Dir, backup.Dir, log);
            var touches = new List<string>();
            foreach (var st in steps)
            {
                if (st.Kind == StepKind.RestoreFromCd) foreach (var r in Known.CdRestoreFor110) touches.Add(r[0]);
                if (st.Kind == StepKind.Official120) touches.AddRange(Known.Patch12Touches);
                if (st.Kind == StepKind.Fan142) touches.AddRange(Known.FanTouches());
            }
            backup.Add(g.Dir, touches);
            log("game files backed up to \"" + backup.Dir + "\"");
            foreach (var rel in touches)
            {
                string p = Path.Combine(g.Dir, rel);
                if (File.Exists(p)) File.SetAttributes(p, FileAttributes.Normal); // CD installs can leave them read-only
            }
            try
            {
                foreach (var st in steps)
                {
                    log("— " + st.Title);
                    switch (st.Kind)
                    {
                        case StepKind.RestoreFromCd: RestoreFromCd(g.Dir, cdDir, log); break;
                        case StepKind.Official120: Official120(g.Dir, files[st.Source.Id], log, percent); break;
                        case StepKind.Fan142: Fan142(g.Dir, files[st.Source.Id], log); break;
                    }
                }
            }
            catch
            {
                log("A step failed. Your game files from before patching are in \"" + backup.Dir + "\"; Restore puts them back.");
                throw;
            }
            var after = Game.Inspect(g.Dir);
            if (after.Version != GameVersion.V120Fan) throw new IOException("after patching the game reads as " + Known.Describe(after.Version));
            log("the game is now " + Known.Describe(after.Version));
        }

        static void RestoreFromCd(string gameDir, string cdDir, Action<string> log)
        {
            if (cdDir == null) throw new InvalidOperationException("the Black & White CD wasn't found");
            var cab = new IsCab(Path.Combine(cdDir, "data1.hdr"));
            string tmp = AppPaths.Work;
            foreach (var r in Known.CdRestoreFor110)
            {
                var f = cab.Find(r[2], r[1]);
                if (f == null) throw new FileNotFoundException(r[2] + " isn't on this CD");
                string t = Path.Combine(tmp, Path.GetFileName(r[0]));
                cab.Extract(f, t);
                if (Hash.Sha256(t) != r[3]) throw new InvalidDataException(r[2] + " on this CD isn't the English retail file");
                Sys.CopyFile(t, Path.Combine(gameDir, r[0]));
            }
            log("12 original files restored from the CD");
            if (Game.FromExe(Hash.Sha256(Path.Combine(gameDir, "runblack.exe"))) != GameVersion.V100)
                throw new InvalidDataException("runblack.exe isn't the retail one after restoring from the CD");
        }

        static void Official120(string gameDir, string patchExe, Action<string> log, Action<int> percent)
        {
            string work = AppPaths.Work;
            MsCab.ExtractFromExe(patchExe, work);
            var cab = new IsCab(Path.Combine(work, @"Disk1\data1.hdr"));
            const string engineGroup = "<Support>Language Independent Intel 32 Files";
            string rtp = Path.Combine(work, Known.Rtp12), dll = Path.Combine(work, "patchw32.dll");
            cab.Extract(Need(cab, Known.Rtp12, engineGroup), rtp);
            cab.Extract(Need(cab, "patchw32.dll", engineGroup), dll);
            log("applying the patch (Lionhead's patch engine, RTPatch)…");
            RtPatch.Apply(dll, rtp, gameDir, s => Log.Write("  rtpatch: " + s), percent);
            foreach (var c in Known.Patch12Copies)
            {
                string t = Path.Combine(work, "copy", c[0].Replace("<", "").Replace(">", ""), c[1]);
                cab.Extract(Need(cab, c[1], c[0]), t);
                Sys.CopyFile(t, Path.Combine(gameDir, c[2]));
            }
            Game.SetRegistryVersion(120, log);
            var v = Game.FromExe(Hash.Sha256(Path.Combine(gameDir, "runblack.exe")));
            if (v != GameVersion.V120) throw new IOException("runblack.exe isn't v1.20 after the patch");
            log("official patch v1.20 applied");
            try { Directory.Delete(work, true); } catch { }
        }

        static IsCabFile Need(IsCab c, string name, string group)
        {
            var f = c.Find(name, group);
            if (f == null) throw new FileNotFoundException(name + " isn't in the patch");
            return f;
        }

        static void Fan142(string gameDir, string fanExe, Action<string> log)
        {
            byte[] b = File.ReadAllBytes(fanExe);
            if (Hash.Sha256(b) != Known.Fan142.Sha256) throw new InvalidDataException("this isn't the Fan Patch 1.42 installer the installer knows");
            // runblack.exe last: until it is in, the game doesn't read as patched.
            var order = new List<KnownFile>(Known.FanFiles);
            order.Sort((x, y) => (x.Path == "runblack.exe" ? 1 : 0) - (y.Path == "runblack.exe" ? 1 : 0));
            foreach (var f in order)
            {
                byte[] data = FanFile(b, f);
                string dst = Path.Combine(gameDir, f.Path);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                string tmp = dst + ".bwvr-new";
                File.WriteAllBytes(tmp, data);
                if (File.Exists(dst)) { File.SetAttributes(dst, FileAttributes.Normal); File.Delete(dst); }
                File.Move(tmp, dst);
            }
            log("Fan Patch 1.42: " + Known.FanFiles.Length + " files installed");
        }

        public static byte[] FanFile(byte[] installer, KnownFile f)
        {
            long p = Known.FanDataStart + f.Offset;
            uint n = BitConverter.ToUInt32(installer, (int)p);
            int len = (int)(n & 0x7fffffff);
            byte[] data;
            if ((n & 0x80000000) != 0)
            {
                using (var ms = new MemoryStream(installer, (int)p + 4, len))
                using (var d = new DeflateStream(ms, CompressionMode.Decompress))
                using (var o = new MemoryStream())
                {
                    d.CopyTo(o);
                    data = o.ToArray();
                }
            }
            else
            {
                data = new byte[len];
                Array.Copy(installer, p + 4, data, 0, len);
            }
            if (data.Length != f.Size || Hash.Sha256(data) != f.Sha256)
                throw new InvalidDataException(f.Path + " in the Fan Patch doesn't match");
            return data;
        }
    }
}
