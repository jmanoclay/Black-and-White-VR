using System;
using System.Net;
using System.Reflection;
using System.Runtime.Versioning;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Jakeypoo's Black & White VR Mod Installer")] // the file description: what Task Manager shows
[assembly: AssemblyProduct("Jakeypoo's Black & White VR Mod")]
[assembly: AssemblyDescription("Installs the Black & White VR mod and launches the game in SteamVR")]
[assembly: AssemblyVersion("1.0.19.0")]
[assembly: AssemblyFileVersion("1.0.19.0")]
[assembly: AssemblyInformationalVersion("1.0.19")]
[assembly: TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]

namespace BwVrSetup
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12 | (SecurityProtocolType)12288; // TLS 1.3
            if (args.Length > 1 && args[0] == "--cli")
            {
                var rest = new string[args.Length - 1];
                Array.Copy(args, 1, rest, 0, rest.Length);
                try { return DevCli.Run(rest); }
                catch (Exception e) { Console.Error.WriteLine(e); return 1; }
            }
            bool afterUpdate = false;
            string oldExe = null;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--game" && i + 1 < args.Length) Settings.GameDirOverride = args[i + 1];
                if (args[i] == "--after-update") afterUpdate = true;
                if (args[i] == "--old" && i + 1 < args.Length) oldExe = args[i + 1];
            }
            bool first;
            using (var m = new Mutex(true, "BWVR-Setup-single-instance", out first))
            {
                if (!first && afterUpdate)
                {
                    // Started by the previous version's update: wait for it to close.
                    try { first = m.WaitOne(20000); } catch (AbandonedMutexException) { first = true; }
                }
                if (!first) { MessageBox.Show("The Black & White VR installer is already open.", "Black & White VR"); return 0; }
                // Started by an update: an update by 1.0.3/1.0.4 passes no --old and keeps its versioned name.
                if (afterUpdate && oldExe == null) oldExe = SelfUpdate.TakeOwnName();
                ThreadPool.QueueUserWorkItem(_ => SelfUpdate.CleanUp(oldExe));
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.ThreadException += (s, e) => { Log.Write("error: " + e.Exception); MessageBox.Show(e.Exception.Message, "Black & White VR"); };
                Log.Write("installer " + MainForm.AppVersion + " started");
                Application.Run(new MainForm { AskOnOpen = true });
            }
            return 0;
        }
    }
}
