using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

// The secure web proxy's root certificate in Windows Mobile's root store (docs/research-tls-proxy.md).
// The app makes a private root CA on first use and puts its certificate (DER) in the mailbox as
// pocketstylus-ca.cer; the proxy signs a certificate for each HTTPS site with it, so Windows Mobile
// has to trust it as a root.
//
// Commands (Helper.Execute):
//   tlsca install <SHA1>   add the mailbox's pocketstylus-ca.cer (thumbprint SHA1) to the root store
//                          unless it is there; drops earlier PocketStylus roots (an app reinstall makes
//                          a new CA, and the old one's key is gone)
//   tlsca remove [SHA1]    take the PocketStylus root(s) out again (the proxy was turned off)
//   tlsca status <SHA1>    whether the root store has it (crypt32 and the registry)
//
// Two ways in: the CertificateStore configuration service provider (DMProcessConfigXML, the
// supported way to provision roots on WM 5 / 6.x), then crypt32's CertAddEncodedCertificateToStore
// on the LOCAL_MACHINE "Root" system store if the CSP left it out. Both end in
// HKLM\Comm\Security\SystemCertificates\Root\Certificates\<SHA1>.
static class TlsCa
{
    const string FileName = "pocketstylus-ca.cer";
    const string Prefix = "PocketStylus Secure Web Proxy CA";
    const string RegRoot = @"Comm\Security\SystemCertificates\Root\Certificates";

    public static string Command(string rest)
    {
        try
        {
            int sp = rest.IndexOf(' ');
            string verb = sp > 0 ? rest.Substring(0, sp) : rest;
            string thumb = sp > 0 ? rest.Substring(sp + 1).Trim().ToUpper() : "";
            if (verb == "install" && thumb.Length == 40) return Install(thumb);
            if (verb == "remove") return Remove(thumb);
            if (verb == "status" && thumb.Length == 40) return "tlsca " + (Present(thumb) ? "present" : "absent") + Where(thumb);
            return "bad args";
        }
        catch (Exception e) { return "failed " + e.GetType().Name + " " + e.Message; }
    }

