using System.Text;

namespace BotwCoopLauncher;

/// <summary>
/// Prise en charge de l'emulateur Eden (derive de yuzu), a cote de Ryujinx. Constats du 2026-11-16
/// (Eden v0.2.1, essais e1 a e4 et ed1, ed2 du projet) :
/// - Eden est « portable » quand un dossier user\ est a cote de eden.exe, sinon il range tout dans
///   %APPDATA%\eden. Cles : user\keys\prod.keys ; mods : user\load\&lt;titre&gt;\&lt;mod&gt;\exefs, romfs.
/// - Son qt-config.ini peut imposer d'autres dossiers (nand_directory, load_directory...) en chemins absolus.
/// - Lance avec -g sur le jeu de base, il n'applique la mise a jour 1.9.0 que si elle est installee dans
///   sa NAND : sinon le programme 1.0.0 demarre et le mod se coupe (version inconnue).
/// - Il ouvre les fichiers de sa NAND sans partage : un second Eden ne peut pas lire les memes ; le
///   joueur 2 sur ce PC a donc sa propre copie d'Eden et de sa NAND.
/// - Les messages du mod vont dans user\log\eden_log.txt (pas sur la sortie standard).
/// </summary>
static class Eden
{
    public static bool IsEdenExe(string exe) =>
        !string.IsNullOrEmpty(exe) && Path.GetFileName(exe).StartsWith("eden", StringComparison.OrdinalIgnoreCase);

