using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http.Headers;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Es;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using Path = System.IO.Path;

namespace BotwCoopLauncher;

/// <summary>Une version publiee du mod (release GitHub du depot public).</summary>
public sealed class Release
{
    public string Tag = "";
    public string ModZipUrl = "";
    public string LauncherUrl = "";
    public Version Version => ParseVersion(Tag);

    public static Version ParseVersion(string tag)
    {
        var t = (tag ?? "").Trim().TrimStart('v', 'V');
        return Version.TryParse(t, out var v) ? v : new Version(0, 0);
    }
}

/// <summary>
/// Installation du mod : telechargement de la release, lecture des fichiers du jeu de l'utilisateur,
/// generation des fichiers du mod (scripts Python du projet), copie dans Ryujinx.
/// </summary>
public sealed class Setup
{
    /// <summary>Depot public du mod (releases : botw-coop-mod.zip et BotwCoopLauncher.exe).</summary>
    public const string Repo = "dorianskyfr/BOTW-COOP-mod";
    public const string TitleId = "01007ef00011e000";

    /// <summary>Python portable officiel (python.org), meme version que celle des essais du projet.</summary>
    const string PythonZipUrl = "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip";
    /// <summary>Installeur officiel de pip (PyPA).</summary>
    const string GetPipUrl = "https://bootstrap.pypa.io/get-pip.py";
    /// <summary>Paquets PyPI utilises par les scripts, versions des essais du projet.</summary>
    static readonly string[] PythonPackages = { "oead==1.3.0", "rstb==1.2.2" };

    /// <summary>Fichiers du jeu lus par les scripts (README, « Liste blanche des drapeaux »).</summary>
    static readonly string[] RomfsPrefixes =
    {
        "/Pack/Bootup.pack", "/Pack/Bootup_EUfr.pack",
        "/Actor/Pack/Npc_DungeonPriest001.sbactorpack", "/Actor/Pack/PauseMenuPlayer.sbactorpack",
        "/Actor/Pack/Armor_", "/Actor/Pack/Weapon_", "/Model/Armor_",
        "/Actor/ActorInfo.product.sbyml", "/System/Resource/ResourceSizeTable.product.srsizetable",
    };

    readonly Action<string> _log;
    readonly Action<int> _progress;
    static readonly HttpClient Http = CreateHttp();

    public Setup(Action<string> log, Action<int> progress)
    {
        _log = log;
        _progress = progress;
    }

