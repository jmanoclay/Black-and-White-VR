// Reads InstallShield 6 cabinets (dataN.hdr + dataN.cab), the format the retail CD and Lionhead's
// official patches use. Lets the installer take the files out without running InstallShield,
// whose patch scripts fail on Windows 11. Layout as documented by the unshield project.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

namespace BwVrSetup
{
    public sealed class IsCabFile
    {
        public int Index;
        public string Name;
        public string Directory;
        public string Group;
        public ushort Flags;
        public long ExpandedSize, CompressedSize, DataOffset;
        public int Volume;
        public bool Valid { get { return (Flags & 8) == 0 && DataOffset != 0; } }
        public string Path { get { return string.IsNullOrEmpty(Directory) ? Name : Directory + "\\" + Name; } }
    }

    public sealed class IsCab
    {
        readonly byte[] hdr;
        readonly string dir, prefix;
        readonly uint cabOff;
        public readonly int Major;
        public readonly List<IsCabFile> Files = new List<IsCabFile>();

        // headerPath: ...\data1.hdr; data volumes are data1.cab, data2.cab, ... next to it.
        public IsCab(string headerPath)
        {
            hdr = File.ReadAllBytes(headerPath);
            dir = System.IO.Path.GetDirectoryName(headerPath);
            string stem = System.IO.Path.GetFileNameWithoutExtension(headerPath); // "data1"
            prefix = stem.TrimEnd('0', '1', '2', '3', '4', '5', '6', '7', '8', '9');
            if (U32(0) != 0x28635349) throw new InvalidDataException("not an InstallShield cabinet header");
            uint version = U32(4);
            if (version >> 24 == 1) Major = (int)((version >> 12) & 0xf);
            else Major = (int)(version & 0xffff) / 100;
            if (Major < 6) throw new InvalidDataException("InstallShield version " + Major + " not supported");
            cabOff = U32(12);

            uint p = cabOff + 0xc;
            uint fileTableOffset = U32(p); p += 4;
            p += 4;
            p += 4; // file_table_size
            p += 4; // file_table_size2
            uint dirCount = U32(p); p += 4;
            p += 8;
            uint fileCount = U32(p); p += 4;
            uint fileTableOffset2 = U32(p); p += 4;
            p += 0xe;
            var groupOffsets = new uint[71];
            for (int i = 0; i < 71; i++) { groupOffsets[i] = U32(p); p += 4; }

            uint ft = cabOff + fileTableOffset;
            var dirs = new string[dirCount];
            for (int i = 0; i < dirCount; i++) dirs[i] = Str(ft + U32(ft + (uint)i * 4));

            for (int i = 0; i < fileCount; i++)
            {
                uint q = ft + fileTableOffset2 + (uint)i * 0x57;
                var f = new IsCabFile { Index = i };
                f.Flags = U16(q); q += 2;
                f.ExpandedSize = (long)U64(q); q += 8;
                f.CompressedSize = (long)U64(q); q += 8;
                f.DataOffset = (long)U64(q); q += 8;
                q += 0x20; // md5 + unknown
                uint nameOff = U32(q); q += 4;
                ushort dirIndex = U16(q); q += 2;
                q += 0xc;
                q += 4 + 4 + 1; // links
                f.Volume = U16(q);
                f.Name = Str(ft + nameOff);
                f.Directory = dirIndex < dirs.Length ? dirs[dirIndex] : "";
                Files.Add(f);
            }

            // File groups name ranges of files ("Program Files", "InfoScriptPatch2English", ...).
            foreach (uint start in groupOffsets)
            {
                uint o = start;
                while (o != 0)
                {
                    uint descr = U32(cabOff + o + 4);
                    uint next = U32(cabOff + o + 8);
                    uint g = cabOff + descr;
                    string name = Str(cabOff + U32(g));
                    g += 4 + 0x12;
                    uint first = U32(g), last = U32(g + 4);
                    for (uint k = first; k <= last && k < Files.Count; k++)
                        if (Files[(int)k].Group == null) Files[(int)k].Group = name;
                    o = next;
                }
            }
        }

