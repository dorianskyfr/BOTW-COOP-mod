namespace BotwCoopLauncher;

static class Program
{
    /// <summary>
    /// Options (essais) : --data-dir &lt;dossier de donnees Ryujinx&gt; (au lieu de celui detecte),
    /// --mod-zip &lt;archive locale&gt; (au lieu de la derniere release GitHub), --work-dir &lt;dossier&gt;
    /// (au lieu de %LOCALAPPDATA%\BotwCoop). Sans fenetre : --install-now --ryujinx &lt;exe&gt;
    /// --base &lt;nsp&gt; --update &lt;nsp&gt; [--guest &lt;adresse&gt;] [--password &lt;mdp&gt;] ; journal dans
    /// &lt;work-dir&gt;\install.log, code de sortie 0 si tout s'est bien passe.
    /// </summary>
    [STAThread]
    static int Main(string[] args)
    {
        string dataDir = null, zip = null, ryujinx = null, baseNsp = null, update = null, guest = null, password = null;
        bool headless = false;
        for (int i = 0; i < args.Length; i++)
        {
            string Next() => i + 1 < args.Length ? args[++i] : "";
            switch (args[i])
            {
                case "--data-dir": dataDir = Next(); break;
                case "--mod-zip": zip = Next(); break;
                case "--work-dir": Settings.WorkDirOverride = Path.GetFullPath(Next()); break;
                case "--install-now": headless = true; break;
                case "--ryujinx": ryujinx = Next(); break;
                case "--base": baseNsp = Next(); break;
                case "--update": update = Next(); break;
                case "--guest": guest = Next(); break;
                case "--password": password = Next(); break;
            }
        }
        if (headless)
            return InstallHeadless(dataDir, zip, ryujinx, baseNsp, update, guest, password);
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm(dataDir, zip));
        return 0;
    }

    static int InstallHeadless(string dataDir, string zip, string ryujinx, string baseNsp, string update, string guest, string password)
    {
        Directory.CreateDirectory(Settings.WorkDir);
        var logPath = Path.Combine(Settings.WorkDir, "install.log");
        using var log = new StreamWriter(logPath, false) { AutoFlush = true };
        var s = Settings.Load();
        s.RyujinxExe = ryujinx ?? s.RyujinxExe;
        s.BaseNsp = baseNsp ?? s.BaseNsp;
        s.UpdateNsp = update ?? s.UpdateNsp;
        s.Host = guest == null;
        s.RelayAddress = guest ?? "";
        s.Password = password ?? "";
        try
        {
            var setup = new Setup(line => log.WriteLine(line), p => log.WriteLine($"[{p} %]"));
            setup.InstallAsync(new Setup.Options { Settings = s, DataDirOverride = dataDir, LocalModZip = zip }).GetAwaiter().GetResult();
            log.WriteLine("OK");
            return 0;
        }
        catch (Exception e)
        {
            log.WriteLine("ERREUR : " + e);
            return 1;
        }
    }
}
