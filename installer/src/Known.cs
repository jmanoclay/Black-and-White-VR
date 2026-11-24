// What the installer knows about Black & White's versions and patches: fingerprints measured
// on the English retail CD and the patches below (2026-11-01, documentation/installer.md).
using System.Collections.Generic;

namespace BwVrSetup
{
    public enum GameVersion { Missing, Unknown, V100, V110, V120, V120Other, V120Fan }

    public sealed class PatchSource
    {
        public string Id, Title, FileName, Url, PageUrl, Sha256;
        public long Size;
    }

    public sealed class KnownFile
    {
        public string Path, Sha256; public long Offset, Size;
        public KnownFile(string path, long offset, long size, string sha) { Path = path; Offset = offset; Size = size; Sha256 = sha; }
    }

    public static class Known
    {
        // runblack.exe (SHA-256) at each step.
        public const string Exe100 = "b1163b44f4e6c5e64d81d5fcce95dea8efdc587ad52910df4d4942e01dbfb3d1";
        public const string Exe110 = "266dfe776974c142623a832967f104986746ba75bada91ddbe289f4137ea521b";
        public const string Exe120 = "8b950ab537e77b32c22affea8d2e3dc6e8e62155d9f68a6a82d2a1114c4775ca";
        public const string Exe120Fan = "3f58bc02a6c421b4e66ef08716646bf0e1fa55ab292786edb521d7596ed48aba";

        public static string Describe(GameVersion v)
        {
            switch (v)
            {
                case GameVersion.V100: return "v1.00 (retail, no patches)";
                case GameVersion.V110: return "v1.10";
                case GameVersion.V120: return "v1.20";
                case GameVersion.V120Other: return "v1.20 with an unrecognised runblack.exe";
                case GameVersion.V120Fan: return "v1.20 + Fan Patch 1.42";
                case GameVersion.Missing: return "not found";
                default: return "unrecognised version";
            }
        }

        // Downloads come from bwgame.net, the community site that hosts the patches; the installer
        // never ships them. Size and hash must match exactly.
        public static readonly PatchSource Patch12 = new PatchSource
        {
            Id = "patch120", Title = "Official patch v1.20 (Lionhead)", FileName = "black_white_patch_v1_20.exe",
            Url = "https://www.bwgame.net/downloads/v1-2-patch.130/download",
            PageUrl = "https://www.bwgame.net/downloads/v1-2-patch.130/",
            Size = 7816658, Sha256 = "8ced1a0f2605d173beacb36c5732a92af0355e7e632ba0468c2385cdda31fddf",
        };
        public static readonly PatchSource Fan142 = new PatchSource
        {
            Id = "fan142", Title = "Unofficial Fan Patch v1.42 (Matt Stevens, bwfiles.com)", FileName = "BWFanPatchInstaller.exe",
            Url = "https://www.bwgame.net/downloads/black-white-unofficial-patch-v1-42.1418/download",
            PageUrl = "https://www.bwgame.net/downloads/black-white-unofficial-patch-v1-42.1418/",
            Size = 4179067, Sha256 = "d10ec37527517e416197bc2fd9100f77a22bf92933bab554f3f6857d1ae1c6fb",
        };

        // Patch 1.2's own files, besides its RTPatch file: InstallShield file group -> game path.
        public const string Rtp12 = "BWPatch-V100-V102.rtp";
        public static readonly string[][] Patch12Copies =
        {
            new[] { "InfoScriptPatch2English", "InfoScriptPatch2.txt", @"Scripts\InfoScriptPatch2.txt" },
            new[] { "LHLogR", "LHLogR.dll", "LHLogR.dll" },
            new[] { "<Support>Language Independent OS Independent Files", "readme-patch-v120.txt", "readme-patch-v120.txt" },
        };