        uint U32(uint p) { return BitConverter.ToUInt32(hdr, (int)p); }
        ushort U16(uint p) { return BitConverter.ToUInt16(hdr, (int)p); }
        ulong U64(uint p) { return BitConverter.ToUInt64(hdr, (int)p); }
        string Str(uint p)
        {
            int e = (int)p;
            while (e < hdr.Length && hdr[e] != 0) e++;
            return Encoding.Default.GetString(hdr, (int)p, e - (int)p);
        }

        public IsCabFile Find(string name, string group = null)
        {
            foreach (var f in Files)
                if (f.Valid && string.Equals(f.Name, name, StringComparison.OrdinalIgnoreCase)
                    && (group == null || string.Equals(f.Group, group, StringComparison.OrdinalIgnoreCase)))
                    return f;
            return null;
        }

        sealed class Volume
        {
            public uint FirstFile, LastFile;
            public ulong FirstOffset, FirstCompressed, LastOffset, LastCompressed;
            public FileStream S;
        }

        Volume OpenVolume(int n)
        {
            string path = System.IO.Path.Combine(dir, prefix + n + ".cab");
            var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            var b = new byte[20 + 56];
            if (s.Read(b, 0, b.Length) != b.Length) throw new InvalidDataException("short volume " + path);
            return new Volume
            {
                S = s,
                FirstFile = BitConverter.ToUInt32(b, 20),
                LastFile = BitConverter.ToUInt32(b, 24),
                FirstOffset = BitConverter.ToUInt64(b, 28),
                FirstCompressed = BitConverter.ToUInt64(b, 44),
                LastOffset = BitConverter.ToUInt64(b, 52),
                LastCompressed = BitConverter.ToUInt64(b, 68),
            };
        }

        // The file's stored bytes, following it into the next volume when it is split.
        byte[] ReadStored(IsCabFile f)
        {
            long need = (f.Flags & 4) != 0 ? f.CompressedSize : f.ExpandedSize;
            var outBuf = new byte[need];
            long got = 0;
            int vn = f.Volume == 0 ? 1 : f.Volume;
            var v = OpenVolume(vn);
            try
            {
                v.S.Seek(f.DataOffset, SeekOrigin.Begin);
                long avail = (f.Flags & 1) != 0 && f.Index == v.LastFile ? (long)v.LastCompressed : need;
                while (got < need)
                {
                    if (avail <= 0)
                    {
                        v.S.Dispose();
                        v = OpenVolume(++vn);
                        v.S.Seek((long)v.FirstOffset, SeekOrigin.Begin);
                        avail = f.Index == v.LastFile ? (long)v.LastCompressed : (long)v.FirstCompressed;
                    }
                    int r = v.S.Read(outBuf, (int)got, (int)Math.Min(avail, need - got));
                    if (r <= 0) throw new InvalidDataException("unexpected end of " + f.Name);
                    got += r; avail -= r;
                }
            }
            finally { v.S.Dispose(); }
            if ((f.Flags & 2) != 0) Deobfuscate(outBuf);
            return outBuf;
        }

        static void Deobfuscate(byte[] b)
        {
            for (int i = 0; i < b.Length; i++)
            {
                int t = b[i] ^ 0xd5;
                t = ((t >> 2) | (t << 6)) & 0xff;
                b[i] = (byte)(t - (i % 0x47));
            }
        }

        public void Extract(IsCabFile f, string target)
        {
            byte[] stored = ReadStored(f);
            System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(target)));
            using (var o = new FileStream(target, FileMode.Create, FileAccess.Write))
            {
                if ((f.Flags & 4) == 0) { o.Write(stored, 0, stored.Length); }
                else
                {
                    // A run of chunks: a 16-bit length, then raw deflate data.
                    int p = 0;
                    while (p + 2 <= stored.Length && o.Length < f.ExpandedSize)
                    {
                        int len = BitConverter.ToUInt16(stored, p); p += 2;
                        if (len == 0 || p + len > stored.Length) break;
                        using (var ms = new MemoryStream(stored, p, len))
                        using (var d = new DeflateStream(ms, CompressionMode.Decompress))
                            d.CopyTo(o);
                        p += len;
                    }
                }
                if (o.Length != f.ExpandedSize)
                    throw new InvalidDataException(f.Name + ": unpacked " + o.Length + " bytes, expected " + f.ExpandedSize);
            }
        }
    }
}
