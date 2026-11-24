// Updating the installer in place. Windows won't let a running exe be overwritten, but it can be
// renamed: the new exe is downloaded next to this one and checked (SHA-256 from the release, and
// its own version), this one is renamed to "<name>.old", the new one takes its name (or, for a copy
// still named BWVR-Setup-<version>.exe, JakeypooBWVRModInstaller.exe) and is started with
// --old <that file>, and this one exits. The new one deletes the .old file when it starts. If the folder can't be written, the player is sent to the release page instead.
using System;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading;

namespace BwVrSetup
{
    public static class SelfUpdate
    {
        public static string ExePath { get { return Process.GetCurrentProcess().MainModule.FileName; } }

        public static bool CanUpdateInPlace(Release r)
        {
            return r != null && r.SetupDownload != null && !string.IsNullOrEmpty(r.SetupSha256)
                && Sys.CanWrite(Path.GetDirectoryName(ExePath));
        }

        // Downloads and checks the new installer, swaps it in and starts it. On success the caller
        // exits. On failure nothing has changed (or the swap is undone) and it throws.
        public static void Apply(Release r, string restartArgs, Action<string> log, Action<int> percent)
        {
            string exe = ExePath, dir = Path.GetDirectoryName(exe);
            string fresh = exe + ".new", old = exe + ".old";
            try { if (File.Exists(fresh)) File.Delete(fresh); } catch { }
            log("downloading installer " + r.SetupVersion + "…");
            if (File.Exists(r.SetupDownload)) File.Copy(r.SetupDownload, fresh, true); // a local feed
            else Download.File(r.SetupDownload, fresh, (got, total) => { if (percent != null && total > 0) percent((int)(got * 100 / total)); });
            if (Hash.Sha256(fresh) != r.SetupSha256)
            {
                File.Delete(fresh);
                throw new InvalidDataException("the downloaded installer doesn't match its SHA-256; nothing was changed");
            }
            string v = FileVersionInfo.GetVersionInfo(fresh).ProductVersion;
            if (Ver.Compare(v, r.SetupVersion) != 0)
            {
                File.Delete(fresh);
                throw new InvalidDataException("the downloaded installer says it is version " + v + ", not " + r.SetupVersion + "; nothing was changed");
            }
            if (File.Exists(old)) File.Delete(old); // left from an earlier update, if its cleanup failed
            // A copy still named with its version (BWVR-Setup-1.0.4.exe, up to 1.0.4) takes the
            // installer's one name, since its version would be wrong from now on.
            string target = exe;
            if (Regex.IsMatch(Path.GetFileName(exe), @"^BWVR-Setup(-\d+(\.\d+)*)?\.exe$", RegexOptions.IgnoreCase))
                target = Path.Combine(dir, Releases.InstallerName);
            File.Move(exe, old);
            try
            {
                if (target != exe && File.Exists(target)) File.Delete(target); // another, older copy of the installer
                File.Move(fresh, target);
            }
            catch { File.Move(old, exe); throw; }
            log("installer updated to " + r.SetupVersion + (target != exe ? " (now " + Path.GetFileName(target) + ")" : "") + "; restarting");
            Process.Start(new ProcessStartInfo(target, restartArgs + " --old \"" + old + "\"") { UseShellExecute = false, WorkingDirectory = dir });
        }

        // Right after an update by 1.0.3 or 1.0.4, this copy still has the old one's name,
        // BWVR-Setup-<old version>.exe: it takes the installer's one name (a running exe can be renamed).
        // Returns where the previous version was left (<old name>.old), for CleanUp.
        public static string TakeOwnName()
        {
            string exe = ExePath, dir = Path.GetDirectoryName(exe), left = exe + ".old";
            try
            {
                if (!Regex.IsMatch(Path.GetFileName(exe), @"^BWVR-Setup(-\d+(\.\d+)*)?\.exe$", RegexOptions.IgnoreCase)) return left;
                string target = Path.Combine(dir, Releases.InstallerName);
                if (File.Exists(target)) return left;
                File.Move(exe, target);
                Log.Write("renamed " + Path.GetFileName(exe) + " to " + Releases.InstallerName);
            }
            catch (Exception e) { Log.Write("installer not renamed: " + e.Message); }
            return left;
        }

        // At start after an update: remove the previous version (it may take a moment to exit).
        public static void CleanUp(string old)
        {
            if (string.IsNullOrEmpty(old) || !old.EndsWith(".old", StringComparison.OrdinalIgnoreCase) || !File.Exists(old)) return;
            for (int i = 0; i < 20; i++)
            {
                try { File.Delete(old); Log.Write("removed the previous installer (" + Path.GetFileName(old) + ")"); return; }
                catch { Thread.Sleep(250); }
            }
        }
    }
}
