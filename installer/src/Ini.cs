// bwvr.ini read and written the way the mod reads it (Windows' profile functions), so a change
// touches only its key and leaves the comments and every other line as they are.
using System.Runtime.InteropServices;
using System.Text;

namespace BwVrSetup
{
    public static class Ini
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileStringW(string section, string key, string def, StringBuilder ret, int size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool WritePrivateProfileStringW(string section, string key, string value, string file);

        // null when the key isn't there.
        public static string Get(string file, string section, string key)
        {
            var sb = new StringBuilder(512);
            GetPrivateProfileStringW(section, key, "\u0001", sb, sb.Capacity, file);
            string v = sb.ToString().Trim();
            return v == "\u0001" ? null : v;
        }

        public static void Set(string file, string section, string key, string value)
        {
            if (!WritePrivateProfileStringW(section, key, value, file))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "writing " + key + " in " + file);
        }
    }
}