    static string Install(string thumb)
    {
        string stale = RemoveOthers(thumb);
        if (Present(thumb)) return "tlsca present" + Where(thumb) + stale;
        string path = Path.Combine(Helper.Box, FileName);
        if (!File.Exists(path)) return "tlsca no " + FileName;
        byte[] der;
        using (var f = File.OpenRead(path))  // CF has no File.ReadAllBytes
        {
            der = new byte[f.Length];
            int got = 0;
            while (got < der.Length) { int r = f.Read(der, got, der.Length - got); if (r <= 0) break; got += r; }
        }

        string how = "";
        IntPtr result;
        string xml = "<wap-provisioningdoc><characteristic type=\"CertificateStore\"><characteristic type=\"ROOT\">" +
                     "<characteristic type=\"" + thumb + "\"><parm name=\"EncodedCertificate\" value=\"" +
                     Convert.ToBase64String(der) + "\"/></characteristic></characteristic></characteristic></wap-provisioningdoc>";
        int hr = DMProcessConfigXML(xml, 1 /* CFGFLAG_PROCESS */, out result);
        if (result != IntPtr.Zero) LocalFree(result);
        if (Present(thumb)) how = "csp";
        else
        {
            IntPtr store = OpenRoot();
            if (store != IntPtr.Zero)
            {
                if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der, (uint)der.Length,
                        CERT_STORE_ADD_REPLACE_EXISTING, IntPtr.Zero))
                    how = "crypt32";
                else
                    how = "crypt32 error " + Marshal.GetLastWin32Error();
                CertCloseStore(store, 0);
            }
        }
        bool ok = Present(thumb);
        Helper.Log("tlsca: install " + thumb + " csp hr=0x" + hr.ToString("X8") + " -> " + (ok ? "present (" + how + ")" : "absent " + how));
        return "tlsca " + (ok ? "installed " + how : "failed csp=0x" + hr.ToString("X8") + " " + how) + Where(thumb) + stale;
    }

    static string Remove(string thumb)
    {
        var gone = new List<string>();
        foreach (string t in FindOurs())
            if (thumb.Length == 0 || t == thumb)
                if (Delete(t)) gone.Add(t);
        if (thumb.Length == 40 && !gone.Contains(thumb) && Present(thumb) && Delete(thumb)) gone.Add(thumb);
        if (gone.Count > 0) Helper.Log("tlsca: removed " + string.Join(" ", gone.ToArray()));
        return "tlsca removed " + gone.Count;
    }

    // Earlier PocketStylus roots (another CA from an earlier install): their keys are gone.
    static string RemoveOthers(string keep)
    {
        int n = 0;
        foreach (string t in FindOurs())
            if (t != keep && Delete(t)) n++;
        if (n > 0) Helper.Log("tlsca: removed " + n + " earlier root(s)");
        return n > 0 ? " (removed " + n + " earlier)" : "";
    }

    static bool Delete(string thumb)
    {
        IntPtr result;
        string xml = "<wap-provisioningdoc><characteristic type=\"CertificateStore\"><characteristic type=\"ROOT\">" +
                     "<nocharacteristic type=\"" + thumb + "\"/></characteristic></characteristic></wap-provisioningdoc>";
        DMProcessConfigXML(xml, 1, out result);
        if (result != IntPtr.Zero) LocalFree(result);
        if (!Present(thumb)) return true;
        IntPtr store = OpenRoot();
        if (store == IntPtr.Zero) return false;
        try
        {
            IntPtr ctx = FindByThumb(store, thumb);
            if (ctx != IntPtr.Zero) CertDeleteCertificateFromStore(ctx);  // frees ctx
        }
        finally { CertCloseStore(store, 0); }
        return !Present(thumb);
    }

    // Thumbprints of the PocketStylus roots in the store.
    static List<string> FindOurs()
    {
        var list = new List<string>();
        IntPtr store = OpenRoot();
        if (store == IntPtr.Zero) return list;
        try
        {
            IntPtr ctx = IntPtr.Zero;
            var name = new StringBuilder(256);
            while ((ctx = CertEnumCertificatesInStore(store, ctx)) != IntPtr.Zero)
            {
                name.Length = 0;
                CertGetNameString(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, IntPtr.Zero, name, 256);
                if (!name.ToString().StartsWith(Prefix)) continue;
                byte[] hash = new byte[20];
                uint cb = 20;
                if (CertGetCertificateContextProperty(ctx, CERT_SHA1_HASH_PROP_ID, hash, ref cb))
                    list.Add(Hex(hash));
            }
        }
        finally { CertCloseStore(store, 0); }
        return list;
    }

    static bool Present(string thumb)
    {
        IntPtr store = OpenRoot();
        if (store == IntPtr.Zero) return InRegistry(thumb);
        try
        {
            IntPtr ctx = FindByThumb(store, thumb);
            if (ctx == IntPtr.Zero) return false;
            CertFreeCertificateContext(ctx);
            return true;
        }
        finally { CertCloseStore(store, 0); }
    }

    static bool InRegistry(string thumb)
    {
        using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(RegRoot + "\\" + thumb)) return k != null;
    }

    static string Where(string thumb) { return InRegistry(thumb) ? " reg" : ""; }

    static IntPtr OpenRoot()
    {
        return CertOpenStore(new IntPtr(CERT_STORE_PROV_SYSTEM_W), 0, IntPtr.Zero, CERT_SYSTEM_STORE_LOCAL_MACHINE, "Root");
    }

    static IntPtr FindByThumb(IntPtr store, string thumb)
    {
        byte[] hash = new byte[20];
        for (int i = 0; i < 20; i++) hash[i] = Convert.ToByte(thumb.Substring(i * 2, 2), 16);
        IntPtr mem = Marshal.AllocHGlobal(20);
        try
        {
            Marshal.Copy(hash, 0, mem, 20);
            var blob = new CRYPT_HASH_BLOB { cbData = 20, pbData = mem };
            return CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, ref blob, IntPtr.Zero);
        }
        finally { Marshal.FreeHGlobal(mem); }
    }

    static string Hex(byte[] b)
    {
        var sb = new StringBuilder(b.Length * 2);
        foreach (byte x in b) sb.Append(x.ToString("X2"));
        return sb.ToString();
    }

    const uint X509_ASN_ENCODING = 1, PKCS_7_ASN_ENCODING = 0x10000;
    const int CERT_STORE_PROV_SYSTEM_W = 10;
    const uint CERT_SYSTEM_STORE_LOCAL_MACHINE = 0x00020000;
    const uint CERT_STORE_ADD_REPLACE_EXISTING = 3;
    const uint CERT_FIND_SHA1_HASH = 0x00010000;
    const uint CERT_NAME_SIMPLE_DISPLAY_TYPE = 4;
    const uint CERT_SHA1_HASH_PROP_ID = 3;

    struct CRYPT_HASH_BLOB { public uint cbData; public IntPtr pbData; }

    [DllImport("crypt32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CertOpenStore(IntPtr provider, uint encoding, IntPtr hCryptProv, uint flags, string para);
    [DllImport("crypt32.dll")] static extern bool CertCloseStore(IntPtr store, uint flags);
    [DllImport("crypt32.dll", SetLastError = true)]
    static extern bool CertAddEncodedCertificateToStore(IntPtr store, uint encoding, byte[] encoded, uint cb, uint disposition, IntPtr ppContext);
    [DllImport("crypt32.dll")]
    static extern IntPtr CertFindCertificateInStore(IntPtr store, uint encoding, uint findFlags, uint findType, ref CRYPT_HASH_BLOB para, IntPtr prev);
    [DllImport("crypt32.dll")] static extern IntPtr CertEnumCertificatesInStore(IntPtr store, IntPtr prev);
    [DllImport("crypt32.dll")] static extern bool CertDeleteCertificateFromStore(IntPtr ctx);
    [DllImport("crypt32.dll")] static extern bool CertFreeCertificateContext(IntPtr ctx);
    [DllImport("crypt32.dll")]
    static extern bool CertGetCertificateContextProperty(IntPtr ctx, uint propId, byte[] data, ref uint cb);
    [DllImport("crypt32.dll", CharSet = CharSet.Unicode)]
    static extern uint CertGetNameString(IntPtr ctx, uint type, uint flags, IntPtr typePara, StringBuilder name, uint cch);
    [DllImport("aygshell.dll", CharSet = CharSet.Unicode)]
    static extern int DMProcessConfigXML(string xml, int flags, out IntPtr result);
    [DllImport("coredll.dll")] static extern IntPtr LocalFree(IntPtr p);
}