        // Fan Patch 1.42 (NSIS, deflate, not solid): each file's block offset in its data, size and hash.
        public const long FanDataStart = 57563;
        public static readonly KnownFile[] FanFiles =
        {
            new KnownFile("runblack.exe", 132503, 8663040, "3f58bc02a6c421b4e66ef08716646bf0e1fa55ab292786edb521d7596ed48aba"),
            new KnownFile("Setup.exe", 2772005, 111104, "c32adaf10814724730bf65b382bc64d15cd1750098f74859dbe96c3f437a4c7e"),
            new KnownFile("readme-patch-v142.txt", 2872644, 2101, "36d66f158e8c4f4dcce4b51d02eb6076d804ece476e17b77d3756e521b25a78d"),
            new KnownFile("LHLogR.dll", 2873585, 143360, "8cae8abc42b14732d00a979bd6cf3420b0fdc5b030ecfe40365ac5e6993a6afd"),
            new KnownFile("LHMultiplayerR.dll", 2940014, 446552, "7b6e099afa2b9f29b4d2a93d2b5810e4b88397d1545783e7a98f43be409ea1d2"),
            new KnownFile("d3dim.dll", 3150865, 6144, "0a06de746ed78d234ebb5f2c8daa3b562d01223cf4cbd750b547f4ba64ab6c5d"),
            new KnownFile("d3dim700.dll", 3152979, 6144, "4cbbf8b02acf5b2696be7e4ebb673b3074effdd9e8daca40830b8bce81dccefc"),
            new KnownFile(@"Data\ctrspec27.txt", 3155225, 2525, "cf9a0e3cf4eda10c05c995feaf4d4a24462b48f51e9c73aece1c255626b1b12d"),
            new KnownFile(@"Data\Textures\ATMOS.RAW", 3156204, 196608, "4e3be9924f6d20e4f679ea75f88545f7518bc447d0f3adbc128b76b1abdc44fb"),
            new KnownFile(@"Data\Textures\ATMOSA.RAW", 3200884, 65536, "2f44a28cb0c60274a7a4ad7d003ad387430b0e96af1175f36bb61f4c3e4e8cc2"),
            new KnownFile(@"Data\Textures\p4.raw", 3213393, 196608, "0cb6e1bcfc49cd449f4659b91868f91a793ccc1c7f9a57ee11a1f79afd0be971"),
            new KnownFile(@"Data\Textures\p4a.raw", 3321766, 65536, "9a6ffbc87cae52b6da1a6eb407f981f1d0cffd921ba70e13cd3df3d764d3bb0d"),
            new KnownFile(@"Data\Textures\p4t.raw", 3321964, 196608, "c1f7d702a80ae5e7dc52d62ef2577cbbb25c045d6dda3a49a872130518f48cc0"),
            new KnownFile(@"Data\Textures\p4ta.raw", 3322175, 65536, "bbc40a764c04e7bf88c4ea90ef5a788c2af4cca9e755dd454cb578374799aa3c"),
            new KnownFile(@"Plug Ins\ScriptLibraryR.dll", 3335633, 262144, "047b1d7cd967fb653f214a67a23a688e9f57558c102b80e5706d44e9221b79a1"),
            new KnownFile(@"Scripts\Land1.txt", 3455392, 168428, "e665683ae4b0d361f5a7eff1fe489d75a291cbdcaf7fb1742f6041e65e5c7955"),
            new KnownFile(@"Scripts\info.dat", 3493748, 582066, "e618f3127b6c53af1d2e845c4d9d66c28ead5c83bc4628075affa0e655115830"),
            new KnownFile(@"Scripts\Quests\challenge.chl", 3538631, 3234236, "0cc4cf1c0a84a3aefef3f091cb5bddbe4b036d4b451579a9e9099531ed7ff9b9"),
        };

