// The installer and launcher window: a checklist (game, patches, SteamVR, the VR mod), the
// patch notes, and Launch in VR.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace BwVrSetup
{
    public sealed class MainForm : Form
    {
        readonly StatusRow rowGame = new StatusRow("Black & White");
        readonly StatusRow rowPatch = new StatusRow("Game patches");
        readonly StatusRow rowVr = new StatusRow("SteamVR");
        readonly StatusRow rowMod = new StatusRow("Black & White VR mod");
        readonly RichTextBox notes = new RichTextBox();
        readonly RichTextBox controls = new RichTextBox();
        readonly RichTextBox features = new PictureTextBox(); // can show the built-in pictures
        readonly RichTextBox issues = new RichTextBox();
        readonly TabStrip tabs = new TabStrip();
        string controlsShownFrom;
        DateTime controlsTime;

        static string BuiltInControls() { return BuiltIn("BwVrSetup.controls.md"); }

        static string BuiltIn(string name)
        {
            using (var s = typeof(MainForm).Assembly.GetManifestResourceStream(name))
            using (var r = new StreamReader(s)) return r.ReadToEnd();
        }

        // Tab ids, as saved in the settings: 0 patch notes, 1 controls, 2 VR features, 3 known issues.
        // Shown in the order patch notes, known issues, VR features, controls.
        static readonly int[] TabPosition = { 0, 3, 2, 1 };

        int featuresWidth = -1;
        void RenderFeatures()
        {
            if (!features.IsHandleCreated || features.ClientSize.Width == featuresWidth) return;
            featuresWidth = features.ClientSize.Width;
            features.Clear();
            Notes.RenderPage(features, BuiltIn("BwVrSetup.features.md"));
            features.SelectionStart = 0; features.ScrollToCaret();
        }

        // For screenshots: the shown tab scrolled so that the text is at the top.
        internal void ScrollShownTab(string text)
        {
            foreach (var box in new[] { notes, controls, features, issues })
            {
                if (!box.Visible) continue;
                int at = box.Text.IndexOf(text, StringComparison.Ordinal);
                if (at < 0) return;
                box.SelectionStart = box.TextLength; box.ScrollToCaret(); // to the end first, so the text lands at the top
                box.SelectionStart = at; box.SelectionLength = 0; box.ScrollToCaret();
            }
        }

        internal void ShowTab(int i, bool remember = false)
        {
            if (i < 0 || i > 3) i = 0;
            notes.Visible = i == 0; controls.Visible = i == 1; features.Visible = i == 2; issues.Visible = i == 3;
            tabs.Selected = TabPosition[i];
            if (i == 2) RenderFeatures(); // drawn when first seen (needs its box on screen)
            if (remember && Settings.Current.Tab != i) { Settings.Current.Tab = i; Settings.Current.Save(); }
        }
        readonly Label status = new Label();
        readonly ProgressBar progress = new ProgressBar();
        readonly FlatButton launch = new FlatButton("Launch in VR", true);
        readonly TextBox details = new TextBox();
        readonly LinkLabel detailsLink = new LinkLabel();
        const string DiscordInvite = "https://discord.com/invite/YzWQUZSsYZ";
        readonly FlatButton updateBanner = new FlatButton("");

        GameInfo game;
        List<Release> releases;
        string feedError;
        bool busy, exiting;
        CancellationTokenSource launchCancel;

        public static string AppVersion { get { var v = typeof(MainForm).Assembly.GetName().Version; return v.Major + "." + v.Minor + "." + v.Build; } }

        public MainForm()
        {
            Text = "Jakeypoo's Black & White VR Mod Installer";
            AutoScaleMode = AutoScaleMode.Dpi; AutoScaleDimensions = new SizeF(96, 96);
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            ClientSize = new Size(1000, 660);
            MinimumSize = new Size(860, 600);
            // After a drag ends, or on maximise/restore, repaint everything once more.
            FormWindowState lastState = WindowState;
            ResizeEnd += (s, e) => Invalidate(true);
            SizeChanged += (s, e) => { if (WindowState != lastState) { lastState = WindowState; Invalidate(true); RenderFeatures(); } };
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            // Header
            var header = new Panel { Dock = DockStyle.Top, Height = 92, BackColor = Theme.Back };
            var title = new Label { Text = "Black & White VR", UseMnemonic = false, Font = Theme.Title, ForeColor = Theme.Gold, AutoSize = true, Location = new Point(24, 16) };
            var sub = new Label
            {
                Text = "Installer and launcher  ·  version " + AppVersion + "  ·  Lionhead's 2001 god game, in your headset",
                Font = Theme.Body, ForeColor = Theme.Dim, AutoSize = true, Location = new Point(27, 58),
            };
            updateBanner.UseMnemonic = false; updateBanner.Visible = false; updateBanner.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            updateBanner.FlatAppearance.BorderColor = Theme.Gold; updateBanner.ForeColor = Theme.Gold; updateBanner.MinimumSize = new Size(0, 32);
            updateBanner.Click += BannerClick;
            header.Controls.Add(title); header.Controls.Add(sub); header.Controls.Add(updateBanner);
            header.Resize += (s, e) => updateBanner.Location = new Point(header.Width - updateBanner.Width - 24, 26);

            // Checklist on the left, patch notes on the right.
            var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, Padding = new Padding(20, 0, 20, 0), BackColor = Theme.Back };
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 58));
            body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 42));
            var left = new Panel { Dock = DockStyle.Fill, Padding = new Padding(0, 0, 10, 0), BackColor = Theme.Back };
            foreach (var r in new[] { rowMod, rowVr, rowPatch, rowGame })
            {
                left.Controls.Add(r);
                left.Controls.Add(new Panel { Dock = DockStyle.Top, Height = 8, BackColor = Theme.Back });
            }
            var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(10, 0, 0, 8), BackColor = Theme.Back };
            // Four tabs: the patch notes, the known issues (the "Known issues" section of the newest
            // release's text), what the mod does in VR (built in), and the VR controls (from the installed
            // mod, else the installer's own copy).
            foreach (var box in new[] { notes, controls, features, issues })
            {
                box.Dock = DockStyle.Fill; box.ReadOnly = true; box.BorderStyle = BorderStyle.None; box.BackColor = Theme.Panel;
                box.ForeColor = Theme.Text; box.DetectUrls = true; box.LinkClicked += (s, e) => Sys.OpenUrl(e.LinkText);
                // No automatic font switching: for a character like an arrow it changed the font of the
                // text after it, so bold landed on the wrong words.
                box.LanguageOption = RichTextBoxLanguageOptions.UIFonts;
                // Repaint the whole pane when its size changes: while the window was dragged, the text
                // wrapped anew but bits of the old lines stayed on screen.
                box.SizeChanged += (s, e) => ((Control)s).Invalidate();
                box.VScroll += (s, e) => ((Control)s).Invalidate();
            }
            var notesFrame = new Panel { Dock = DockStyle.Fill, Padding = new Padding(12), BackColor = Theme.Panel };
            notesFrame.Controls.Add(notes); notesFrame.Controls.Add(controls); notesFrame.Controls.Add(features); notesFrame.Controls.Add(issues);
            // The VR features page is drawn once the window is up (its pictures need the box on screen
            // and its width), and again when the window's width changes.
            Shown += (s, e) => RenderFeatures();
            ResizeEnd += (s, e) => RenderFeatures();
            tabs.Add("Patch notes", () => ShowTab(0, true));
            tabs.Add("Known issues", () => ShowTab(3, true));
            tabs.Add("VR features", () => ShowTab(2, true));
            tabs.Add("Controls", () => ShowTab(1, true));
            right.Controls.Add(notesFrame); right.Controls.Add(tabs);
            ShowTab(Settings.Current.Tab);
            body.Controls.Add(left, 0, 0); body.Controls.Add(right, 1, 0);

            // Footer: progress, status, details, Launch.
            var footer = new Panel { Dock = DockStyle.Bottom, Height = 86, BackColor = Theme.Panel, Padding = new Padding(24, 12, 24, 12) };
            launch.Font = Theme.Big; launch.MinimumSize = new Size(220, 50); launch.Anchor = AnchorStyles.Right | AnchorStyles.Top;
            launch.Click += (s, e) => OnLaunch();
            status.AutoSize = false; status.UseMnemonic = false; status.ForeColor = Theme.Text; status.Location = new Point(24, 14); status.AutoEllipsis = true;
            progress.Location = new Point(24, 40); progress.Height = 8; progress.Style = ProgressBarStyle.Continuous; progress.Visible = false;
            detailsLink.Text = "Show details"; detailsLink.AutoSize = true; detailsLink.LinkColor = Theme.Dim; detailsLink.ActiveLinkColor = Theme.Gold;
            detailsLink.Location = new Point(24, 56);
            detailsLink.LinkClicked += (s, e) => ToggleDetails();
            var more = new LinkLabel { Text = "Folders and backups", AutoSize = true, LinkColor = Theme.Dim, ActiveLinkColor = Theme.Gold };
            more.LinkClicked += (s, e) => ShowFolderMenu(more);
            var report = new LinkLabel { Text = "Report a bug or an idea", AutoSize = true, LinkColor = Theme.Gold, ActiveLinkColor = Theme.Text };
            report.LinkClicked += (s, e) => { using (var f = new ReportForm(game)) f.ShowDialog(this); };
            // The community's Discord (an invite that doesn't expire, opening on #welcome).
            var discord = new LinkLabel { Text = "Join the Discord", AutoSize = true, LinkColor = Theme.Gold, ActiveLinkColor = Theme.Text };
            discord.LinkClicked += (s, e) => Sys.OpenUrl(DiscordInvite);
            footer.Controls.Add(status); footer.Controls.Add(progress); footer.Controls.Add(detailsLink); footer.Controls.Add(more); footer.Controls.Add(report); footer.Controls.Add(discord); footer.Controls.Add(launch);
            footer.Resize += (s, e) =>
            {
                launch.Location = new Point(footer.Width - launch.Width - 24, (footer.Height - launch.Height) / 2);
                status.Width = launch.Left - 48; progress.Width = launch.Left - 48;
                more.Location = new Point(detailsLink.Right + 18, 56);
                report.Location = new Point(more.Right + 18, 56);
                discord.Location = new Point(report.Right + 18, 56);
            };

            details.Multiline = true; details.ReadOnly = true; details.ScrollBars = ScrollBars.Vertical; details.Dock = DockStyle.Bottom;
            details.Height = 150; details.Visible = false; details.BackColor = Theme.Back; details.ForeColor = Theme.Dim;
            details.BorderStyle = BorderStyle.None; details.Font = new Font("Consolas", 9f);

            Controls.Add(body); Controls.Add(details); Controls.Add(footer); Controls.Add(header);

            Log.Line += s => UI(() => { details.AppendText(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + s + Environment.NewLine); SetStatus(s); });
            Shown += (s, e) =>
            {
                LoadLogTail(); Reload(true);
                if (AskOnOpen && !Settings.Current.AskedCountLaunches) BeginInvoke(new Action(AskCountLaunches));
            };
            FormClosing += (s, e) =>
            {
                if (busy && !exiting && AskForm.Show(this, "Black & White VR", "The installer is still working. Close it anyway?", "Keep working", "Close") != "Close") e.Cancel = true;
            };
        }

        [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        // A dark title bar to match (Windows 10 2004 and later; ignored before).
        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            int on = 1;
            try { DwmSetWindowAttribute(Handle, 20, ref on, 4); } catch { }
        }

        void UI(Action a) { if (IsHandleCreated && !IsDisposed) BeginInvoke(a); }
        void SetStatus(string s) { status.Text = s; }

        // What stands between the player and Launch in VR.
        string ReadyText()
        {
            if (game == null || game.Version == GameVersion.Missing) return "Pick the Black & White folder to begin.";
            if (game.Version == GameVersion.Unknown) return "This copy of Black & White isn't supported yet.";
            if (!game.Ready) return "Next: install the game patches.";
            InstallRecord rec;
            var ms = Mod.State(game.Dir, out rec);
            if (ms != ModState.Installed && ms != ModState.DevBuild) return "Next: install the VR mod.";
            if (!SteamVr.Installed) return "Next: install SteamVR.";
            return "Ready to play. Plug in your headset and press Launch in VR.";
        }

        internal void ToggleDetails()
        {
            // The window grows by the pane's height (as far as the screen allows), so the checklist keeps its room.
            bool show = !details.Visible;
            if (WindowState == FormWindowState.Normal)
            {
                var screen = Screen.FromControl(this).WorkingArea;
                if (show) { Height = Math.Min(Height + details.Height, screen.Height); if (Bottom > screen.Bottom) Top = Math.Max(screen.Top, screen.Bottom - Height); }
                else Height = Math.Max(MinimumSize.Height, Height - details.Height);
            }
            details.Visible = show;
            detailsLink.Text = details.Visible ? "Hide details" : "Show details";
            if (details.Visible) { details.SelectionStart = details.TextLength; details.ScrollToCaret(); }
        }

        // The details pane shows the installer's log: its recent history, then what happens now.
        void LoadLogTail()
        {
            try
            {
                if (!File.Exists(Log.FilePath)) return;
                string[] lines;
                using (var s = new FileStream(Log.FilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var r = new StreamReader(s)) lines = r.ReadToEnd().Replace("\r", "").Split('\n');
                int from = Math.Max(0, lines.Length - 300);
                details.Text = string.Join(Environment.NewLine, lines, from, lines.Length - from).TrimEnd() + Environment.NewLine;
            }
            catch (Exception e) { details.Text = "(the installer's log couldn't be read: " + e.Message + ")" + Environment.NewLine; }
        }

        // Re-reads everything; online: also fetch the releases.
        void Reload(bool online)
        {
            if (game == null || online)
            {
                var c = Game.Candidates();
                game = Game.Inspect(c.Count > 0 ? c[0] : null);
                InstallRecord rec = null;
                var ms = game.Version == GameVersion.Missing ? ModState.None : Mod.State(game.Dir, out rec);
                Log.Write(game.Version == GameVersion.Missing ? "game: not found (" + c.Count + " candidate folders)" :
                    "game: " + game.Dir + ", " + Known.Describe(game.Version) + (game.Writable ? "" : ", needs administrator rights to change"));
                Log.Write("VR mod: " + (ms == ModState.Installed ? "installed " + rec.version : ms == ModState.DevBuild ? "developer build" :
                    ms == ModState.OtherDdraw ? "not installed (another ddraw.dll is present)" : "not installed") +
                    "; SteamVR: " + (SteamVr.Installed ? (SteamVr.Running ? "running" : "installed") : "not installed"));
                Log.Write("checking releases at " + Settings.Current.EffectiveFeed);
            }
            else game = Game.Inspect(game.Dir);
            ShowRows();
            if (online)
            {
                SetStatus("Checking for mod updates…");
                Task.Run(() =>
                {
                    try
                    {
                        releases = Releases.Fetch(Settings.Current.EffectiveFeed); feedError = null;
                        Log.Write(releases.Count == 0 ? "no releases published yet" : releases.Count + " release(s), newest " + releases[0].Version);
                    }
                    catch (Exception e) { releases = null; feedError = e.Message; Log.Write("couldn't read releases from " + Settings.Current.EffectiveFeed + ": " + e.Message); }
                    UI(() => { ShowRows(); SetStatus(feedError == null ? ReadyText() : "Couldn't check for mod updates. " + ReadyText()); });
                });
            }
        }

        Release Latest
        {
            get
            {
                if (releases == null) return null;
                foreach (var r in releases) if (!r.Prerelease || Settings.Current.ShowBetas) if (r.ZipUrl != null) return r;
                return null;
            }
        }

        void ShowRows()
        {
            // Game
            if (game.Version == GameVersion.Missing)
            {
                rowGame.Set(Mark.Bad, game.Problem);
                rowGame.SetButtons(Btn("Browse…", true, Browse));
            }
            else
            {
                rowGame.Set(game.Version == GameVersion.Unknown ? Mark.Bad : Mark.Good,
                    ShortPath(game.Dir) + (game.Writable ? "" : "\nThis folder needs administrator rights to change."));
                rowGame.SetButtons(Btn("Change…", false, Browse), Btn("Open folder", false, () => Process.Start("explorer.exe", "\"" + game.Dir + "\"")));
            }

            // Patches
            var plan = game.Version == GameVersion.Missing || game.Version == GameVersion.Unknown ? null : Patcher.Plan(game);
            if (game.Version == GameVersion.Missing) { rowPatch.Set(Mark.None, "Waiting for the game folder."); rowPatch.SetButtons(); }
            else if (game.Version == GameVersion.Unknown) { rowPatch.Set(Mark.Bad, game.Problem); rowPatch.SetButtons(); }
            else if (plan.Count == 0) { rowPatch.Set(Mark.Good, Known.Describe(game.Version) + ": ready for the VR mod."); rowPatch.SetButtons(); }
            else
            {
                var names = new List<string>();
                foreach (var p in plan) names.Add(p.Kind == StepKind.RestoreFromCd ? "originals from the CD" : p.Kind == StepKind.Official120 ? "official v1.20" : "Fan Patch 1.42");
                rowPatch.Set(Mark.Warn, "Your game is " + Known.Describe(game.Version) + ". Needs: " + string.Join(", then ", names) + ".");
                rowPatch.SetButtons(NeedsAdmin() ? AdminBtn() : Btn("Install patches", true, InstallPatches));
            }

            // SteamVR
            if (!SteamVr.Installed)
            {
                rowVr.Set(Mark.Bad, "Not installed. The mod plays through SteamVR (free on Steam).");
                rowVr.SetButtons(Btn("Get SteamVR", true, () => Sys.OpenUrl("steam://install/250820")));
            }
            else
            {
                rowVr.Set(Mark.Good, SteamVr.Running ? "Installed and running." : "Installed. Launch in VR starts it for you.");
                rowVr.SetButtons(SteamVr.Running ? null : Btn("Start SteamVR", false, () => { try { SteamVr.Start(); } catch (Exception e) { Fail(e); } }));
            }

            // The mod
            InstallRecord rec = null;
            ModState ms = game.Version == GameVersion.Missing ? ModState.None : Mod.State(game.Dir, out rec);
            var latest = Latest;
            string online = latest != null ? "Latest: " + latest.Version + "." : feedError != null ? "Couldn't check for updates." : releases != null ? "No releases published yet." : "Checking for updates…";
            var menu = Btn("More ▾", false, null);
            menu.Click += (s, e) => ShowModMenu(menu, rec);
            openModMenu = () => ShowModMenu(menu, rec);
            bool gameOk = game.Ready && !NeedsAdmin();
            Control main = null;
            if (ms == ModState.Installed)
            {
                bool newer = latest != null && Ver.Compare(latest.Version, rec.version) > 0;
                rowMod.Set(newer ? Mark.Warn : Mark.Good, "Installed: " + rec.version + ". " + (newer ? "Update " + latest.Version + " available." : latest != null ? "Up to date." : online));
                if (newer && gameOk) main = Btn("Update to " + latest.Version, true, () => InstallRelease(latest));
            }
            else
            {
                string what = ms == ModState.DevBuild ? "A developer build is in place (not installed by this installer). " :
                              ms == ModState.OtherDdraw ? "Another ddraw.dll is in the game folder; it will be kept and put back on uninstall. " : "Not installed. ";
                rowMod.Set(ms == ModState.DevBuild ? Mark.Warn : Mark.None, what + online);
                if (latest != null && gameOk) main = Btn("Install " + latest.Version, true, () => InstallRelease(latest));
            }
            if (!game.Ready && game.Version != GameVersion.Missing) rowMod.Set(Mark.None, "Install the game patches first. " + online);
            if (NeedsAdmin() && game.Ready) main = AdminBtn();
            rowMod.SetButtons(menu, main);

            // Patch notes
            notes.Clear();
            if (releases != null && releases.Count > 0)
            {
                foreach (var r in releases)
                    if (!r.Prerelease || Settings.Current.ShowBetas)
                        Notes.Render(notes, r.Version, r.Title, r.Date, Notes.WithoutKnownIssues(r.Notes),
                                     rec != null && rec.version == r.Version);
                notes.SelectionStart = 0; notes.ScrollToCaret();
            }
            else Notes.Append(notes,
                feedError != null ? "Patch notes couldn't be loaded:\n" + feedError + "\n\nRelease feed: " + Settings.Current.EffectiveFeed :
                releases != null ? "No releases have been published yet. They will appear here, newest first." : "Loading…",
                Theme.Body, Theme.Dim);

            // Known issues: the newest release's own list, so it can change with each release.
            issues.Clear();
            Release newest = null;
            if (releases != null)
                foreach (var r in releases)
                    if (!r.Prerelease || Settings.Current.ShowBetas) { newest = r; break; }
            if (newest != null)
            {
                string list = Notes.KnownIssues(newest.Notes);
                Notes.Append(issues, list == null ? "No known issues are listed for version " + newest.Version + "."
                                                  : "Known issues in version " + newest.Version + ". They're being worked on.\n\n",
                             Theme.Small, Theme.Dim);
                if (list != null) Notes.RenderPage(issues, list);
                issues.SelectionStart = 0; issues.ScrollToCaret();
            }
            else Notes.Append(issues,
                feedError != null ? "The known issues couldn't be loaded:\n" + feedError :
                releases != null ? "No releases have been published yet." : "Loading…",
                Theme.Body, Theme.Dim);

            // Controls: the installed mod's own list (bwvr\controls.md), else the one built into the installer.
            string installed = game.Version == GameVersion.Missing ? null : Path.Combine(game.BwvrDir, "controls.md");
            string from = installed != null && File.Exists(installed) ? installed : "(built in)";
            if (from != controlsShownFrom || (from != "(built in)" && File.GetLastWriteTime(from) > controlsTime))
            {
                string md = null;
                try { md = from == "(built in)" ? BuiltInControls() : File.ReadAllText(from); } catch (Exception e) { Log.Write("controls not readable: " + e.Message); }
                controls.Clear();
                Notes.RenderControls(controls, md ?? BuiltInControls(),
                    from == "(built in)" ? (rec != null ? null : "The controls of the latest version. Once the mod is installed, this shows your version's.") : null);
                controls.SelectionStart = 0; controls.ScrollToCaret();
                controlsShownFrom = from;
                controlsTime = from == "(built in)" ? DateTime.MinValue : File.GetLastWriteTime(from);
            }

            // Installer update
            Release newerSetup = null;
            if (releases != null)
                foreach (var r in releases)
                    if (r.SetupVersion != null && Ver.Compare(r.SetupVersion, AppVersion) > 0
                        && (newerSetup == null || Ver.Compare(r.SetupVersion, newerSetup.SetupVersion) > 0)) newerSetup = r;
            updateBanner.Visible = newerSetup != null;
            if (newerSetup != null)
            {
                updateBanner.Text = "Update the installer to " + newerSetup.SetupVersion;
                updateBanner.Tag = newerSetup;
                updateBanner.Location = new Point(updateBanner.Parent.Width - updateBanner.Width - 24, 26);
            }

            launch.Enabled = !busy && game.Ready && (ms == ModState.Installed || ms == ModState.DevBuild) && SteamVr.Installed;
            foreach (var r in new[] { rowGame, rowPatch, rowVr, rowMod }) foreach (Control b in r.Buttons.Controls) b.Enabled = !busy;
        }

        // "C:\Users\…\Games\Black & White" when the full path is long.
        static string ShortPath(string p)
        {
            if (p == null || p.Length <= 58) return p;
            var parts = p.Split('\\', '/');
            if (parts.Length < 4) return p;
            return parts[0] + "\\…\\" + parts[parts.Length - 2] + "\\" + parts[parts.Length - 1];
        }

        void BannerClick(object s, EventArgs e)
        {
            var r = updateBanner.Tag as Release;
            Log.Write("installer update: clicked (" + (r == null ? "no release" : r.SetupVersion) + (busy ? ", busy" : "") + ")");
            if (r == null || busy) return;
            if (!SelfUpdate.CanUpdateInPlace(r))
            {
                // No hash, or a folder this account can't write to (Program Files, say): the release page.
                if (AskForm.Show(this, "Installer update", "Installer " + r.SetupVersion + " is available. It can't update itself in this folder, so the release page will open: download it there.", "Open the release page", "Not now") == "Open the release page")
                    Sys.OpenUrl(r.SetupUrl);
                return;
            }
            if (AskForm.Show(this, "Installer update", "Update the installer from " + AppVersion + " to " + r.SetupVersion + "?\n\n" +
                "It downloads the new version, checks it, replaces this one in the same place, and restarts. Your game, the mod and your settings aren't touched.",
                "Update and restart", "Not now") != "Update and restart") return;
            string args = Settings.GameDirOverride != null ? "--after-update --game \"" + Settings.GameDirOverride + "\"" : "--after-update";
            Work("updating the installer", pct => SelfUpdate.Apply(r, args, Log.Write, pct), () => { exiting = true; Close(); }, touchesGame: false);
        }

        bool NeedsAdmin() { return game.Version != GameVersion.Missing && !game.Writable && !Sys.IsAdmin(); }

        FlatButton AdminBtn()
        {
            return Btn("Restart as administrator", true, () =>
            {
                try
                {
                    Process.Start(new ProcessStartInfo(Application.ExecutablePath, "--game \"" + game.Dir + "\"") { Verb = "runas", UseShellExecute = true });
                    Close();
                }
                catch (Exception e) { Log.Write("not restarted: " + e.Message); }
            });
        }

        FlatButton Btn(string text, bool primary, Action a)
        {
            var b = new FlatButton(text, primary) { Margin = new Padding(6, 0, 0, 0), MinimumSize = new Size(0, 30) };
            if (a != null) b.Click += (s, e) => a();
            return b;
        }

        void Browse()
        {
            using (var d = new FolderBrowserDialog { Description = "Pick the Black & White folder (the one with runblack.exe in it).", ShowNewFolderButton = false })
            {
                if (game != null && game.Dir != null) d.SelectedPath = game.Dir;
                if (d.ShowDialog(this) != DialogResult.OK) return;
                var g = Game.Inspect(d.SelectedPath);
                if (g.Version == GameVersion.Missing) { AskForm.Show(this, "Black & White VR", "There's no runblack.exe in that folder.", "OK"); return; }
                Settings.Current.GameDir = d.SelectedPath; Settings.Current.Save();
                game = g;
                ShowRows();
            }
        }

        // Runs work off the window's thread, with the progress bar and the buttons locked.
        void Work(string what, Action<Action<int>> job, Action done = null, bool touchesGame = true)
        {
            if (busy) return;
            if (touchesGame && Sys.GameRunning()) { AskForm.Show(this, "Black & White VR", "Black & White is running. Close the game first, then try again.", "OK"); return; }
            busy = true; ShowRows();
            progress.Visible = true; progress.Value = 0; progress.Style = ProgressBarStyle.Marquee;
            Action<int> pct = p => UI(() => { progress.Style = ProgressBarStyle.Continuous; progress.Value = Math.Max(0, Math.Min(100, p)); });
            Log.Write("— " + what);
            Task.Run(() =>
            {
                Exception err = null;
                try { job(pct); } catch (Exception e) { err = e; }
                UI(() =>
                {
                    busy = false; progress.Visible = false;
                    Reload(false);
                    if (err != null) Fail(err); else if (done != null) done();
                });
            });
        }

        void Fail(Exception e)
        {
            Log.Write("error: " + e);
            SetStatus(e.Message);
            AskForm.Show(this, "Black & White VR", e.Message + "\n\nThe details are in the log (Show details).", "OK");
        }

        void InstallPatches()
        {
            var plan = Patcher.Plan(game);
            var lines = new List<string>();
            long dl = 0;
            foreach (var st in plan)
            {
                lines.Add("•  " + st.Title);
                if (st.Source != null && !Download.IsGood(st.Source, Path.Combine(AppPaths.Downloads, st.Source.FileName))) dl += st.Source.Size;
            }
            string msg = "Your game is " + Known.Describe(game.Version) + ". To get it to v1.20 + Fan Patch 1.42 the installer will:\n\n" + string.Join("\n", lines) +
                "\n\n" + (dl > 0 ? "The patches (" + (dl / 1048576.0).ToString("0.0") + " MB) are downloaded from bwgame.net, the community site that hosts them, and checked against their known fingerprints. " : "") +
                "Your saves and every file the patches change are backed up first, to \"BWVR Backups\" in the game folder.\n\n" +
                "The patches' own installers aren't used: they fail on Windows 11. The installer applies the same patch data itself.";
            using (var f = new AskForm("Install game patches", msg, new[] { dl > 0 ? "Download and install" : "Install", "I have the files…", "Cancel" }, null, "About the patches (bwgame.net)", Known.Fan142.PageUrl))
            {
                f.ShowDialog(this);
                if (f.Choice == "Cancel" || f.Choice == null) return;
                if (f.Choice == "I have the files…" && !PickPatchFiles(plan)) return;
            }
            string cd = null;
            if (plan.Exists(s => s.Kind == StepKind.RestoreFromCd))
            {
                cd = Patcher.FindCd();
                if (cd == null)
                {
                    if (AskForm.Show(this, "Black & White CD", "Your game has patch v1.10. Patch v1.20 only updates the original v1.00 files, so 12 of them are first put back from your Black & White CD.\n\nInsert the CD (or mount the disc image), then click Continue. You can also pick a folder with the CD's files.", "Continue", "Pick folder…", "Cancel") is string c && c != "Cancel")
                    {
                        cd = Patcher.FindCd();
                        if (cd == null || c == "Pick folder…")
                            using (var d = new FolderBrowserDialog { Description = "Pick the Black & White CD (the folder with data1.hdr in it)." })
                                if (d.ShowDialog(this) == DialogResult.OK && Patcher.LooksLikeCd(d.SelectedPath)) cd = d.SelectedPath;
                    }
                    if (cd == null) { SetStatus("The Black & White CD wasn't found; nothing was changed."); return; }
                }
            }
            var g = game;
            Work("installing game patches", pct =>
            {
                var files = new Dictionary<string, string>();
                foreach (var st in plan)
                {
                    if (st.Source == null) continue;
                    string file = Download.Patch(st.Source, Log.Write, pct);
                    if (file == null) throw new IOException(st.Source.Title + " couldn't be downloaded. Get it from " + st.Source.PageUrl + " and use \"I have the files…\".");
                    files[st.Source.Id] = file;
                }
                Patcher.Run(g, plan, files, cd, Log.Write, pct);
            }, () => AskForm.Show(this, "Black & White VR", "The game is patched: v1.20 + Fan Patch 1.42.", "OK"));
        }

        bool PickPatchFiles(List<PatchStep> plan)
        {
            foreach (var st in plan)
            {
                if (st.Source == null || Download.IsGood(st.Source, Path.Combine(AppPaths.Downloads, st.Source.FileName))) continue;
                using (var d = new OpenFileDialog { Title = "Where is " + st.Source.FileName + "? (" + st.Source.Title + ")", Filter = st.Source.FileName + "|" + st.Source.FileName + "|Programs|*.exe", FileName = st.Source.FileName })
                {
                    if (d.ShowDialog(this) != DialogResult.OK) return false;
                    if (!Download.Adopt(st.Source, d.FileName))
                    {
                        AskForm.Show(this, "Black & White VR", Path.GetFileName(d.FileName) + " isn't the expected " + st.Source.Title + " (its size or fingerprint differ).", "OK");
                        return false;
                    }
                }
            }
            return true;
        }

        void InstallRelease(Release r)
        {
            var g = game;
            Work("installing Black & White VR " + r.Version, pct =>
            {
                string zip = Releases.GetPackage(r, Log.Write, pct);
                Mod.Install(g.Dir, zip, Log.Write);
            });
        }

        void InstallFromFile()
        {
            using (var d = new OpenFileDialog { Title = "Pick a Black & White VR package", Filter = "Mod package|bwvr-mod-*.zip;*.zip" })
            {
                if (d.ShowDialog(this) != DialogResult.OK) return;
                var g = game; string zip = d.FileName;
                Work("installing from " + Path.GetFileName(zip), pct => Mod.Install(g.Dir, zip, Log.Write));
            }
        }

        void ShowModMenu(Control at, InstallRecord rec)
        {
            var m = new ContextMenuStrip { BackColor = Theme.Panel, ForeColor = Theme.Text, ShowImageMargin = false, Font = Theme.Body, Renderer = new ToolStripProfessionalRenderer(new DarkColors()) };
            bool ready = game.Ready && !NeedsAdmin();
            Add(m, "Install from a file…", ready, InstallFromFile);
            Add(m, "Repair (check every file)", rec != null, () => { var g = game; Work("repairing", p => Mod.Repair(g.Dir, Log.Write)); });
            if (releases != null && releases.Count > 0)
            {
                var pick = new ToolStripMenuItem("Install another version") { Enabled = ready, ForeColor = Theme.Text };
                foreach (var r in releases)
                {
                    var rr = r;
                    if (rr.ZipUrl == null) continue;
                    var it = new ToolStripMenuItem(rr.Version + (rr.Prerelease ? " (beta)" : "") + (rec != null && rec.version == rr.Version ? "  · installed" : "")) { ForeColor = Theme.Text, BackColor = Theme.Panel };
                    it.Click += (s, e) => InstallRelease(rr);
                    pick.DropDownItems.Add(it);
                }
                m.Items.Add(pick);
            }
            // The menu has no room for check marks: these two items say in words what they do.
            bool counting = Settings.Current.CountLaunches;
            var count = new ToolStripMenuItem(counting ? "Stop counting my launches" : "Count my launches (anonymous)…") { ForeColor = Theme.Text };
            count.Click += (s, e) =>
            {
                bool on = !Settings.Current.CountLaunches;
                if (on && AskForm.Show(this, "Count my launches", LaunchCount.Question, "Yes, count my launches", "No thanks") != "Yes, count my launches") return;
                Settings.Current.CountLaunches = on; Settings.Current.AskedCountLaunches = true; Settings.Current.Save();
                Log.Write("counting launches: " + (on ? "on" : "off"));
                SetStatus(on ? "Your launches will be counted. Thank you!" : "Your launches won't be counted any more.");
            };
            m.Items.Add(count);
            var beta = new ToolStripMenuItem(Settings.Current.ShowBetas ? "Hide beta versions" : "Show beta versions") { ForeColor = Theme.Text };
            beta.Click += (s, e) => { Settings.Current.ShowBetas = !Settings.Current.ShowBetas; Settings.Current.Save(); ShowRows(); };
            m.Items.Add(beta);
            Add(m, "Check for updates", true, () => Reload(true));
            m.Items.Add(new ToolStripSeparator());
            Add(m, "Uninstall…", rec != null, Uninstall);
            m.Show(at, new Point(0, at.Height));
        }

        void ShowFolderMenu(Control at)
        {
            var m = new ContextMenuStrip { BackColor = Theme.Panel, ForeColor = Theme.Text, ShowImageMargin = false, Font = Theme.Body, Renderer = new ToolStripProfessionalRenderer(new DarkColors()) };
            bool g = game != null && game.Dir != null;
            Add(m, "Open the game folder", g, () => Process.Start("explorer.exe", "\"" + game.Dir + "\""));
            Add(m, "Open the mod's settings (bwvr.ini)", g && File.Exists(Path.Combine(game.BwvrDir, "bwvr.ini")), () => Process.Start("notepad.exe", "\"" + Path.Combine(game.BwvrDir, "bwvr.ini") + "\""));
            Add(m, "Open the mod's log", g && File.Exists(Path.Combine(game.BwvrDir, @"logs\bwvr.log")), () => Process.Start("notepad.exe", "\"" + Path.Combine(game.BwvrDir, @"logs\bwvr.log") + "\""));
            Add(m, "Open the installer's log", File.Exists(Log.FilePath), () => Process.Start("notepad.exe", "\"" + Log.FilePath + "\""));
            Add(m, "Open the backups folder", g && Directory.Exists(BackupSet.Root(game.Dir)), () => Process.Start("explorer.exe", "\"" + BackupSet.Root(game.Dir) + "\""));
            if (g)
            {
                var sets = BackupSet.List(game.Dir);
                var restore = new ToolStripMenuItem("Restore a backup") { Enabled = sets.Count > 0 && !busy, ForeColor = Theme.Text };
                foreach (var b in sets)
                {
                    var bb = b;
                    var it = new ToolStripMenuItem(Path.GetFileName(bb.Dir) + "  (" + bb.Manifest.files.Count + " files)") { ForeColor = Theme.Text, BackColor = Theme.Panel };
                    it.Click += (s, e) =>
                    {
                        if (AskForm.Show(this, "Restore a backup", "Put back the " + bb.Manifest.files.Count + " game files saved in \"" + Path.GetFileName(bb.Dir) + "\"?\n\n" +
                            "Files the step added are removed again. Your saves aren't touched (their zip is in the same folder).", "Restore", "Cancel") != "Restore") return;
                        var gd = game.Dir;
                        Work("restoring a backup", p => bb.Restore(gd, Log.Write));
                    };
                    restore.DropDownItems.Add(it);
                }
                m.Items.Add(restore);
            }
            m.Show(at, new Point(0, -m.PreferredSize.Height));
        }

        static void Add(ContextMenuStrip m, string text, bool enabled, Action a)
        {
            var it = new ToolStripMenuItem(text) { Enabled = enabled, ForeColor = Theme.Text };
            it.Click += (s, e) => a();
            m.Items.Add(it);
        }

        void Uninstall()
        {
            using (var f = new AskForm("Uninstall Black & White VR",
                "This removes the VR mod's files from the game folder and puts back anything it replaced. The game itself stays patched (v1.20 + Fan Patch), and your saves aren't touched: games saved with the mod still load without it.",
                new[] { "Uninstall", "Cancel" }, "Also remove my VR settings, logs and captures (moved to the Recycle Bin)"))
            {
                f.ShowDialog(this);
                if (f.Choice != "Uninstall") return;
                bool all = f.Checked; var g = game;
                Work("uninstalling", p => Mod.Uninstall(g.Dir, all, Log.Write));
            }
        }

        // Once per player, new or updated: may the launches be counted? (Closing the question counts as no.)
        // Asked when the window opens (Program sets AskOnOpen; never in the developer commands' windows).
        internal bool AskOnOpen;
        Action openModMenu;
        internal void OpenModMenu() { if (openModMenu != null) openModMenu(); } // for the screenshot command

        void AskCountLaunches()
        {
            if (Settings.Current.AskedCountLaunches) return;
            bool yes = AskForm.Show(this, "Count my launches", LaunchCount.Question, "Yes, count my launches", "No thanks") == "Yes, count my launches";
            Settings.Current.CountLaunches = yes; Settings.Current.AskedCountLaunches = true; Settings.Current.Save();
            Log.Write("counting launches: " + (yes ? "on" : "off"));
        }

        void OnLaunch()
        {
            if (busy) return;
            var g = game;
            if (!Settings.Current.AskedCountLaunches) AskCountLaunches(); // in case it wasn't answered on opening
            InstallRecord rec = null;
            var state = Mod.State(g.Dir, out rec);
            LaunchCount.Send(state == ModState.DevBuild ? "dev" : rec != null ? rec.version : null);
            launchCancel = new CancellationTokenSource();
            var tok = launchCancel.Token;
            busy = true; ShowRows();
            Task.Run(() =>
            {
                Exception err = null;
                try { Launcher.Run(g, s => UI(() => SetStatus(s)), tok); } catch (Exception e) { err = e; }
                UI(() => { busy = false; ShowRows(); if (err != null) Fail(err); });
            });
        }

        sealed class DarkColors : ProfessionalColorTable
        {
            public override Color MenuItemSelected { get { return Theme.Line; } }
            public override Color MenuItemBorder { get { return Theme.Line; } }
            public override Color MenuBorder { get { return Theme.Line; } }
            public override Color ToolStripDropDownBackground { get { return Theme.Panel; } }
            public override Color ImageMarginGradientBegin { get { return Theme.Panel; } }
            public override Color ImageMarginGradientMiddle { get { return Theme.Panel; } }
            public override Color ImageMarginGradientEnd { get { return Theme.Panel; } }
            public override Color SeparatorDark { get { return Theme.Line; } }
            public override Color SeparatorLight { get { return Theme.Panel; } }
        }
    }
}
