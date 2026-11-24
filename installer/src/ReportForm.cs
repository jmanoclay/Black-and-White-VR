// The Report window: bug or idea, a title, and exactly what the installer will fill in, before
// anything opens. Sending happens on GitHub, in the player's browser, with their account.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace BwVrSetup
{
    public sealed class ReportForm : Form
    {
        readonly RadioButton bug = new RadioButton(), idea = new RadioButton();
        readonly TextBox title = new TextBox(), preview = new TextBox();
        readonly CheckBox includeLog = new CheckBox();
        readonly string versions, log;

        public ReportForm(GameInfo g)
        {
            versions = Report.Versions(g);
            log = Report.LogExcerpt(g);
            Text = "Report a bug or an idea";
            BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.Dpi; AutoScaleDimensions = new SizeF(96, 96);
            ClientSize = new Size(620, 560);

            var col = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(18), AutoScroll = true };
            Func<string, Label> label = t => new Label { Text = t, AutoSize = true, MaximumSize = new Size(580, 0), UseMnemonic = false, Margin = new Padding(0, 0, 0, 6) };

            col.Controls.Add(label("This opens a short form on GitHub in your browser, with the versions and the end of the mod's log filled in. " +
                "You describe what happened there and press Submit. You need a free GitHub account, and reports are public: don't put personal details in them."));
            var kind = new FlowLayoutPanel { AutoSize = true, Margin = new Padding(0, 6, 0, 6) };
            bug.Text = "Bug report"; bug.Checked = true; bug.AutoSize = true; bug.ForeColor = Theme.Text;
            idea.Text = "Idea or feedback"; idea.AutoSize = true; idea.ForeColor = Theme.Text; idea.Margin = new Padding(18, 3, 3, 3);
            kind.Controls.Add(bug); kind.Controls.Add(idea);
            col.Controls.Add(kind);

            col.Controls.Add(label("Title (you can change it on GitHub)"));
            title.Width = 580; title.BackColor = Theme.Panel; title.ForeColor = Theme.Text; title.BorderStyle = BorderStyle.FixedSingle; title.MaxLength = 120;
            title.Margin = new Padding(0, 0, 0, 10);
            col.Controls.Add(title);

            includeLog.Text = "Include the end of the mod's log (helps find the cause)"; includeLog.Checked = true; includeLog.AutoSize = true; includeLog.ForeColor = Theme.Text;
            col.Controls.Add(includeLog);

            col.Controls.Add(label("What will be filled in (your Windows user name and PC name are taken out):"));
            preview.Multiline = true; preview.ReadOnly = true; preview.ScrollBars = ScrollBars.Vertical; preview.Size = new Size(580, 230);
            preview.BackColor = Theme.Panel; preview.ForeColor = Theme.Dim; preview.BorderStyle = BorderStyle.None; preview.Font = new Font("Consolas", 8.5f);
            col.Controls.Add(preview);

            var row = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Width = 580, Height = 40, Margin = new Padding(0, 12, 0, 0) };
            var open = new FlatButton("Open the form on GitHub", true) { Margin = new Padding(8, 0, 0, 0), MinimumSize = new Size(0, 30) };
            var cancel = new FlatButton("Cancel") { Margin = new Padding(8, 0, 0, 0), MinimumSize = new Size(0, 30) };
            row.Controls.Add(open); row.Controls.Add(cancel);
            string captures = g == null || g.Dir == null ? null : Path.Combine(g.BwvrDir, "captures");
            if (captures != null && Directory.Exists(captures) && Directory.GetFiles(captures, "feedback_*").Length > 0)
            {
                var snaps = new FlatButton("Open my snapshots") { Margin = new Padding(8, 0, 0, 0), MinimumSize = new Size(0, 30) };
                snaps.Click += (s, e) => Process.Start("explorer.exe", "\"" + captures + "\"");
                row.Controls.Add(snaps);
            }
            col.Controls.Add(row);
            Controls.Add(col);

            EventHandler update = (s, e) =>
            {
                includeLog.Enabled = bug.Checked;
                preview.Text = ("Versions and system:\r\n" + versions + "\r\n" +
                    (bug.Checked && includeLog.Checked ? "\r\nLog (last lines):\r\n" + log : "")).Replace("\n", "\r\n").Replace("\r\r\n", "\r\n");
            };
            bug.CheckedChanged += update; includeLog.CheckedChanged += update;
            update(null, EventArgs.Empty);

            cancel.Click += (s, e) => Close();
            open.Click += (s, e) =>
            {
                string url = Report.Url(bug.Checked, title.Text, versions, bug.Checked && includeLog.Checked ? log : null);
                Log.Write("report: opening the " + (bug.Checked ? "bug report" : "idea") + " form on github.com/" + Report.Repo + " (" + url.Length + " characters)");
                Sys.OpenUrl(url);
                Close();
            };
        }
    }
}