    /// <summary>Dossier de donnees d'Eden pour cet executable.</summary>
    public static string DataDir(string exe)
    {
        var portable = Path.Combine(Path.GetDirectoryName(exe) ?? "", "user");
        if (Directory.Exists(portable))
            return portable;
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "eden");
    }

    /// <summary>Un dossier de donnees est-il celui d'Eden (et non de Ryujinx) ?</summary>
    public static bool IsEdenDataDir(string dataDir) =>
        Directory.Exists(Path.Combine(dataDir, "nand")) || File.Exists(Path.Combine(dataDir, "config", "qt-config.ini"));

    /// <summary>Dossier impose par qt-config.ini (nand_directory, load_directory...), sinon celui par defaut.</summary>
    public static string ConfiguredDir(string dataDir, string key)
    {
        var cfg = Path.Combine(dataDir, "config", "qt-config.ini");
        if (File.Exists(cfg))
        {
            foreach (var line in File.ReadLines(cfg))
            {
                if (!line.StartsWith(key + "_directory=", StringComparison.Ordinal))
                    continue;
                var value = line[(key.Length + "_directory=".Length)..].Trim();
                if (value.Length > 0)
                    return Path.GetFullPath(value.Replace('/', Path.DirectorySeparatorChar));
            }
        }
        return Path.Combine(dataDir, key);
    }

    public static string KeysFile(string dataDir) => Path.Combine(dataDir, "keys", "prod.keys");

    public static string ModTarget(string dataDir) =>
        Path.Combine(ConfiguredDir(dataDir, "load"), Setup.TitleId.ToUpperInvariant(), "botw-coop");

    public static string LogFile(string dataDir) => Path.Combine(dataDir, "log", "eden_log.txt");

    // ---- Installation d'un NSP (mise a jour, DLC) dans la NAND ----

    /// <summary>Taille signature + bourrage d'un ticket selon son type (switchbrew, « Ticket »).</summary>
    static int SignatureSkip(uint type) => type switch
    {
        0x10000 or 0x10003 => 4 + 0x200 + 0x3C,
        0x10001 or 0x10004 => 4 + 0x100 + 0x3C,
        0x10002 or 0x10005 => 4 + 0x3C + 0x40,
        _ => -1,
    };

    /// <summary>
    /// Copie les NCA d'un NSP dans nand\user\Contents\registered (&lt;NcaId&gt;.nca, metadonnees comprises),
    /// comme l'installation dans la NAND d'Eden, et ajoute a keys\title.keys la cle de titre (chiffree)
    /// des tickets communs qui n'y est pas encore. Rien n'est recopie si deja present. Renvoie le nombre
    /// de fichiers copies. Format PFS0 : switchbrew, « PFS0 ». Meme logique que tools/nsp_extract.py.
    /// </summary>
    public static int InstallNsp(string nsp, string dataDir, Action<string> log)
    {
        var registered = Path.Combine(ConfiguredDir(dataDir, "nand"), "user", "Contents", "registered");
        var titleKeys = Path.Combine(dataDir, "keys", "title.keys");
        Directory.CreateDirectory(registered);
        var known = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (File.Exists(titleKeys))
            foreach (var line in File.ReadLines(titleKeys))
                if (line.Contains('='))
                    known.Add(line.Split('=')[0].Trim());

        using var f = new FileStream(nsp, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var r = new BinaryReader(f);
        if (Encoding.ASCII.GetString(r.ReadBytes(4)) != "PFS0")
            throw new InvalidOperationException($"{Path.GetFileName(nsp)} n'est pas un NSP.");
        var count = r.ReadInt32();
        var namesSize = r.ReadInt32();
        r.ReadInt32();
        var table = new (long Off, long Size, int NameOff)[count];
        for (var i = 0; i < count; i++)
        {
            table[i] = (r.ReadInt64(), r.ReadInt64(), r.ReadInt32());
            r.ReadInt32();
        }
        var names = r.ReadBytes(namesSize);
        var dataStart = 16 + 24L * count + namesSize;
        var copied = 0;
        foreach (var (off, size, nameOff) in table)
        {
            var end = Array.IndexOf(names, (byte)0, nameOff);
            var name = Encoding.ASCII.GetString(names, nameOff, (end < 0 ? names.Length : end) - nameOff);
            if (name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase))
            {
                var rights = name[..^4];
                if (known.Contains(rights))
                    continue;
                f.Position = dataStart + off;
                var tik = r.ReadBytes((int)Math.Min(size, 0x1000));
                var skip = SignatureSkip(BitConverter.ToUInt32(tik, 0));
                if (skip < 0 || tik.Length < skip + 0x170)
                    continue;
                if (!Convert.ToHexString(tik, skip + 0x160, 16).Equals(rights, StringComparison.OrdinalIgnoreCase))
                    continue;
                File.AppendAllText(titleKeys, $"{Environment.NewLine}{rights.ToLowerInvariant()}={Convert.ToHexString(tik, skip + 0x40, 16).ToLowerInvariant()}{Environment.NewLine}");
                known.Add(rights);
                log($"Eden : clé de titre ajoutée ({rights[..16]}…).");
                continue;
            }
            if (!name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase))
                continue;
            var target = Path.Combine(registered, name.Replace(".cnmt.nca", ".nca", StringComparison.OrdinalIgnoreCase));
            if (File.Exists(target) && new FileInfo(target).Length == size)
                continue;
            f.Position = dataStart + off;
            using (var dst = new FileStream(target + ".part", FileMode.Create, FileAccess.Write))
            {
                var buffer = new byte[1 << 22];
                var left = size;
                while (left > 0)
                {
                    var n = f.Read(buffer, 0, (int)Math.Min(left, buffer.Length));
                    if (n <= 0)
                        throw new InvalidOperationException($"{Path.GetFileName(nsp)} est incomplet.");
                    dst.Write(buffer, 0, n);
                    left -= n;
                }
            }
            File.Move(target + ".part", target, true);
            copied++;
        }
        return copied;
    }

    // ---- Joueur 2 sur ce PC ----

    /// <summary>Copie d'Eden du joueur 2 (deux fenetres sur ce PC) : Work\joueur2-eden\eden.exe.</summary>
    public static string Player2Dir => Path.Combine(Setup.Work, "joueur2-eden");

    public static string Player2Exe => Path.Combine(Player2Dir, "eden.exe");

    public static string Player2DataDir => Path.Combine(Player2Dir, "user");

    /// <summary>
    /// Prepare l'Eden du joueur 2 a partir de celui du joueur 1 (lu seulement) : programme, cles,
    /// micrologiciel et contenus installes (copies : Eden les verrouille), profils, reglages (chemins
    /// ramenes dans son propre dossier) ; sauvegardes copiees la premiere fois si demande.
    /// </summary>
    public static void PreparePlayer2(string exe1, bool copySave, Action<string> log)
    {
        var data1 = DataDir(exe1);
        var dir2 = Player2Dir;
        var data2 = Player2DataDir;
        var firstTime = !File.Exists(Player2Exe);
        Directory.CreateDirectory(data2);
        // Programme : a chaque fois (une nouvelle version d'Eden chez le joueur 1 passe au joueur 2).
        var exeDir = Path.GetDirectoryName(exe1)!;
        foreach (var file in Directory.GetFiles(exeDir))
            File.Copy(file, Path.Combine(dir2, Path.GetFileName(file)), true);
        foreach (var sub in Directory.GetDirectories(exeDir))
            if (!string.Equals(Path.GetFileName(sub), "user", StringComparison.OrdinalIgnoreCase))
                Setup.CopyTree(sub, Path.Combine(dir2, Path.GetFileName(sub)));
        if (!firstTime)
            return;
        log($"Joueur 2 : préparation de sa copie d'Eden ({dir2})… (quelques Go, une seule fois)");
        Setup.CopyTree(Path.Combine(data1, "keys"), Path.Combine(data2, "keys"));
        var nand1 = ConfiguredDir(data1, "nand");
        var nand2 = Path.Combine(data2, "nand");
        foreach (var sub in new[] { Path.Combine("system", "Contents"), Path.Combine("user", "Contents"), Path.Combine("system", "save") })
            if (Directory.Exists(Path.Combine(nand1, sub)))
                Setup.CopyTree(Path.Combine(nand1, sub), Path.Combine(nand2, sub));
        var userSave = Path.Combine(nand1, "user", "save");
        if (copySave && Directory.Exists(userSave))
        {
            Setup.CopyTree(userSave, Path.Combine(nand2, "user", "save"));
            log("Joueur 2 : copie des sauvegardes du joueur 1 (les originales ne sont que lues).");
        }
        else
        {
            log("Joueur 2 : nouvelle partie (pas de copie de sauvegarde).");
        }
        // Reglages : ceux du joueur 1, dossiers ramenes dans le dossier du joueur 2.
        var cfg1 = Path.Combine(data1, "config", "qt-config.ini");
        if (File.Exists(cfg1))
        {
            var user = data2.Replace('\\', '/');
            var lines = File.ReadAllLines(cfg1).Select(l =>
            {
                foreach (var key in new[] { "nand", "sdmc", "load", "dump", "tas" })
                    if (l.StartsWith(key + "_directory=", StringComparison.Ordinal))
                        return $"{key}_directory={user}/{key}";
                return l.StartsWith(@"Screenshots\screenshot_path=", StringComparison.Ordinal) ? $@"Screenshots\screenshot_path={user}/screenshots" : l;
            });
            Directory.CreateDirectory(Path.Combine(data2, "config"));
            File.WriteAllLines(Path.Combine(data2, "config", "qt-config.ini"), lines, new UTF8Encoding(false));
        }
        log("Joueur 2 : réglez sa manette une fois dans SA fenêtre d'Eden (Émulation > Configurer > Contrôles) ; le réglage est gardé.");
    }
}
