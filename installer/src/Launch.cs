// SteamVR and starting the game in VR.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;

namespace BwVrSetup
{
    public static class SteamVr
    {
        // The runtime folder from OpenVR's own registry file.
        public static string RuntimeDir()
        {
            try
            {
                string f = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"openvr\openvrpaths.vrpath");
                if (!File.Exists(f)) return null;
                var d = Json.ParseAny(File.ReadAllText(f)) as Dictionary<string, object>;
                var rt = d != null && d.ContainsKey("runtime") ? d["runtime"] as object[] : null;
                if (rt == null) return null;
                foreach (var o in rt)
                {
                    string dir = o as string;
                    if (dir != null && File.Exists(Path.Combine(dir, @"bin\win64\vrstartup.exe"))) return dir;
                }
            }
            catch { }
            return null;
        }

        public static bool Installed { get { return RuntimeDir() != null; } }

        public static bool Running
        {
            get
            {
                var p = Process.GetProcessesByName("vrserver");
                foreach (var x in p) x.Dispose();
                return p.Length > 0;
            }
        }

        public static void Start()
        {
            string dir = RuntimeDir();
            if (dir == null) throw new InvalidOperationException("SteamVR isn't installed");
            Process.Start(new ProcessStartInfo(Path.Combine(dir, @"bin\win64\vrstartup.exe")) { UseShellExecute = true, WorkingDirectory = Path.Combine(dir, @"bin\win64") });
        }
    }

    public static class Launcher
    {
        // Starts SteamVR (if it isn't running) and the game. The mod (0.2.1 on) turns VR on by itself;
        // with older settings that keep it off, the harness's old "bwctl vr" steps follow: once the
        // last save has loaded with the in-game menu open, close the menu and turn on stereo through
        // SteamVR, centred on where the player stands.
        public static void Run(GameInfo g, Action<string> status, CancellationToken cancel)
        {
            if (Sys.GameRunning()) throw new InvalidOperationException("Black & White is already running.");
            bool startVr = Environment.GetEnvironmentVariable("BWVR_LAUNCH_NO_STEAMVR") != "1"; // (installer tests)
            if (startVr && SteamVr.Installed && !SteamVr.Running)
            {
                status("Starting SteamVR…");
                SteamVr.Start();
                var until = DateTime.Now.AddSeconds(60);
                while (!SteamVr.Running && DateTime.Now < until) { if (cancel.IsCancellationRequested) return; Thread.Sleep(500); }
                Thread.Sleep(3000);
            }
            string control = Path.Combine(g.BwvrDir, "control");
            Directory.CreateDirectory(control);
            try { File.Delete(Path.Combine(control, "status.txt")); } catch { }
            string logFile = Path.Combine(g.BwvrDir, @"logs\bwvr.log");
            status("Starting Black & White…");
            var proc = Process.Start(new ProcessStartInfo(g.Exe) { WorkingDirectory = g.Dir, UseShellExecute = false });
            Log.Write("launched " + g.Exe);

            // The mod is up once it reports a Direct3D device.
            var deadline = DateTime.Now.AddSeconds(150);
            while (DateTime.Now < deadline && !StatusSays(control, "device", "1"))
            {
                if (cancel.IsCancellationRequested || proc.HasExited) { Exited(proc, status); return; }
                Thread.Sleep(500);
            }
            status("Loading… (put your headset on)");
            if (VrFromStart(g))
            {
                // From 0.2.1 the mod turns VR on by itself on the game's first 3D frame.
                Log.Write("the mod starts VR by itself (bwvr.ini)");
                status("Black & White is starting in VR. The other hand's A opens the menu; Ctrl+F12 recentres the view.");
                return;
            }
            // Older settings (stereo off, the test head pose): the harness's steps.
            // The menu shows once the save has loaded (the mod logs its text starting); Esc closes it.
            bool menu = false;
            var t0 = DateTime.Now;
            while ((DateTime.Now - t0).TotalSeconds < 90)
            {
                if (cancel.IsCancellationRequested || proc.HasExited) { Exited(proc, status); return; }
                if (LogSince(logFile, proc.StartTime).Contains("ui: menu text starts")) { menu = true; break; }
                Thread.Sleep(1000);
            }
            Thread.Sleep(2000);
            if (menu) Request(control, "input.req", "key esc\n");
            Thread.Sleep(menu ? 4000 : 0);
            Request(control, "stereo.req", "enabled 1\nvr openvr\nrecenter\n");
            Log.Write("VR on" + (menu ? " (menu closed)" : " (no menu seen)"));
            status("Black & White is running in VR. Ctrl+F12 recentres the view.");
        }

        // Whether the mod's settings let it start VR itself: [stereo] enabled and [vr] backend,
        // missing meaning the mod's defaults (on, SteamVR, from 0.2.1).
        public static bool VrFromStart(GameInfo g)
        {
            string ini = Path.Combine(g.BwvrDir, "bwvr.ini");
            string en = File.Exists(ini) ? Ini.Get(ini, "stereo", "enabled") : null;
            string be = File.Exists(ini) ? Ini.Get(ini, "vr", "backend") : null;
            return (en == null || en == "1") && (be == null || string.Equals(be, "openvr", StringComparison.OrdinalIgnoreCase));
        }

        static void Exited(Process p, Action<string> status)
        {
            if (p.HasExited) status("The game closed (exit code " + p.ExitCode + "). See the mod's log for details.");
        }

        static bool StatusSays(string control, string key, string value)
        {
            try
            {
                foreach (var tok in File.ReadAllText(Path.Combine(control, "status.txt")).Split())
                    if (tok == key + "=" + value) return true;
            }
            catch { }
            return false;
        }

        static string LogSince(string file, DateTime started)
        {
            try
            {
                if (!File.Exists(file) || File.GetLastWriteTime(file) < started) return "";
                using (var s = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var r = new StreamReader(s)) return r.ReadToEnd();
            }
            catch { return ""; }
        }

        // The mod's control files: written aside, then moved into place in one step.
        static void Request(string control, string name, string text)
        {
            string tmp = Path.Combine(control, name + ".tmp"), dst = Path.Combine(control, name);
            File.WriteAllText(tmp, text);
            for (int i = 0; ; i++)
            {
                try
                {
                    if (File.Exists(dst)) File.Delete(dst);
                    File.Move(tmp, dst);
                    return;
                }
                catch (IOException) when (i < 20) { Thread.Sleep(20); }
                catch (UnauthorizedAccessException) when (i < 20) { Thread.Sleep(20); }
            }
        }
    }
}
