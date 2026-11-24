// Command-line tools for checking the installer's parts without its window (used in development
// "JakeypooBWVRModInstaller.exe --cli <command> ...".
using System;
using System.IO;

namespace BwVrSetup
{
    static class DevCli
    {
        public static int Run(string[] a)
        {
            switch (a[0])
            {
                case "mscab":
                    MsCab.ExtractFromExe(a[1], a[2]);
                    return 0;
                case "rtpatch":
                    RtPatch.Apply(a[1], a[2], a[3], Console.WriteLine, null);
                    return 0;
                case "cab-list":
                {
                    var c = new IsCab(a[1]);
                    foreach (var f in c.Files)
                        Console.WriteLine("{0,5} {1,-4} v{2} {3,10} {4,-40} {5}", f.Index, f.Valid ? "" : "x", f.Volume, f.ExpandedSize, f.Group, f.Path);
                    return 0;
                }
                case "cab-extract":
                {
                    var c = new IsCab(a[1]);
                    foreach (var f in c.Files)
                    {
                        if (!f.Valid) continue;
                        if (a.Length > 3 && f.Group != a[3]) continue;
                        c.Extract(f, Path.Combine(a[2], Safe(f.Group ?? "_") + "\\" + Safe(f.Path)));
                    }
                    return 0;
                }
                // The installer's own steps, as its window runs them (installer/test.ps1).
                case "detect":
                {
                    var g = Game.Inspect(a.Length > 1 ? a[1] : (Game.Candidates().Count > 0 ? Game.Candidates()[0] : null));
                    InstallRecord rec = null;
                    var ms = g.Version == GameVersion.Missing ? ModState.None : Mod.State(g.Dir, out rec);
                    Console.WriteLine("dir=" + g.Dir);
                    Console.WriteLine("version=" + g.Version + " (" + Known.Describe(g.Version) + ")");
                    Console.WriteLine("writable=" + g.Writable);
                    Console.WriteLine("mod=" + ms + (rec != null ? " " + rec.version : ""));
                    if (g.Version != GameVersion.Missing && g.Version != GameVersion.Unknown)
                        foreach (var st in Patcher.Plan(g)) Console.WriteLine("needs: " + st.Title);
                    Console.WriteLine("steamvr=" + (SteamVr.Installed ? (SteamVr.Running ? "running" : "installed") : "missing"));
                    return 0;
                }
                case "adopt": // adopt <patch id> <file>: use a patch file already on this PC
                {
                    foreach (var p in new[] { Known.Patch12, Known.Fan142 })
                        if (p.Id == a[1]) { bool ok = Download.Adopt(p, a[2]); Console.WriteLine(ok ? "ok" : "not the expected file"); return ok ? 0 : 1; }
                    return 2;
                }
                case "download": // download <patch id>
                {
                    foreach (var p in new[] { Known.Patch12, Known.Fan142 })
                        if (p.Id == a[1]) return Download.Patch(p, Console.WriteLine, null) != null ? 0 : 1;
                    return 2;
                }
                case "patch": // patch <game dir> [cd dir]: downloads (or uses the cached) patches and applies them
                {
                    var g = Game.Inspect(a[1]);
                    var plan = Patcher.Plan(g);
                    var files = new System.Collections.Generic.Dictionary<string, string>();
                    foreach (var st in plan)
                        if (st.Source != null) files[st.Source.Id] = Download.Patch(st.Source, Console.WriteLine, null) ?? throw new IOException("no " + st.Source.FileName);
                    Patcher.Run(g, plan, files, a.Length > 2 ? a[2] : Patcher.FindCd(), Console.WriteLine, null);
                    return 0;
                }
                case "releases":
                    foreach (var r in Releases.Fetch(a.Length > 1 ? a[1] : Settings.Current.EffectiveFeed))
                        Console.WriteLine(r.Version + (r.Prerelease ? " (beta)" : "") + "  " + r.Date + "  " + r.ZipUrl + "  sha256=" + r.ZipSha256 + "  setup=" + r.SetupVersion);
                    return 0;
                case "install": // install <game dir> <zip | version from the feed>
                {
                    string zip = a[2];
                    if (!File.Exists(zip))
                        foreach (var r in Releases.Fetch(Settings.Current.EffectiveFeed))
                            if (r.Version == a[2]) zip = Releases.GetPackage(r, Console.WriteLine, null);
                    Mod.Install(a[1], zip, Console.WriteLine);
                    return 0;
                }
                case "verify":
                {
                    var rec = Mod.Record(a[1]);
                    if (rec == null) { Console.WriteLine("not installed"); return 1; }
                    var bad = Mod.Verify(a[1], rec);
                    foreach (var b in bad) Console.WriteLine("changed: " + b);
                    Console.WriteLine(bad.Count == 0 ? "ok " + rec.version : bad.Count + " changed");
                    return bad.Count == 0 ? 0 : 1;
                }
                case "repair": Mod.Repair(a[1], Console.WriteLine); return 0;
                case "uninstall": Mod.Uninstall(a[1], a.Length > 2 && a[2] == "all", Console.WriteLine); return 0;
                case "screenshot": // screenshot <png> [game dir]: the window as it opens, for checking the layout
                {
                    System.Windows.Forms.Application.EnableVisualStyles();
                    if (a.Length > 2) Settings.GameDirOverride = a[2];
                    var f = new MainForm();
                    if (a.Length > 2) Settings.GameDirOverride = a[2];
                    var t = new System.Windows.Forms.Timer { Interval = 2500 };
                    t.Tick += (s, e) =>
                    {
                        t.Stop();
                        if (Environment.GetEnvironmentVariable("BWVR_SHOT_DETAILS") == "1") f.ToggleDetails();
                        { int tab; if (int.TryParse(Environment.GetEnvironmentVariable("BWVR_SHOT_TAB"), out tab)) f.ShowTab(tab); } // 1 controls, 2 VR features, 3 known issues
                        // BWVR_SHOT_RESIZE=<width>x<height>: grow the window there in steps, as a drag does,
                        // and capture the screen itself (what the player sees, repaint faults included).
                        string rs = Environment.GetEnvironmentVariable("BWVR_SHOT_RESIZE");
                        bool screen = !string.IsNullOrEmpty(rs);
                        if (Environment.GetEnvironmentVariable("BWVR_SHOT_MENU") == "1") // the mod's More menu, open (from the screen)
                        {
                            f.Activate(); f.OpenModMenu();
                            for (int k = 0; k < 20; k++) { System.Windows.Forms.Application.DoEvents(); System.Threading.Thread.Sleep(25); }
                        }
                        if (screen)
                        {
                            var wh = rs.Split('x');
                            int w1 = int.Parse(wh[0]), h1 = int.Parse(wh[1]), w0 = f.Width, h0 = f.Height;
                            f.Activate();
                            for (int k = 1; k <= 12; k++)
                            {
                                f.Size = new System.Drawing.Size(w0 + (w1 - w0) * k / 12, h0 + (h1 - h0) * k / 12);
                                System.Windows.Forms.Application.DoEvents();
                                System.Threading.Thread.Sleep(40);
                            }
                            for (int k = 0; k < 20; k++) { System.Windows.Forms.Application.DoEvents(); System.Threading.Thread.Sleep(25); }
                        }
                        using (var bmp = new System.Drawing.Bitmap(f.Width, f.Height))
                        {
                            if (screen || Environment.GetEnvironmentVariable("BWVR_SHOT_MENU") == "1")
                                using (var g = System.Drawing.Graphics.FromImage(bmp)) g.CopyFromScreen(f.Left, f.Top, 0, 0, bmp.Size);
                            else f.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, f.Width, f.Height));
                            bmp.Save(a[1], System.Drawing.Imaging.ImageFormat.Png);
                        }
                        f.Close();
                    };
                    f.Shown += (s, e) => t.Start();
                    System.Windows.Forms.Application.Run(f);
                    return 0;
                }
                case "launch": // launch <game dir>: Launch in VR without the window (no checks of the game's version)
                {
                    var g = new GameInfo { Dir = a[1] };
                    Launcher.Run(g, Console.WriteLine, System.Threading.CancellationToken.None);
                    return 0;
                }
                case "self-update": // self-update <feed> <marker file>: update this exe in place from the feed
                {
                    Release best = null;
                    foreach (var r in Releases.Fetch(a[1]))
                        if (r.SetupVersion != null && (best == null || Ver.Compare(r.SetupVersion, best.SetupVersion) > 0)) best = r;
                    if (best == null || Ver.Compare(best.SetupVersion, MainForm.AppVersion) <= 0) { Console.WriteLine("up to date"); return 0; }
                    if (!SelfUpdate.CanUpdateInPlace(best)) { Console.WriteLine("can't update in place"); return 1; }
                    SelfUpdate.Apply(best, "--cli updated \"" + a[2] + "\"", Console.WriteLine, null);
                    return 0;
                }
                case "updated": // (started by self-update) removes the old exe, notes its own version
                    System.Threading.Thread.Sleep(500);
                    string old = a.Length > 3 && a[2] == "--old" ? a[3] : SelfUpdate.TakeOwnName(); // as Program does
                    SelfUpdate.CleanUp(old);
                    File.WriteAllText(a[1], MainForm.AppVersion + " as " + Path.GetFileName(SelfUpdate.ExePath) + " old-left=" + File.Exists(old));
                    return 0;
                case "report": // report <game dir> [png]: the bug-report text and link (and the window, as a picture)
                {
                    var g = Game.Inspect(a[1]);
                    string v = Report.Versions(g), l = Report.LogExcerpt(g);
                    Console.WriteLine(v); Console.WriteLine("---"); Console.WriteLine(l); Console.WriteLine("---");
                    string url = Report.Url(true, "Test title", v, l);
                    Console.WriteLine(url.Length + " characters: " + url.Substring(0, Math.Min(300, url.Length)) + "...");
                    if (a.Length > 2)
                    {
                        System.Windows.Forms.Application.EnableVisualStyles();
                        var f = new ReportForm(g);
                        f.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
                        f.Shown += (s, e) =>
                        {
                            var t = new System.Windows.Forms.Timer { Interval = 800 };
                            t.Tick += (s2, e2) =>
                            {
                                t.Stop();
                                using (var bmp = new System.Drawing.Bitmap(f.Width, f.Height))
                                {
                                    f.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, f.Width, f.Height));
                                    bmp.Save(a[2], System.Drawing.Imaging.ImageFormat.Png);
                                }
                                f.Close();
                            };
                            t.Start();
                        };
                        System.Windows.Forms.Application.Run(f);
                    }
                    return 0;
                }
                case "count-ping": // count-ping <url> <mod version>: one launch ping (id from BWVR_STATS_ID, else all zeros: a test)
                {
                    string id = Environment.GetEnvironmentVariable("BWVR_STATS_ID") ?? "00000000000000000000000000000000";
                    Console.WriteLine("status " + LaunchCount.Post(a[1], id, a[2], MainForm.AppVersion));
                    return 0;
                }
                case "count-ask": // count-ask <png>: the counting question, as a picture
                {
                    System.Windows.Forms.Application.EnableVisualStyles();
                    var f = new AskForm("Count my launches", LaunchCount.Question, new[] { "Yes, count my launches", "No thanks" });
                    f.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
                    var t = new System.Windows.Forms.Timer { Interval = 800 };
                    t.Tick += (s, e) =>
                    {
                        t.Stop();
                        using (var bmp = new System.Drawing.Bitmap(f.Width, f.Height))
                        {
                            f.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, f.Width, f.Height));
                            bmp.Save(a[1], System.Drawing.Imaging.ImageFormat.Png);
                        }
                        f.Close();
                    };
                    f.Shown += (s, e) => t.Start();
                    System.Windows.Forms.Application.Run(f);
                    return 0;
                }
                case "backups":
                    foreach (var b in BackupSet.List(a[1])) Console.WriteLine(b.Manifest.created + "  " + b.Manifest.reason + "  " + b.Manifest.files.Count + " files");
                    return 0;
                case "restore": // restore <game dir> [index, 0 = newest]
                {
                    var l = BackupSet.List(a[1]);
                    l[a.Length > 2 ? int.Parse(a[2]) : 0].Restore(a[1], Console.WriteLine);
                    return 0;
                }
            }
            Console.Error.WriteLine("unknown command " + a[0]);
            return 2;
        }

        static string Safe(string s)
        {
            foreach (char c in "<>:\"|?*") s = s.Replace(c, '_');
            return s;
        }
    }
}
