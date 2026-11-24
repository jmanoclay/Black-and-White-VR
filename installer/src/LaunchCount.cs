using System;
using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace BwVrSetup
{
    // Counting launches, only for players who said yes (Settings.CountLaunches; asked once, on the first
    // Launch in VR, and changed under More). Each Launch in VR sends {id, version, setup} to the launch
    // counter (installer/stats/worker.js): a random id made here, the mod's version and the installer's.
    // Nothing else: no name, account, computer or game details. A ping that fails is only logged.
    public static class LaunchCount
    {
        public const string DefaultUrl = "https://bwvr-stats.jacobclayman.workers.dev/launch";

        public const string Question =
            "Can I count how many times the mod is played?\n\n" +
            "I'd love to know how many people are playing it, and that's all. If you say yes, each time you press " +
            "Launch in VR the installer sends just a random number made for this installer, and the versions of the mod and the installer.\n\n" +
            "No personal information is sent: no name, no account, nothing about your computer, and your internet " +
            "address isn't kept. It's never shared with anyone.\n\n" +
            "To stop it any time, press the More button on the \"Black & White VR mod\" line and choose " +
            "\"Stop counting my launches\".";

        static string Url { get { return Environment.GetEnvironmentVariable("BWVR_STATS_URL") ?? DefaultUrl; } }

        public static string Id
        {
            get
            {
                var s = Settings.Current;
                if (s.LaunchId == null || !Regex.IsMatch(s.LaunchId, "^[0-9a-f]{32}$"))
                {
                    s.LaunchId = Guid.NewGuid().ToString("N");
                    s.Save();
                }
                return s.LaunchId;
            }
        }

        // On Launch in VR: in the background, never holding up or failing the launch.
        public static void Send(string modVersion)
        {
            if (!Settings.Current.CountLaunches) return;
            string id = Id, setup = MainForm.AppVersion;
            Task.Run(() =>
            {
                try { Log.Write("launch counted (" + Post(Url, id, modVersion, setup) + ")"); }
                catch (Exception e) { Log.Write("launch not counted: " + e.Message); }
            });
        }

        public static int Post(string url, string id, string modVersion, string setup)
        {
            string v = Regex.IsMatch(modVersion ?? "", "^[0-9A-Za-z.-]{1,20}$") ? modVersion : "unknown";
            byte[] body = Encoding.UTF8.GetBytes("{\"id\":\"" + id + "\",\"version\":\"" + v + "\",\"setup\":\"" + setup + "\"}");
            var req = (HttpWebRequest)WebRequest.Create(url);
            req.Method = "POST"; req.ContentType = "application/json"; req.UserAgent = "bwvr-installer/" + setup;
            req.Timeout = 8000; req.ReadWriteTimeout = 8000; req.ContentLength = body.Length;
            using (var s = req.GetRequestStream()) s.Write(body, 0, body.Length);
            try { using (var r = (HttpWebResponse)req.GetResponse()) return (int)r.StatusCode; }
            catch (WebException e) when (e.Response is HttpWebResponse) { return (int)((HttpWebResponse)e.Response).StatusCode; }
        }
    }
}
