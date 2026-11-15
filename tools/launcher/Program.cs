namespace BotwCoopLauncher;

static class Program
{
    /// <summary>
    /// Options (essais) : --data-dir &lt;dossier de donnees Ryujinx&gt; (au lieu de celui detecte),
    /// --mod-zip &lt;archive locale&gt; (au lieu de la derniere release GitHub), --work-dir &lt;dossier&gt;
    /// (au lieu de %LOCALAPPDATA%\BotwCoop). Sans fenetre : --install-now --ryujinx &lt;exe&gt;
    /// --base &lt;nsp&gt; --update &lt;nsp&gt; [--guest &lt;adresse&gt;] [--password &lt;mdp&gt;] ; journal dans
    /// &lt;work-dir&gt;\install.log, code de sortie 0 si tout s'est bien passe. --update-if-needed :
    /// meme verification que le bouton « Jouer » (installe la derniere version si elle differe), sans lancer le jeu.
    /// --screenshot &lt;png&gt; [--page main|settings|log] : essai, enregistre la fenetre et ferme.
    /// --relay-test &lt;s&gt; : essai, lance le relais comme « Jouer » (hote) pendant s secondes.
    /// </summary>
    [STAThread]
    static int Main(string[] args)
    {
        string dataDir = null, zip = null, ryujinx = null, baseNsp = null, update = null, guest = null, password = null;
        bool headless = false, updateOnly = false;
        string screenshot = null, page = "main";
        int relayTest = 0, duoTest = 0;
        for (int i = 0; i < args.Length; i++)
        {
            string Next() => i + 1 < args.Length ? args[++i] : "";
            switch (args[i])
            {
                case "--data-dir": dataDir = Next(); break;
                case "--mod-zip": zip = Next(); break;
                case "--work-dir": Settings.WorkDirOverride = Path.GetFullPath(Next()); break;
                case "--install-now": headless = true; break;
                case "--update-if-needed": headless = true; updateOnly = true; break;
                case "--screenshot": screenshot = Next(); break;
                case "--page": page = Next(); break;
                case "--relay-test": relayTest = int.Parse(Next()); break;
                case "--duo-test": duoTest = int.Parse(Next()); break;
                case "--ryujinx": ryujinx = Next(); break;
                case "--base": baseNsp = Next(); break;
                case "--update": update = Next(); break;
                case "--guest": guest = Next(); break;
                case "--password": password = Next(); break;
            }
        }
        if (relayTest > 0)
            return RelayTest(relayTest);
        if (duoTest > 0)
            return DuoTest(duoTest, dataDir);
        if (headless)
            return InstallHeadless(dataDir, zip, ryujinx, baseNsp, update, guest, password, updateOnly);
        ApplicationConfiguration.Initialize();
        var form = new MainForm(dataDir, zip);
        if (screenshot != null)
            // Essai : capture de la fenetre apres chargement (icone, captures), puis fermeture.
            form.Shown += async (_, _) => await form.SaveScreenshotAsync(screenshot, 6000, page);
        Application.Run(form);
        return 0;
    }

    /// <summary>Essai : relais lance comme par « Jouer » (hote), garde N secondes, puis arrete ; journal relay-test.log.</summary>
    static int RelayTest(int seconds)
    {
        Directory.CreateDirectory(Settings.WorkDir);
        using var log = new StreamWriter(Path.Combine(Settings.WorkDir, "relay-test.log"), false) { AutoFlush = true };
        var s = Settings.Load();
        s.Host = true;
        try
        {
            var setup = new Setup(line => log.WriteLine(line), _ => { });
            using var relay = setup.StartRelay(s);
            Thread.Sleep(seconds * 1000);
            relay.Kill();
            relay.WaitForExit(5000);
            var save = Setup.ReadCoopSave();
            log.WriteLine($"sauvegarde : existe={save.Exists} progression={save.Progress} lune={save.MoonFlags} ({Setup.CoopSavePath})");
            return 0;
        }
        catch (Exception e)
        {
            log.WriteLine("ERREUR : " + e);
            return 1;
        }
    }

    /// <summary>
    /// Essai : « Jouer » en mode deux joueurs sur ce PC (relais, joueur 1, joueur 2), garde les jeux
    /// N secondes puis ferme tout ; journal duo-test.log (identifiants des processus en tete).
    /// </summary>
    static int DuoTest(int seconds, string dataDir)
    {
        Directory.CreateDirectory(Settings.WorkDir);
        using var log = new StreamWriter(Path.Combine(Settings.WorkDir, "duo-test.log"), false) { AutoFlush = true };
        var s = Settings.Load();
        s.Host = true;
        s.LocalDuo = true;
        var started = new List<System.Diagnostics.Process>();
        try
        {
            var setup = new Setup(line => log.WriteLine(line), _ => { });
            setup.ApplyPartySettings(s, dataDir);
            started.Add(setup.StartRelay(s));
            started.AddRange(setup.StartLocalDuoAsync(s, dataDir).GetAwaiter().GetResult());
            log.WriteLine("pids " + string.Join(' ', started.Skip(1).Select(p => p.Id)));
            Thread.Sleep(seconds * 1000);
            return 0;
        }
        catch (Exception e)
        {
            log.WriteLine("ERREUR : " + e);
            return 1;
        }
        finally
        {
            foreach (var p in started)
            {
                try
                {
                    if (!p.HasExited)
                        p.Kill();
                }
                catch (Exception)
                {
                    // Deja ferme.
                }
            }
            log.WriteLine("ferme");
        }
    }

    static int InstallHeadless(string dataDir, string zip, string ryujinx, string baseNsp, string update, string guest, string password, bool updateOnly)
    {
        Directory.CreateDirectory(Settings.WorkDir);
        var logPath = Path.Combine(Settings.WorkDir, "install.log");
        using var log = new StreamWriter(logPath, false) { AutoFlush = true };
        var s = Settings.Load();
        s.RyujinxExe = ryujinx ?? s.RyujinxExe;
        s.BaseNsp = baseNsp ?? s.BaseNsp;
        s.UpdateNsp = update ?? s.UpdateNsp;
        s.Host = guest == null ? (updateOnly ? s.Host : true) : false;
        s.RelayAddress = guest ?? (updateOnly ? s.RelayAddress : "");
        s.Password = password ?? (updateOnly ? s.Password : "");
        try
        {
            var setup = new Setup(line => log.WriteLine(line), p => log.WriteLine($"[{p} %]"));
            var options = new Setup.Options { Settings = s, DataDirOverride = dataDir, LocalModZip = zip };
            if (updateOnly)
                // Meme etape que le bouton « Jouer », sans lancer le jeu.
                log.WriteLine(setup.UpdateIfNeededAsync(options).GetAwaiter().GetResult() ? "MIS A JOUR" : "DEJA A JOUR");
            else
                setup.InstallAsync(options).GetAwaiter().GetResult();
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
