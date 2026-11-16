using System.Text.Json;

namespace BotwCoopLauncher;

/// <summary>Reglages du launcher, gardes dans %LOCALAPPDATA%\BotwCoop\launcher.json.</summary>
public sealed class Settings
{
    /// <summary>Executable de l'emulateur : Ryujinx.exe ou eden.exe (nom garde pour les reglages existants).</summary>
    public string RyujinxExe { get; set; } = "";
    public string BaseNsp { get; set; } = "";
    public string UpdateNsp { get; set; } = "";
    public bool Host { get; set; } = true;
    public string RelayAddress { get; set; } = "";
    public string Password { get; set; } = "";
    public bool FixIgnoreMissingServices { get; set; } = true;
    /// <summary>Deux joueurs sur ce PC : deux fenetres Ryujinx, le joueur 1 heberge (Host vaut alors vrai).</summary>
    public bool LocalDuo { get; set; }
    /// <summary>Premiere preparation du joueur 2 : partir d'une copie des sauvegardes du joueur 1.</summary>
    public bool Player2CopiesSave { get; set; } = true;
    /// <summary>Conseil sur la demande du pare-feu Windows deja montre a l'hote.</summary>
    public bool FirewallTipShown { get; set; }
    /// <summary>Version du mod installee (etiquette de la release GitHub).</summary>
    public string InstalledVersion { get; set; } = "";
    /// <summary>Empreinte des fichiers du jeu et de la version des scripts ayant servi a la generation.</summary>
    public string GeneratedKey { get; set; } = "";

    /// <summary>Dossier de travail ; --work-dir le remplace (essais).</summary>
    public static string WorkDirOverride;

    public static string WorkDir => WorkDirOverride ??
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BotwCoop");

    static string FilePath => Path.Combine(WorkDir, "launcher.json");

    public static Settings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings();
        }
        catch (Exception)
        {
            // Fichier abime : on repart des valeurs par defaut.
        }
        return new Settings();
    }

    public void Save()
    {
        Directory.CreateDirectory(WorkDir);
        File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }

    /// <summary>
    /// Dossier de donnees de Ryujinx : "portable" a cote de l'executable s'il existe (mode portable
    /// de Ryujinx), sinon %APPDATA%\Ryujinx (installation standard).
    /// </summary>
    public string RyujinxDataDir(string overrideDir = null)
    {
        if (!string.IsNullOrEmpty(overrideDir))
            return overrideDir;
        // Eden (son executable est choisi dans le meme champ) : voir Eden.cs.
        if (Eden.IsEdenExe(RyujinxExe))
            return Eden.DataDir(RyujinxExe);
        if (!string.IsNullOrEmpty(RyujinxExe))
        {
            var portable = Path.Combine(Path.GetDirectoryName(RyujinxExe) ?? "", "portable");
            if (Directory.Exists(portable))
                return portable;
        }
        return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Ryujinx");
    }
}