    static HttpClient CreateHttp()
    {
        var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        // Nom de client generique (GitHub exige un User-Agent) ; aucune donnee personnelle.
        http.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("BotwCoopLauncher", LauncherVersion.ToString()));
        return http;
    }

    public static Version LauncherVersion => Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0);

    static string Work => Settings.WorkDir;

    // ---- Releases ----

    public async Task<Release> GetLatestReleaseAsync()
    {
        using var req = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Repo}/releases/latest");
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        using var resp = await Http.SendAsync(req);
        if (resp.StatusCode == System.Net.HttpStatusCode.NotFound)
            return null; // pas encore de release publiee
        resp.EnsureSuccessStatusCode();
        var json = JsonNode.Parse(await resp.Content.ReadAsStringAsync());
        var rel = new Release { Tag = (string)json?["tag_name"] ?? "" };
        foreach (var a in json?["assets"]?.AsArray() ?? new JsonArray())
        {
            var name = (string)a?["name"] ?? "";
            var url = (string)a?["browser_download_url"] ?? "";
            if (!url.StartsWith($"https://github.com/{Repo}/releases/download/", StringComparison.Ordinal))
                continue; // on ne telecharge que depuis les releases du depot
            if (name.Equals("botw-coop-mod.zip", StringComparison.OrdinalIgnoreCase))
                rel.ModZipUrl = url;
            else if (name.Equals("BotwCoopLauncher.exe", StringComparison.OrdinalIgnoreCase))
                rel.LauncherUrl = url;
        }
        return rel;
    }

    async Task DownloadAsync(string url, string dest)
    {
        _log($"Téléchargement : {url}");
        Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
        using var resp = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead);
        resp.EnsureSuccessStatusCode();
        var tmp = dest + ".part";
        await using (var fs = File.Create(tmp))
            await resp.Content.CopyToAsync(fs);
        File.Move(tmp, dest, true);
    }

    /// <summary>Remplace l'executable du launcher par la version de la release, puis le relance.</summary>
    public async Task<bool> SelfUpdateAsync(Release rel)
    {
        if (rel == null || string.IsNullOrEmpty(rel.LauncherUrl) || rel.Version <= LauncherVersion)
            return false;
        var exe = Environment.ProcessPath;
        if (string.IsNullOrEmpty(exe))
            return false;
        var fresh = exe + ".new";
        await DownloadAsync(rel.LauncherUrl, fresh);
        var old = exe + ".old";
        if (File.Exists(old))
            File.Delete(old);
        File.Move(exe, old);   // Windows permet de renommer un executable en cours d'execution
        File.Move(fresh, exe);
        Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
        return true;
    }

    // ---- Installation ----

    public sealed class Options
    {
        public Settings Settings;
        /// <summary>Dossier de donnees de Ryujinx impose (essais) ; sinon detecte.</summary>
        public string DataDirOverride;
        /// <summary>Archive du mod locale (essais, ou hors ligne) ; sinon telechargee.</summary>
        public string LocalModZip;
        public Release Release;
    }

    public async Task InstallAsync(Options o)
    {
        var s = o.Settings;
        _progress(0);
        CheckInputs(s);
        var dataDir = s.RyujinxDataDir(o.DataDirOverride);
        var keys = Path.Combine(dataDir, "system", "prod.keys");
        if (!File.Exists(keys))
            throw new InvalidOperationException($"Clés introuvables : {keys}. Lancez une fois Ryujinx avec vos clés installées.");

        // 1. Archive du mod
        string zip, version;
        if (!string.IsNullOrEmpty(o.LocalModZip))
        {
            zip = o.LocalModZip;
            version = "local";
        }
        else
        {
            var rel = o.Release ?? await GetLatestReleaseAsync();
            if (rel == null || string.IsNullOrEmpty(rel.ModZipUrl))
                throw new InvalidOperationException("Aucune version du mod n'est encore publiée.");
            zip = Path.Combine(Work, "downloads", $"botw-coop-mod-{rel.Tag}.zip");
            if (!File.Exists(zip))
                await DownloadAsync(rel.ModZipUrl, zip);
            version = rel.Tag;
        }
        var modDir = Path.Combine(Work, "mod");
        if (Directory.Exists(modDir))
            Directory.Delete(modDir, true);
        ZipFile.ExtractToDirectory(zip, modDir);
        if (!File.Exists(Path.Combine(modDir, "subsdk9")))
            throw new InvalidOperationException("Archive du mod incomplète (subsdk9 absent).");
        var modVersion = File.Exists(Path.Combine(modDir, "VERSION")) ? File.ReadAllText(Path.Combine(modDir, "VERSION")).Trim() : version;
        _log($"Mod : version {modVersion}");
        _progress(10);

        // 2. Fichiers generes depuis le jeu de l'utilisateur (une fois par version et par copie du jeu)
        var generated = Path.Combine(Work, "generated", "romfs");
        var key = GenerationKey(s, modDir);
        if (s.GeneratedKey != key || !File.Exists(Path.Combine(generated, "Actor", "ActorInfo.product.sbyml")))
        {
            var python = await EnsurePythonAsync();
            _progress(30);
            var romfs = Path.Combine(Work, "romfs-1.9.0");
            ExtractRomfs(keys, s.BaseNsp, s.UpdateNsp, romfs);
            _progress(50);
            if (Directory.Exists(Path.Combine(Work, "generated")))
                Directory.Delete(Path.Combine(Work, "generated"), true);
            Directory.CreateDirectory(Path.Combine(generated, "botw-coop"));
            var scripts = Path.Combine(modDir, "scripts");
            var flags = Path.Combine(Work, "generated", "flags.tsv");
            await RunAsync(python, $"-I \"{Path.Combine(scripts, "gamedata_names.py")}\" \"{romfs}\" \"{flags}\"", "catalogue des drapeaux");
            await RunAsync(python, $"-I \"{Path.Combine(scripts, "build_sync_flags.py")}\" \"{flags}\" \"{Path.Combine(generated, "botw-coop", "sync-flags.bin")}\"", "liste blanche");
            _progress(60);
            await RunAsync(python, $"-I \"{Path.Combine(scripts, "build_travel_points.py")}\" \"{romfs}\" \"{Path.Combine(generated, "botw-coop", "travel-points.txt")}\"", "points de voyage");
            await RunAsync(python, $"-I \"{Path.Combine(scripts, "build_puppet.py")}\" \"{romfs}\" \"{generated}\"", "Link du partenaire, armures, armes");
            _progress(85);
            await RunAsync(python, $"-I \"{Path.Combine(scripts, "build_item_names.py")}\" \"{romfs}\" EUfr \"{generated}\"", "noms des objets");
            s.GeneratedKey = key;
        }
        else
        {
            _log("Fichiers du mod déjà générés pour cette version et ce jeu.");
        }
        _progress(90);

        // 3. Copie dans Ryujinx
        var target = ModTarget(dataDir);
        Directory.CreateDirectory(Path.Combine(target, "exefs"));
        File.Copy(Path.Combine(modDir, "subsdk9"), Path.Combine(target, "exefs", "subsdk9"), true);
        CopyTree(generated, Path.Combine(target, "romfs"));
        WriteModConfig(target, s);
        _log($"Mod installé dans {target}");
        if (s.FixIgnoreMissingServices)
            FixRyujinxConfig(dataDir);
        EnsureUpdateSelected(dataDir, s.UpdateNsp);
        s.InstalledVersion = modVersion;
        s.Save();
        _progress(100);
    }

    static void CheckInputs(Settings s)
    {
        if (string.IsNullOrEmpty(s.RyujinxExe) || !File.Exists(s.RyujinxExe))
            throw new InvalidOperationException("Indiquez le fichier Ryujinx.exe.");
        if (!File.Exists(s.BaseNsp))
            throw new InvalidOperationException("Indiquez le jeu de base (.nsp).");
        if (!File.Exists(s.UpdateNsp))
            throw new InvalidOperationException("Indiquez la mise à jour 1.9.0 (.nsp).");
    }

    public static string ModTarget(string dataDir) =>
        Path.Combine(dataDir, "mods", "contents", TitleId, "botw-coop");

    static string GenerationKey(Settings s, string modDir)
    {
        var sb = new StringBuilder();
        foreach (var f in new[] { s.BaseNsp, s.UpdateNsp })
            sb.Append(Path.GetFileName(f)).Append('|').Append(new FileInfo(f).Length).Append('|');
        foreach (var f in Directory.GetFiles(Path.Combine(modDir, "scripts")).OrderBy(x => x, StringComparer.Ordinal))
            sb.Append(Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(f)))).Append('|');
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    static void CopyTree(string src, string dst)
    {
        foreach (var dir in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(dst, Path.GetRelativePath(src, dir)));
        Directory.CreateDirectory(dst);
        foreach (var file in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            File.Copy(file, Path.Combine(dst, Path.GetRelativePath(src, file)), true);
    }

    void WriteModConfig(string target, Settings s)
    {
        var lines = new List<string>();
        var relay = RelayFor(s);
        if (!string.IsNullOrEmpty(relay))
            lines.Add($"relay={relay}");
        if (!string.IsNullOrEmpty(s.Password))
            lines.Add($"password={s.Password}");
        var cfg = Path.Combine(target, "romfs", "botw-coop.txt");
        Directory.CreateDirectory(Path.GetDirectoryName(cfg)!);
        File.WriteAllLines(cfg, lines, new UTF8Encoding(false));
        _log($"Relais : {(string.IsNullOrEmpty(relay) ? "127.0.0.1:47800" : relay)}, mot de passe : {(string.IsNullOrEmpty(s.Password) ? "non" : "oui")}");
    }

    /// <summary>Hote : son propre relais sur son adresse de reseau local ; invite : l'adresse saisie.</summary>
    public static string RelayFor(Settings s)
    {
        if (s.Host)
        {
            var ip = LanAddress();
            return ip == null ? "" : $"{ip}:47800";
        }
        var a = (s.RelayAddress ?? "").Trim();
        if (a.Length > 0 && !a.Contains(':'))
            a += ":47800";
        return a;
    }

    /// <summary>Premiere adresse IPv4 privee d'une interface active (reseau local uniquement).</summary>
    public static string LanAddress()
    {
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || nic.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                continue;
            foreach (var u in nic.GetIPProperties().UnicastAddresses)
            {
                if (u.Address.AddressFamily != AddressFamily.InterNetwork)
                    continue;
                var b = u.Address.GetAddressBytes();
                var isPrivate = b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168);
                if (isPrivate)
                    return u.Address.ToString();
            }
        }
        return null;
    }

    /// <summary>
    /// Ryujinx : « Ignorer les services manquants » doit etre active (sinon le jeu 1.9.0 s'arrete
    /// apres environ 2 minutes, README). Copie de securite de Config.json avant modification.
    /// </summary>
    void FixRyujinxConfig(string dataDir)
    {
        var cfg = Path.Combine(dataDir, "Config.json");
        if (!File.Exists(cfg))
        {
            _log("Config.json de Ryujinx absent : réglage « Ignorer les services manquants » à faire dans Ryujinx.");
            return;
        }
        var json = JsonNode.Parse(File.ReadAllText(cfg));
        if (json is not JsonObject obj)
            return;
        if (obj["ignore_missing_services"]?.GetValue<bool>() == true)
            return;
        var backup = cfg + ".avant-botw-coop";
        if (!File.Exists(backup))
            File.Copy(cfg, backup);
        obj["ignore_missing_services"] = true;
        File.WriteAllText(cfg, obj.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        _log($"Ryujinx : « Ignorer les services manquants » activé (copie : {backup}).");
    }

    /// <summary>
    /// Ryujinx applique la mise a jour choisie dans games\&lt;titre&gt;\updates.json
    /// ({"selected": chemin, "paths": [...]}, format ecrit aussi par tools/run-live.ps1).
    /// </summary>
    void EnsureUpdateSelected(string dataDir, string updateNsp)
    {
        var file = Path.Combine(dataDir, "games", TitleId, "updates.json");
        JsonObject obj = null;
        if (File.Exists(file))
            obj = JsonNode.Parse(File.ReadAllText(file)) as JsonObject;
        obj ??= new JsonObject();
        if (string.Equals((string)obj["selected"], updateNsp, StringComparison.OrdinalIgnoreCase))
            return;
        if (File.Exists(file) && !File.Exists(file + ".avant-botw-coop"))
            File.Copy(file, file + ".avant-botw-coop");
        var paths = obj["paths"] as JsonArray ?? new JsonArray();
        if (!paths.Any(p => string.Equals((string)p, updateNsp, StringComparison.OrdinalIgnoreCase)))
            paths.Add(updateNsp);
        obj["paths"] = paths.DeepClone();
        obj["selected"] = updateNsp;
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, obj.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        _log("Ryujinx : mise à jour 1.9.0 sélectionnée pour le jeu.");
    }

    public void Uninstall(Settings s, string dataDirOverride)
    {
        var target = ModTarget(s.RyujinxDataDir(dataDirOverride));
        if (Directory.Exists(target))
        {
            Directory.Delete(target, true);
            _log($"Retiré : {target}");
        }
        else
        {
            _log("Le mod n'était pas installé.");
        }
        s.InstalledVersion = "";
        s.Save();
    }

    // ---- Python portable ----

    async Task<string> EnsurePythonAsync()
    {
        var dir = Path.Combine(Work, "python");
        var exe = Path.Combine(dir, "python.exe");
        if (File.Exists(exe) && await TryRunAsync(exe, "-c \"import oead, rstb\""))
            return exe;
        _log("Installation de Python portable (python.org) et des paquets oead, rstb (PyPI)…");
        var zip = Path.Combine(Work, "downloads", Path.GetFileName(PythonZipUrl));
        if (!File.Exists(zip))
            await DownloadAsync(PythonZipUrl, zip);
        if (Directory.Exists(dir))
            Directory.Delete(dir, true);
        ZipFile.ExtractToDirectory(zip, dir);
        // Python portable : « import site » est commente dans pythonXY._pth ; il faut l'activer pour pip.
        foreach (var pth in Directory.GetFiles(dir, "python*._pth"))
        {
            var text = File.ReadAllText(pth).Replace("#import site", "import site");
            File.WriteAllText(pth, text);
        }
        var getPip = Path.Combine(Work, "downloads", "get-pip.py");
        await DownloadAsync(GetPipUrl, getPip);
        await RunAsync(exe, $"\"{getPip}\" --no-warn-script-location", "installation de pip");
        await RunAsync(exe, "-m pip install --no-warn-script-location " + string.Join(' ', PythonPackages), "paquets Python");
        return exe;
    }

    static async Task<bool> TryRunAsync(string exe, string args)
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo(exe, args)
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true });
            if (p == null)
                return false;
            await p.WaitForExitAsync();
            return p.ExitCode == 0;
        }
        catch (Exception)
        {
            return false;
        }
    }

    async Task RunAsync(string exe, string args, string what)
    {
        _log($"→ {what}");
        var psi = new ProcessStartInfo(exe, args)
        {
            UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        using var p = Process.Start(psi) ?? throw new InvalidOperationException($"{what} : impossible de lancer {exe}");
        var stdout = p.StandardOutput.ReadToEndAsync();
        var stderr = p.StandardError.ReadToEndAsync();
        await p.WaitForExitAsync();
        var outText = (await stdout).Trim();
        var errText = (await stderr).Trim();
        foreach (var line in outText.Split('\n').TakeLast(4))
            if (line.Trim().Length > 0)
                _log("   " + line.TrimEnd());
        if (p.ExitCode != 0)
        {
            foreach (var line in errText.Split('\n').TakeLast(8))
                _log("   " + line.TrimEnd());
            throw new InvalidOperationException($"{what} : échec (code {p.ExitCode})");
        }
    }

    // ---- Lecture des fichiers du jeu (meme methode que tools/romfs-extract) ----

    void ExtractRomfs(string keysPath, string baseNsp, string updateNsp, string outDir)
    {
        _log("Lecture des fichiers nécessaires dans votre jeu (lecture seule)…");
        var keys = new KeySet();
        ExternalKeyReader.ReadKeyFile(keys, keysPath, null, null, null, null);
        using var baseFile = new LocalStorage(baseNsp, FileAccess.Read, FileMode.Open);
        using var updFile = new LocalStorage(updateNsp, FileAccess.Read, FileMode.Open);
        var program = ProgramNca(baseFile, keys, baseNsp);
        var update = ProgramNca(updFile, keys, updateNsp);
        var romfs = new RomFsFileSystem(program.OpenStorageWithPatch(update, NcaSectionType.Data, IntegrityCheckLevel.None));
        int count = 0;
        long total = 0;
        foreach (DirectoryEntryEx e in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            if (e.Type != DirectoryEntryType.File || !RomfsPrefixes.Any(p => e.FullPath.StartsWith(p, StringComparison.Ordinal)))
                continue;
            var dest = Path.Combine(outDir, e.FullPath.TrimStart('/').Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
            using var file = new UniqueRef<IFile>();
            romfs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            using var output = File.Create(dest);
            var buffer = new byte[1 << 22];
            long at = 0;
            while (at < e.Size)
            {
                file.Get.Read(out long got, at, buffer.AsSpan(0, (int)Math.Min(buffer.Length, e.Size - at))).ThrowIfFailure();
                if (got <= 0)
                    throw new IOException($"{e.FullPath} : lecture arrêtée à {at} sur {e.Size}");
                output.Write(buffer, 0, (int)got);
                at += got;
            }
            count++;
            total += e.Size;
        }
        if (count == 0)
            throw new InvalidOperationException("Aucun fichier lu dans le jeu : vérifiez qu'il s'agit bien de BOTW et de sa mise à jour 1.9.0.");
        _log($"   {count} fichiers lus ({total / (1024 * 1024)} Mo)");
    }

    static Nca ProgramNca(IStorage nspStorage, KeySet keys, string name)
    {
        var pfs = new PartitionFileSystem();
        pfs.Initialize(nspStorage).ThrowIfFailure();
        foreach (DirectoryEntryEx t in pfs.EnumerateEntries("/", "*.tik"))
        {
            using var tik = new UniqueRef<IFile>();
            if (pfs.OpenFile(ref tik.Ref, t.FullPath.ToU8Span(), OpenMode.Read).IsFailure())
                continue;
            var ticket = new Ticket(tik.Get.AsStream());
            var titleKey = ticket.GetTitleKey(keys);
            if (titleKey != null)
                keys.ExternalKeySet.Add(new RightsId(ticket.RightsId), new AccessKey(titleKey));
        }
        foreach (DirectoryEntryEx e in pfs.EnumerateEntries("/", "*.nca"))
        {
            var f = new UniqueRef<IFile>();
            pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            var nca = new Nca(keys, f.Release().AsStorage());
            if (nca.Header.ContentType == NcaContentType.Program)
                return nca;
        }
        throw new InvalidOperationException($"Pas de programme dans {Path.GetFileName(name)} (un fichier .nsp de BOTW est attendu).");
    }

    // ---- Lancement ----

    public Process StartRelay(Settings s, string modDirOverride = null)
    {
        var python = Path.Combine(Work, "python", "python.exe");
        var relay = Path.Combine(modDirOverride ?? Path.Combine(Work, "mod"), "scripts", "relay.py");
        if (!File.Exists(python) || !File.Exists(relay))
            throw new InvalidOperationException("Installez d'abord le mod (Python et le relais manquent).");
        var ip = LanAddress() ?? "127.0.0.1";
        var args = $"-I \"{relay}\" --host {ip}";
        if (!string.IsNullOrEmpty(s.Password))
            args += $" --password \"{s.Password}\"";
        _log($"Relais lancé sur {ip}:47800 (réseau local uniquement). Donnez cette adresse à l'autre joueur.");
        return Process.Start(new ProcessStartInfo(python, args) { UseShellExecute = false, CreateNoWindow = true });
    }

    public void StartGame(Settings s)
    {
        _log("Lancement de Ryujinx…");
        Process.Start(new ProcessStartInfo(s.RyujinxExe, $"\"{s.BaseNsp}\"") { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(s.RyujinxExe) });
    }
}
