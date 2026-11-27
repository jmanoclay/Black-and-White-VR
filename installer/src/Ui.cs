// Look and small controls for the window: a dark theme with the game's gold and the two gods'
// colours, flat buttons, status rows and a simple patch-notes renderer.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace BwVrSetup
{
    public static class Theme
    {
        public static readonly Color Back = Color.FromArgb(22, 21, 26);
        public static readonly Color Panel = Color.FromArgb(32, 31, 38);
        public static readonly Color PanelHi = Color.FromArgb(42, 41, 50);
        public static readonly Color Line = Color.FromArgb(56, 54, 66);
        public static readonly Color Text = Color.FromArgb(232, 228, 220);
        public static readonly Color Dim = Color.FromArgb(150, 146, 160);
        public static readonly Color Gold = Color.FromArgb(212, 175, 92);
        public static readonly Color Good = Color.FromArgb(108, 196, 128);
        public static readonly Color Warn = Color.FromArgb(230, 176, 70);
        public static readonly Color Bad = Color.FromArgb(222, 92, 84);
        public static readonly Color Accent = Color.FromArgb(78, 136, 214);
        public static readonly Color AccentHi = Color.FromArgb(98, 156, 234);

        public static readonly Font Body = new Font("Segoe UI", 9.75f);
        public static readonly Font BodyBold = new Font("Segoe UI Semibold", 9.75f);
        public static readonly Font Small = new Font("Segoe UI", 8.75f);
        public static readonly Font Title = new Font("Georgia", 22f, FontStyle.Bold);
        public static readonly Font Heading = new Font("Segoe UI Semibold", 11.5f);
        public static readonly Font Big = new Font("Segoe UI Semibold", 13f);
    }

    public sealed class FlatButton : Button
    {
        public bool Primary;
        public FlatButton(string text, bool primary = false)
        {
            Text = text; Primary = primary;
            AccessibleRole = AccessibleRole.PushButton; // flat buttons otherwise read as panes to screen readers
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = primary ? 0 : 1;
            FlatAppearance.BorderColor = Theme.Line;
            BackColor = primary ? Theme.Accent : Theme.PanelHi;
            FlatAppearance.MouseOverBackColor = primary ? Theme.AccentHi : Theme.Line;
            ForeColor = Theme.Text;
            Font = primary ? Theme.BodyBold : Theme.Body;
            Cursor = Cursors.Hand;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Padding = new Padding(10, 3, 10, 3);
            UseVisualStyleBackColor = false;
        }
        protected override void OnEnabledChanged(EventArgs e)
        {
            base.OnEnabledChanged(e);
            ForeColor = Enabled ? Theme.Text : Theme.Dim;
            BackColor = Enabled ? (Primary ? Theme.Accent : Theme.PanelHi) : Theme.Panel;
        }
    }

    public enum Mark { None, Good, Warn, Bad, Busy }

    // One line of the checklist: a coloured mark, a title, a detail line and its buttons.
    public sealed class StatusRow : Panel
    {
        readonly Label title = new Label(), detail = new Label();
        public readonly FlowLayoutPanel Buttons = new FlowLayoutPanel();
        Mark mark;

        public StatusRow(string heading)
        {
            // Repaint all of it on a resize: the border is drawn at the edge, and the old one stayed
            // behind as stripes while the window was dragged wider.
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);
            BackColor = Theme.Panel;
            Padding = new Padding(44, 8, 12, 8);
            Height = 78;
            Dock = DockStyle.Top;
            var grid = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1, BackColor = Color.Transparent, Margin = new Padding(0) };
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            grid.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            var text = new Panel { Dock = DockStyle.Fill, BackColor = Color.Transparent, Margin = new Padding(0) };
            title.Text = heading; title.Font = Theme.Heading; title.ForeColor = Theme.Text; title.AutoSize = true; title.UseMnemonic = false;
            title.Location = new Point(0, 2);
            detail.Font = Theme.Body; detail.ForeColor = Theme.Dim; detail.UseMnemonic = false; detail.AutoEllipsis = true;
            detail.Location = new Point(1, 27);
            text.Controls.Add(title); text.Controls.Add(detail);
            text.Resize += (s, e) => detail.Size = new Size(Math.Max(40, text.Width - 4), Math.Max(20, text.Height - 27));
            Buttons.FlowDirection = FlowDirection.RightToLeft;
            Buttons.WrapContents = false;
            Buttons.AutoSize = true;
            Buttons.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Buttons.BackColor = Color.Transparent;
            Buttons.Anchor = AnchorStyles.Right; // vertically centred in its cell
            Buttons.Margin = new Padding(8, 0, 0, 0);
            grid.Controls.Add(text, 0, 0);
            grid.Controls.Add(Buttons, 1, 0);
            Controls.Add(grid);
        }

        public void Set(Mark m, string text)
        {
            mark = m; detail.Text = text; Invalidate();
        }

        public void SetButtons(params Control[] bs)
        {
            Buttons.SuspendLayout();
            Buttons.Controls.Clear();
            foreach (var b in bs) if (b != null) Buttons.Controls.Add(b);
            Buttons.ResumeLayout();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            Color c = mark == Mark.Good ? Theme.Good : mark == Mark.Warn ? Theme.Warn : mark == Mark.Bad ? Theme.Bad : Theme.Dim;
            var r = new Rectangle(14, 12, 18, 18);
            using (var b = new SolidBrush(c)) g.FillEllipse(b, r);
            using (var p = new Pen(Theme.Panel, 2.2f))
            {
                if (mark == Mark.Good) g.DrawLines(p, new[] { new Point(18, 21), new Point(22, 25), new Point(28, 17) });
                else if (mark == Mark.Bad) { g.DrawLine(p, 19, 17, 27, 25); g.DrawLine(p, 27, 17, 19, 25); }
                else if (mark == Mark.Warn) { g.DrawLine(p, 23, 15, 23, 22); g.DrawLine(p, 23, 25, 23, 26); }
            }
            using (var p = new Pen(Theme.Line)) g.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }
    }

    // Tabs as headings: the selected one in full colour with a gold underline.
    public sealed class TabStrip : FlowLayoutPanel
    {
        readonly System.Collections.Generic.List<Label> items = new System.Collections.Generic.List<Label>();
        int selected;

        public TabStrip()
        {
            Dock = DockStyle.Top; Height = 34; BackColor = Theme.Back; WrapContents = false;
            Padding = new Padding(0); Margin = new Padding(0);
        }

        public void Add(string text, Action onClick)
        {
            var l = new Label
            {
                Text = text, Font = Theme.Heading, AutoSize = true, UseMnemonic = false, Cursor = Cursors.Hand,
                Padding = new Padding(0, 4, 0, 8), Margin = new Padding(0, 0, 14, 0), ForeColor = Theme.Dim,
                AccessibleRole = AccessibleRole.PageTab,
            };
            l.Click += (s, e) => onClick();
            l.Paint += (s, e) =>
            {
                if (items.IndexOf(l) != selected) return;
                using (var b = new SolidBrush(Theme.Gold)) e.Graphics.FillRectangle(b, 0, l.Height - 3, l.Width, 3);
            };
            items.Add(l);
            Controls.Add(l);
            Fit();
        }

        protected override void OnSizeChanged(EventArgs e) { base.OnSizeChanged(e); Fit(); }

        // All tabs on one line at any width and text size: the gaps narrow first, then the font
        // gets smaller (with large text scaling the fourth tab was cut off).
        void Fit()
        {
            if (items.Count == 0 || Width <= 0) return;
            foreach (var font in new[] { Theme.Heading, Theme.BodyBold, Theme.Small })
                for (int gap = 22; gap >= 6; gap -= 4)
                {
                    int total = 0;
                    foreach (var l in items) total += TextRenderer.MeasureText(l.Text, font).Width + l.Padding.Horizontal;
                    total += gap * (items.Count - 1) + 4;
                    if (total <= Width || (font == Theme.Small && gap <= 6)) { Apply(font, gap); return; }
                }
        }

        void Apply(Font font, int gap)
        {
            SuspendLayout();
            foreach (var l in items)
            {
                if (l.Font != font) l.Font = font;
                var m = new Padding(0, 0, gap, 0);
                if (l.Margin != m) l.Margin = m;
            }
            ResumeLayout();
        }

        public int Selected
        {
            get { return selected; }
            set
            {
                selected = value;
                for (int i = 0; i < items.Count; i++) { items[i].ForeColor = i == value ? Theme.Text : Theme.Dim; items[i].Invalidate(); }
            }
        }
    }

    // A RichTextBox on Windows' newer rich edit control (msftedit.dll, RICHEDIT50W, in every Windows
    // since XP SP1), which shows JPEG and PNG pictures in its text; WinForms' own uses the older one,
    // which shows them as blank space. Falls back to the older one if msftedit can't be loaded.
    public sealed class PictureTextBox : RichTextBox
    {
        [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
        static extern IntPtr LoadLibrary(string name);
        static readonly bool Msftedit = LoadLibrary("msftedit.dll") != IntPtr.Zero;

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                if (Msftedit) cp.ClassName = "RICHEDIT50W";
                return cp;
            }
        }
    }

    public static class Notes
    {
        // The controls list (installer/controls.md): sections, and tables of control and what it does,
        // each shown as the control in bold with what it does indented below.
        public static void RenderControls(RichTextBox box, string md, string note)
        {
            if (note != null) Append(box, note + "\n\n", Theme.Small, Theme.Dim);
            bool first = true;
            foreach (var raw in (md ?? "").Replace("\r", "").Split('\n'))
            {
                string line = raw.Trim();
                if (line.Length == 0) continue;
                if (line.StartsWith("# ")) continue; // the tab is the title
                if (line.StartsWith("## "))
                {
                    Append(box, (first ? "" : "\n") + line.Substring(3) + "\n", Theme.Heading, Theme.Gold);
                    first = false;
                    continue;
                }
                if (line.StartsWith("|"))
                {
                    var cells = line.Trim('|').Split('|');
                    if (cells.Length < 2) continue;
                    string what = cells[0].Trim(), does = cells[1].Trim();
                    if (what == "Control" || what.StartsWith("---")) continue; // header row, separator
                    box.SelectionStart = box.TextLength;
                    box.SelectionIndent = 0;
                    Append(box, what.Replace("**", "") + "\n", Theme.BodyBold, Theme.Text);
                    box.SelectionStart = box.TextLength;
                    box.SelectionIndent = 16;
                    Append(box, does.Replace("**", "") + "\n", Theme.Body, Theme.Dim);
                    box.SelectionStart = box.TextLength;
                    box.SelectionIndent = 0;
                    Append(box, "\n", Theme.Small, Theme.Dim);
                    continue;
                }
                box.SelectionStart = box.TextLength;
                box.SelectionIndent = 0;
                Append(box, line.Replace("**", "") + "\n\n", Theme.Body, Theme.Text);
            }
        }

        // A release's text may end its patch notes with a "## Known issues" section (any heading level).
        // KnownIssues gives that section's body (null if there is none); WithoutKnownIssues the text
        // without it, for the Patch notes tab. The section runs to the next heading of the same or a
        // higher level.
        public static string KnownIssues(string md) { string body, rest; Split(md, out body, out rest); return body; }
        public static string WithoutKnownIssues(string md) { string body, rest; Split(md, out body, out rest); return rest; }

        static void Split(string md, out string body, out string rest)
        {
            body = null; rest = md;
            if (string.IsNullOrEmpty(md)) return;
            var lines = md.Replace("\r", "").Split('\n');
            int start = -1, level = 0, end = lines.Length;
            for (int i = 0; i < lines.Length; i++)
            {
                string t = lines[i].Trim();
                int h = 0; while (h < t.Length && t[h] == '#') h++;
                if (h == 0 || h == t.Length) continue;
                if (start < 0)
                {
                    if (string.Equals(t.Substring(h).Trim(), "Known issues", StringComparison.OrdinalIgnoreCase)) { start = i; level = h; }
                }
                else if (h <= level) { end = i; break; }
            }
            if (start < 0) return;
            body = string.Join("\n", lines, start + 1, end - start - 1).Trim();
            var kept = new System.Collections.Generic.List<string>();
            for (int i = 0; i < lines.Length; i++) if (i < start || i >= end) kept.Add(lines[i]);
            rest = string.Join("\n", kept).TrimEnd();
            if (body.Length == 0) body = null;
        }

        // A page of text (installer/features.md): sections in gold, paragraphs and bullets with **bold**,
        // and pictures: a line "![what it shows](name.jpg)" shows res\features\name.jpg (built in as
        // BwVrSetup.img.name.jpg) across the box's width.
        public static void RenderPage(RichTextBox box, string md)
        {
            bool first = true, afterHeading = false;
            foreach (var raw in (md ?? "").Replace("\r", "").Split('\n'))
            {
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("# ")) continue; // the tab is the title
                bool wasHeading = afterHeading; afterHeading = line.StartsWith("## ");
                if (line.StartsWith("![") && line.EndsWith(")") && line.Contains("]("))
                {
                    string name = line.Substring(line.IndexOf("](") + 2).TrimEnd(')');
                    box.SelectionStart = box.TextLength;
                    box.SelectionIndent = 0; box.SelectionHangingIndent = 0;
                    if (!wasHeading) Append(box, "\n", Theme.Small, Theme.Dim); // a gap after the text above
                    Picture(box, "BwVrSetup.img." + name);
                    continue;
                }
                if (line.StartsWith("## "))
                {
                    Append(box, (first ? "" : "\n") + line.Substring(3) + "\n", Theme.Heading, Theme.Gold);
                    first = false;
                    continue;
                }
                bool bullet = line.StartsWith("- ");
                box.SelectionStart = box.TextLength;
                box.SelectionIndent = bullet ? 4 : 0;
                box.SelectionHangingIndent = bullet ? 14 : 0; // wrapped lines start under the text, not the bullet
                if (bullet) { Append(box, "•\t", Theme.Body, Theme.Dim); line = line.Substring(2); }
                var parts = line.Split(new[] { "**" }, StringSplitOptions.None);
                for (int i = 0; i < parts.Length; i++)
                    Append(box, parts[i], i % 2 == 1 ? Theme.BodyBold : Theme.Body, Theme.Text);
                Append(box, "\n", Theme.Body, Theme.Text);
                first = false;
            }
        }

        // Patch notes are Markdown; this shows headings, bullets and **bold** plainly.
        public static void Render(RichTextBox box, string version, string title, string date, string md, bool installed)
        {
            Append(box, title == null || title == version ? "Version " + version : title, Theme.Heading, Theme.Gold);
            Append(box, "  " + (date ?? "") + (installed ? "   · installed" : "") + "\n", Theme.Small, installed ? Theme.Good : Theme.Dim);
            foreach (var raw in (md ?? "").Replace("\r", "").Split('\n'))
            {
                string line = raw.TrimEnd();
                if (line.StartsWith("#")) { Append(box, line.TrimStart('#').Trim() + "\n", Theme.BodyBold, Theme.Text); continue; }
                string indent = "";
                string t = line.TrimStart();
                if (t.StartsWith("- ") || t.StartsWith("* ")) { indent = new string(' ', (line.Length - t.Length)) + "  •  "; t = t.Substring(2); }
                Append(box, indent, Theme.Body, Theme.Dim);
                var parts = t.Split(new[] { "**" }, StringSplitOptions.None);
                for (int i = 0; i < parts.Length; i++)
                    Append(box, parts[i].Replace("`", ""), i % 2 == 1 ? Theme.BodyBold : Theme.Body, Theme.Text);
                Append(box, "\n", Theme.Body, Theme.Text);
            }
            Append(box, "\n", Theme.Body, Theme.Text);
        }

        // A built-in picture at the end of the box, as wide as the box (less a margin), with a gap below.
        // Put in as RTF: the picture's own bytes (\jpegblip or \pngblip) and its size on the page.
        static void Picture(RichTextBox box, string resource)
        {
            byte[] bytes;
            using (var s = typeof(Notes).Assembly.GetManifestResourceStream(resource))
            {
                if (s == null) return; // a picture not built in: nothing
                using (var ms = new System.IO.MemoryStream()) { s.CopyTo(ms); bytes = ms.ToArray(); }
            }
            int w, h;
            using (var img = Image.FromStream(new System.IO.MemoryStream(bytes))) { w = img.Width; h = img.Height; }
            float dpi; using (var g = box.CreateGraphics()) dpi = g.DpiX;
            int shownPx = Math.Max(160, Math.Min(box.ClientSize.Width - 28, (int)(560 * dpi / 96)));
            int twipW = shownPx * 1440 / (int)dpi, twipH = twipW * h / w;
            string blip = resource.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ? "\\pngblip" : "\\jpegblip";
            var rtf = new System.Text.StringBuilder(bytes.Length * 2 + 200);
            rtf.Append("{\\rtf1{\\pict").Append(blip).Append("\\picw").Append(w).Append("\\pich").Append(h)
               .Append("\\picwgoal").Append(twipW).Append("\\pichgoal").Append(twipH).Append(' ');
            foreach (byte b in bytes) rtf.Append(b.ToString("x2"));
            rtf.Append("}\\par}");
            box.SelectionStart = box.TextLength; box.SelectionLength = 0;
            bool ro = box.ReadOnly; box.ReadOnly = false; // a read-only box refuses pictures put in
            box.SelectedRtf = rtf.ToString();
            box.ReadOnly = ro;
        }

        public static void Append(RichTextBox box, string text, Font f, Color c)
        {
            box.SelectionStart = box.TextLength;
            box.SelectionLength = 0;
            box.SelectionFont = f;
            box.SelectionColor = c;
            box.AppendText(text);
        }
    }

    // A small themed question box: a message, buttons, and an optional check box.
    public sealed class AskForm : Form
    {
        public string Choice;
        public bool Checked { get { return check != null && check.Checked; } }
        readonly CheckBox check;

        public AskForm(string caption, string message, string[] buttons, string checkText = null, string link = null, string linkUrl = null)
        {
            Text = caption;
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.Dpi; AutoScaleDimensions = new SizeF(96, 96);
            var layout = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, Padding = new Padding(18), WrapContents = false };
            var msg = new Label { Text = message, UseMnemonic = false, AutoSize = true, MaximumSize = new Size(520, 0), ForeColor = Theme.Text, Margin = new Padding(0, 0, 0, 12) };
            layout.Controls.Add(msg);
            if (link != null)
            {
                var l = new LinkLabel { Text = link, AutoSize = true, LinkColor = Theme.AccentHi, ActiveLinkColor = Theme.Gold, Margin = new Padding(0, 0, 0, 12) };
                l.LinkClicked += (s, e) => Sys.OpenUrl(linkUrl);
                layout.Controls.Add(l);
            }
            if (checkText != null)
            {
                check = new CheckBox { Text = checkText, AutoSize = true, ForeColor = Theme.Text, Margin = new Padding(0, 0, 0, 12) };
                layout.Controls.Add(check);
            }
            var row = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, AutoSize = true, Anchor = AnchorStyles.Right, Margin = new Padding(0) };
            for (int i = 0; i < buttons.Length; i++)
            {
                string b = buttons[i];
                var btn = new FlatButton(b, i == 0) { Margin = new Padding(8, 0, 0, 0), MinimumSize = new Size(90, 30) };
                btn.Click += (s, e) => { Choice = b; DialogResult = DialogResult.OK; Close(); };
                row.Controls.Add(btn);
            }
            layout.Controls.Add(row);
            Controls.Add(layout);
            AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
            Shown += (s, e) => row.Width = Math.Max(row.Width, msg.Width);
        }

        public static string Show(IWin32Window owner, string caption, string message, params string[] buttons)
        {
            using (var f = new AskForm(caption, message, buttons)) { f.ShowDialog(owner); return f.Choice; }
        }
    }
}
