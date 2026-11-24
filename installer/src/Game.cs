// Finding the game and telling which version it is.
using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Win32;

namespace BwVrSetup
{
    public sealed class GameInfo
    {
        public string Dir;
        public GameVersion Version;
        public string ExeSha256;
        public bool Writable;
        public string Problem; // why it can't be used, when it can't

        public bool Ready { get { return Version == GameVersion.V120Fan; } }
        public string Exe { get { return Path.Combine(Dir, "runblack.exe"); } }
        public string BwvrDir { get { return Path.Combine(Dir, "bwvr"); } }
    }

    public static class Game
    {
        const string RegKey = @"Software\Lionhead Studios Ltd\Black & White";

        // Places a normal install could be, best first.
        public static List<string> Candidates()
        {
            var list = new List<string>();
            Action<string> add = d =>
            {
                if (string.IsNullOrEmpty(d)) return;
                try
                {
                    d = Path.GetFullPath(d.Trim().Trim('"')).TrimEnd('\\');
                    if (File.Exists(Path.Combine(d, "runblack.exe")) && !list.Exists(x => string.Equals(x, d, StringComparison.OrdinalIgnoreCase)))
                        list.Add(d);
                }
                catch { }
            };
            add(Settings.GameDirOverride);
            add(Settings.Current.GameDir);
            foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
                foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
                {
                    try
                    {
                        using (var b = RegistryKey.OpenBaseKey(hive, view))
                        using (var k = b.OpenSubKey(RegKey))
                            if (k != null) add(k.GetValue("GameDir") as string);
                    }
                    catch { }
                }
            // Uninstall entries naming the game.
            foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
            {
                try
                {
                    using (var b = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                    using (var u = b.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall"))
                    {
                        if (u == null) continue;
                        foreach (var n in u.GetSubKeyNames())
                            using (var k = u.OpenSubKey(n))
                            {
                                var name = k == null ? null : k.GetValue("DisplayName") as string;
                                if (name != null && name.IndexOf("Black & White", StringComparison.OrdinalIgnoreCase) >= 0
                                    && name.IndexOf("2", StringComparison.Ordinal) < 0)
                                    add(k.GetValue("InstallLocation") as string);
                            }
                    }
                }
                catch { }
            }
            string pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
            string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            foreach (var root in new[] { pf86, pf })
            {
                add(Path.Combine(root, @"Lionhead Studios Ltd\Black & White"));
                add(Path.Combine(root, @"EA GAMES\Black & White"));
                add(Path.Combine(root, "Black & White"));
            }
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed) continue;
                foreach (var sub in new[] { @"Games\Black & White", "Black & White", @"Games\BW1", "BW1" })
                    add(Path.Combine(drive.RootDirectory.FullName, sub));
            }
            return list;
        }

        public static GameInfo Inspect(string dir)
        {
            var g = new GameInfo { Dir = dir };
            if (dir == null || !File.Exists(Path.Combine(dir, "runblack.exe")))
            {
                g.Version = GameVersion.Missing;
                g.Problem = "Black & White wasn't found. Use Browse to pick the folder that has runblack.exe in it.";
                return g;
            }
            g.ExeSha256 = Hash.Sha256(g.Exe);
            g.Version = FromExe(g.ExeSha256);
            if (g.Version == GameVersion.Unknown && File.Exists(Path.Combine(dir, @"Scripts\InfoScriptMultiplayer2.txt")))
                g.Version = GameVersion.V120Other; // patch 1.2 adds this file
            g.Writable = Sys.CanWrite(dir);
            if (g.Version == GameVersion.Unknown)
                g.Problem = "This runblack.exe isn't one the installer knows (it knows the English retail CD and its patches). " +
                            "Other language editions aren't supported yet.";
            return g;
        }

        public static GameVersion FromExe(string sha)
        {
            switch (sha)
            {
                case Known.Exe100: return GameVersion.V100;
                case Known.Exe110: return GameVersion.V110;
                case Known.Exe120: return GameVersion.V120;
                case Known.Exe120Fan: return GameVersion.V120Fan;
                default: return GameVersion.Unknown;
            }
        }

        // Patch 1.2's installer records the version; the game and its tools read it.
        public static void SetRegistryVersion(int version, Action<string> log)
        {
            using (var k = Registry.CurrentUser.CreateSubKey(RegKey))
                k.SetValue("GameVersion", version, RegistryValueKind.DWord);
            try
            {
                using (var b = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                using (var k = b.OpenSubKey(RegKey, true))
                    if (k != null) k.SetValue("GameVersion", version, RegistryValueKind.DWord);
            }
            catch (Exception e)
            {
                log("the machine-wide game version wasn't updated (" + e.GetType().Name + "); the per-user one was");
            }
        }
    }
}