        // Patch 1.2 only patches the retail 1.00 files. On a 1.10 game these 12 files (the ones 1.10
        // changed) are first put back from the CD: game path -> CD file group, name, retail hash.
        public static readonly string[][] CdRestoreFor110 =
        {
            new[] { @"Data\Textures\ATMOS.RAW", "Textures", "ATMOS.RAW", "bb5b8cb2759b939fb9d2014694cc9b90c5c5c4dce512974b010a3f01fb07b475" },
            new[] { @"Data\Textures\ATMOSA.RAW", "Textures", "ATMOSA.RAW", "52426fac5a22abaf21eea52d125b10c4f61c3d7540d9ae6815ce98e1ef2e342d" },
            new[] { @"Data\ctrspec27.txt", "Other Graphics", "ctrspec27.txt", "63789ff5c0683c0f781ae8c7e5ad1fd33da1881ed7f4c30aa9d23d93bce88ee8" },
            new[] { "LHMultiplayerR.dll", "Program Files", "LHMultiplayerR.dll", "9e335ff75f637be3e426fa2a4bfe1cf542fbf943ffc4ccf3f20d58cff2889c93" },
            new[] { @"Plug Ins\ScriptLibraryR.dll", "Plugins", "ScriptLibraryR.dll", "a3db26d1d58b37ec22bf6a7e6e2f12abd3707cca652731b326c7f1793ed3bd57" },
            new[] { @"Scripts\Land1.txt", "Data", "Land1.txt", "7c72ea0d46c809cfeff443a94f65bdf84f08e8fa16c1ab32304fa5368d3f5959" },
            new[] { @"Scripts\Quests\challenge.chl", "Data", "challenge.chl", "62b178c9c6c1edff28a62b7d8c534681a724db6e844186f9a586e95f223146e0" },
            new[] { @"Scripts\info.dat", "Data", "info.dat", "2f297bc54da304b235b4e7fffab6b62858af23efcdaa7e41d013ae7314f1f554" },
            new[] { "Setup.exe", "Setup Program", "Setup.exe", "b444722aeb6da21f6ab0dcb7937c5fcf087f12f162f87797b68de3f477070326" },
            new[] { "drvmgt.dll", "Program Files", "drvmgt.dll", "45444ba2e295e8620d0d17de954fe7d31c276cc1488e250b7dfb12fae7ea9898" },
            new[] { "runblack.exe", "Program Files", "runblack.exe", "b1163b44f4e6c5e64d81d5fcce95dea8efdc587ad52910df4d4942e01dbfb3d1" },
            new[] { "secdrv.sys", "Program Files", "secdrv.sys", "67d84f9bfafe3ea74819dae82afe5b9699dbe74c5b22edcec7fa56389ddc329b" },
        };

        // Files patch 1.2 changes, adds or removes (from its own log), for the backup taken first.
        public static readonly string[] Patch12Touches =
        {
            @"Data\Textures\ATMOS.RAW", @"Data\Textures\ATMOSA.RAW", @"Data\ctrspec27.txt", "LHMultiplayerR.dll",
            @"Plug Ins\ScriptLibraryR.dll", @"Scripts\Land1.txt", @"Scripts\Quests\challenge.chl", @"Scripts\info.dat",
            "Setup.exe", "drvmgt.dll", "runblack.exe", "secdrv.sys", @"Data\Textures\p4.raw", @"Data\Textures\p4a.raw",
            @"Data\Textures\p4t.raw", @"Data\Textures\p4ta.raw", "MemoryLeaks.txt", @"Online Maps\ConditionTemplate.txt",
            @"Scripts\InfoScriptMultiplayer2.txt", "inetlog.txt", @"Data\Landscape\dance.lnd", @"Plug Ins\LanguageD.dll",
            @"Plug Ins\ScriptLibraryD.dll", @"Scripts\InfoScriptPatch2.txt", "LHLogR.dll", "readme-patch-v120.txt",
        };

        public static IEnumerable<string> FanTouches()
        {
            foreach (var f in FanFiles) yield return f.Path;
        }
    }
}
