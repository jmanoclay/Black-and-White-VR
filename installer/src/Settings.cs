using System;
using System.IO;

namespace BwVrSetup
{
    public sealed class Settings
    {
        // Where mod releases come from: "github:owner/repo", or "folder:<path>" (a folder with
        // releases.json, see installer/package.ps1).
        // "#<number>": the repo's id, which never changes; releases are read by it, so a renamed repo is still found.
        public const string DefaultFeed = "github:jmanoclay/Black-and-White-VR#1442480971";

        public string GameDir;
        public string Feed;
        public bool ShowBetas;
        public bool CountLaunches;       // the player said yes to counting launches (LaunchCount)
        public bool AskedCountLaunches;  // asked once, on the first Launch in VR
        public string LaunchId;          // the random id sent with each counted launch
        public int Tab; // the right-hand pane: 0 patch notes, 1 controls, 2 VR features, 3 known issues

        // A game folder from the command line (--game): used first, never saved.
        public static string GameDirOverride;

        static Settings current;
        static string FilePath { get { return Path.Combine(AppPaths.Data, "settings.json"); } }

        public static Settings Current
        {
            get
            {
                if (current == null)
                {
                    try { current = File.Exists(FilePath) ? Json.Read<Settings>(FilePath) : new Settings(); }
                    catch { current = new Settings(); }
                }
                return current;
            }
        }

        [System.Web.Script.Serialization.ScriptIgnore]
        public string EffectiveFeed
        {
            get
            {
                string env = Environment.GetEnvironmentVariable("BWVR_FEED");
                if (!string.IsNullOrEmpty(env)) return env;
                return string.IsNullOrEmpty(Feed) ? DefaultFeed : Feed;
            }
        }

        public void Save()
        {
            try { Json.Write(FilePath, this); } catch (Exception e) { Log.Write("settings not saved: " + e.Message); }
        }
    }
}
