using System.Diagnostics;
using System.Text.Json.Nodes;

namespace BotwCoopLauncher;

/// <summary>
/// Fenetre du launcher : bandeau (icone du jeu lue dans le .nsp de l'utilisateur), partie (hote /
/// invite, mot de passe), Jouer / Installer, carrousel de captures du depot ; page « Parametres »
/// pour les chemins ; journal repliable.
/// </summary>
public sealed class MainForm : Form
{
    readonly Settings _s = Settings.Load();
    readonly string _dataDirOverride;
    readonly string _localZip;
    readonly float _k;
    Release _latest;
    Process _relay;

    readonly Hero _hero = new();
    readonly Panel _mainPage = new();
    readonly Panel _settingsPage = new();
    readonly Panel _logPage = new();

    // Partie
    readonly StatusLine _status = new();
    readonly FlatButton _hostBtn = new() { Kind = FlatButton.Kinds.Segment, Text = "J'héberge" };
    readonly FlatButton _guestBtn = new() { Kind = FlatButton.Kinds.Segment, Text = "Je rejoins un ami" };
    readonly InputBox _relayAddr = new() { Placeholder = "Adresse de l'hôte, ex. 192.168.1.20" };
    readonly Label _hostInfo = new() { AutoSize = false, ForeColor = Theme.Muted };
    readonly InputBox _password = new() { Placeholder = "Facultatif, le même pour les deux" };
    readonly FlatButton _play = new() { Kind = FlatButton.Kinds.Primary, Text = "JOUER", Radius = 10 };
    readonly FlatButton _install = new() { Text = "Installer / Mettre à jour", Radius = 10 };
    readonly ThinProgress _bar = new();
    readonly Label _lastLine = new() { AutoSize = false, ForeColor = Theme.Muted, AutoEllipsis = true };
    readonly Carousel _carousel = new() { Empty = "Captures du mod\n(chargées depuis le dépôt du projet)" };

    // Pied de page
    readonly FlatButton _settingsBtn = new() { Kind = FlatButton.Kinds.Ghost, Text = "⚙  Paramètres" };
    readonly FlatButton _logBtn = new() { Kind = FlatButton.Kinds.Ghost, Text = "☰  Journal" };
    readonly FlatButton _uninstall = new() { Kind = FlatButton.Kinds.DangerGhost, Text = "Désinstaller" };
    readonly Label _versionLabel = new() { AutoSize = false, ForeColor = Theme.Muted, TextAlign = ContentAlignment.MiddleRight };

    // Parametres
    readonly InputBox _ryujinx = new() { Placeholder = "Chemin de Ryujinx.exe" };
    readonly InputBox _base = new() { Placeholder = "Jeu de base BOTW (.nsp)" };
    readonly InputBox _update = new() { Placeholder = "Mise à jour 1.9.0 (.nsp)" };
    readonly CheckBox _fixCfg = new()
    {
        Text = "Activer « Ignorer les services manquants » dans Ryujinx (nécessaire au jeu 1.9.0)",
        ForeColor = Theme.Text, FlatStyle = FlatStyle.Flat, AutoSize = true,
    };
    readonly FlatButton _back = new() { Kind = FlatButton.Kinds.Primary, Text = "Enregistrer", Radius = 10 };

    readonly TextBox _log = new()
    {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, BorderStyle = BorderStyle.None,
        BackColor = Theme.Surface, ForeColor = Color.FromArgb(190, 200, 210), Font = new Font("Consolas", 9f),
    };

    readonly List<FlatButton> _busyButtons = new();

    public MainForm(string dataDirOverride, string localZip)
    {
        _dataDirOverride = dataDirOverride;
        _localZip = localZip;
        _k = DeviceDpi / 96f;
        Text = "BOTW Coop";
        BackColor = Theme.Bg;
        ForeColor = Theme.Text;
        Font = Theme.Font(10f);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(S(940), S(660));
        try
        {
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath ?? "");
        }
        catch (Exception)
        {
            // Icone par defaut.
        }

        BuildHero();
        BuildMainPage();
        BuildSettingsPage();
        BuildLogPage();
        BuildFooter();
        _busyButtons.AddRange(new[] { _play, _install, _uninstall, _back });

        LoadForm();
        _hostBtn.Click += (_, _) => SetRole(true);
        _guestBtn.Click += (_, _) => SetRole(false);
        _install.Click += async (_, _) => await Busy(InstallAsync);
        _play.Click += async (_, _) => await Busy(PlayAsync);
        _uninstall.Click += (_, _) => Uninstall();
        _settingsBtn.Click += (_, _) => ShowPage(_settingsPage.Visible ? _mainPage : _settingsPage);
        _logBtn.Click += (_, _) => ShowPage(_logPage.Visible ? _mainPage : _logPage);
        _back.Click += (_, _) => { ReadForm(); _s.Save(); RefreshStatus(); LoadGameIconAsync(); ShowPage(_mainPage); };
        HandleCreated += (_, _) => Theme.DarkTitleBar(Handle);
        Shown += async (_, _) => await OnShownAsync();
        FormClosing += (_, _) => StopRelay();

        CleanOldExe();
        RefreshStatus();
        // Premier lancement : les chemins d'abord.
        ShowPage(IsConfigured() ? _mainPage : _settingsPage);
    }

    int S(int v) => (int)Math.Round(v * _k);

    // ---- Construction ----

    void BuildHero()
    {
        _hero.SetBounds(0, 0, ClientSize.Width, S(232));
        _hero.Subtitle = "Breath of the Wild à deux, en réseau local";
        _hero.Badge = $"launcher {Setup.LauncherVersion.ToString(3)}";
        Controls.Add(_hero);
    }

    Label Caption(string text, Control parent, int x, int y, int w = 300)
    {
        var l = new Label
        {
            Text = text, AutoSize = false, ForeColor = Theme.Muted, Font = Theme.Font(9f, semibold: true),
            BackColor = parent.BackColor,
        };
        l.SetBounds(x, y, w, S(20));
        parent.Controls.Add(l);
        return l;
    }

    void BuildMainPage()
    {
        _mainPage.BackColor = Theme.Bg;
        _mainPage.SetBounds(0, S(232), ClientSize.Width, S(372));
        Controls.Add(_mainPage);

        var left = new Card();
        left.SetBounds(S(24), S(8), S(560), S(352));
        _mainPage.Controls.Add(left);

        _status.SetBounds(S(22), S(18), S(516), S(28));
        left.Controls.Add(_status);

        Caption("PARTIE", left, S(24), S(62));
        _hostBtn.SetBounds(S(22), S(84), S(150), S(38));
        _guestBtn.SetBounds(S(178), S(84), S(170), S(38));
        _relayAddr.SetBounds(S(22), S(132), S(516), S(38));
        _hostInfo.SetBounds(S(24), S(132), S(514), S(38));
        _hostInfo.BackColor = left.BackColor;
        _hostInfo.Font = Theme.Font(9.5f);
        _hostInfo.TextAlign = ContentAlignment.MiddleLeft;
        left.Controls.AddRange(new Control[] { _hostBtn, _guestBtn, _relayAddr, _hostInfo });

        Caption("MOT DE PASSE DE LA PARTIE", left, S(24), S(182));
        _password.SetBounds(S(22), S(204), S(516), S(38));
        left.Controls.Add(_password);

        _play.Font = Theme.Font(14f, FontStyle.Bold);
        _play.SetBounds(S(22), S(260), S(250), S(56));
        _install.SetBounds(S(284), S(260), S(254), S(56));
        left.Controls.AddRange(new Control[] { _play, _install });

        _bar.SetBounds(S(22), S(326), S(516), S(4));
        _lastLine.SetBounds(S(22), S(332), S(516), S(18));
        _lastLine.BackColor = left.BackColor;
        _lastLine.Font = Theme.Font(8.5f);
        left.Controls.AddRange(new Control[] { _bar, _lastLine });

        var right = new Card();
        right.SetBounds(S(600), S(8), S(316), S(352));
        _mainPage.Controls.Add(right);
        Caption("CE QUE PARTAGE LE MOD", right, S(18), S(16));
        _carousel.BackColor = right.BackColor;
        _carousel.SetBounds(S(16), S(42), S(284), S(296));
        right.Controls.Add(_carousel);
    }

    void BuildSettingsPage()
    {
        _settingsPage.BackColor = Theme.Bg;
        _settingsPage.SetBounds(0, S(232), ClientSize.Width, S(372));
        Controls.Add(_settingsPage);
        var card = new Card();
        card.SetBounds(S(24), S(8), S(892), S(352));
        _settingsPage.Controls.Add(card);

        var title = new Label
        {
            Text = "Paramètres", Font = Theme.Font(15f, FontStyle.Bold), ForeColor = Theme.Text,
            BackColor = card.BackColor, AutoSize = false,
        };
        title.SetBounds(S(22), S(16), S(400), S(32));
        card.Controls.Add(title);
        var hint = new Label
        {
            Text = "Vos fichiers restent sur ce PC : le mod est fabriqué à partir de votre propre copie du jeu. Ryujinx doit déjà avoir vos clés (prod.keys).",
            ForeColor = Theme.Muted, BackColor = card.BackColor, AutoSize = false, Font = Theme.Font(9.5f),
        };
        hint.SetBounds(S(24), S(50), S(840), S(22));
        card.Controls.Add(hint);

        int y = S(86);
        AddPathRow(card, "RYUJINX", _ryujinx, "Ryujinx|Ryujinx.exe;Ryujinx.Ava.exe|Programme|*.exe", OnRyujinxPicked, ref y);
        AddPathRow(card, "JEU DE BASE", _base, "BOTW (.nsp)|*.nsp", () => { ReadForm(); LoadGameIconAsync(); }, ref y);
        AddPathRow(card, "MISE À JOUR 1.9.0", _update, "Mise à jour (.nsp)|*.nsp", null, ref y);

        _fixCfg.BackColor = card.BackColor;
        _fixCfg.FlatAppearance.BorderColor = Theme.Border;
        _fixCfg.FlatAppearance.CheckedBackColor = Theme.Surface2;
        _fixCfg.Font = Theme.Font(9.5f);
        _fixCfg.Location = new Point(S(24), y + S(4));
        card.Controls.Add(_fixCfg);

        _back.SetBounds(S(892 - 22 - 180), S(352 - 22 - 46), S(180), S(46));
        card.Controls.Add(_back);
    }

    void AddPathRow(Control parent, string caption, InputBox box, string filter, Action after, ref int y)
    {
        Caption(caption, parent, S(24), y);
        box.SetBounds(S(22), y + S(22), S(720), S(38));
        var browse = new FlatButton { Text = "Parcourir…" };
        browse.SetBounds(S(752), y + S(22), S(116), S(38));
        browse.Click += (_, _) =>
        {
            using var dlg = new OpenFileDialog { Filter = filter, CheckFileExists = true };
            if (File.Exists(box.Text))
                dlg.InitialDirectory = Path.GetDirectoryName(box.Text);
            if (dlg.ShowDialog(this) == DialogResult.OK)
            {
                box.Text = dlg.FileName;
                after?.Invoke();
            }
        };
        parent.Controls.Add(box);
        parent.Controls.Add(browse);
        y += S(70);
    }

    void BuildLogPage()
    {
        _logPage.BackColor = Theme.Bg;
        _logPage.SetBounds(0, S(232), ClientSize.Width, S(372));
        Controls.Add(_logPage);
        var card = new Card();
        card.SetBounds(S(24), S(8), S(892), S(352));
        _logPage.Controls.Add(card);
        Caption("JOURNAL", card, S(22), S(14));
        _log.SetBounds(S(22), S(40), S(848), S(296));
        card.Controls.Add(_log);
    }

    void BuildFooter()
    {
        var y = S(612);
        _settingsBtn.SetBounds(S(20), y, S(140), S(34));
        _logBtn.SetBounds(S(164), y, S(120), S(34));
        _uninstall.SetBounds(S(288), y, S(130), S(34));
        _versionLabel.SetBounds(S(440), y, S(476), S(34));
        _versionLabel.Font = Theme.Font(9f);
        _versionLabel.BackColor = Theme.Bg;
        Controls.AddRange(new Control[] { _settingsBtn, _logBtn, _uninstall, _versionLabel });
    }

    // ---- Etat ----

    bool IsConfigured() => File.Exists(_s.RyujinxExe) && File.Exists(_s.BaseNsp) && File.Exists(_s.UpdateNsp);

    void ShowPage(Panel page)
    {
        foreach (var p in new[] { _mainPage, _settingsPage, _logPage })
            p.Visible = p == page;
        _settingsBtn.Text = page == _settingsPage ? "←  Retour" : "⚙  Paramètres";
        _logBtn.Text = page == _logPage ? "←  Retour" : "☰  Journal";
    }

    void SetRole(bool host)
    {
        _hostBtn.Selected = host;
        _guestBtn.Selected = !host;
        _hostBtn.Invalidate();
        _guestBtn.Invalidate();
        _relayAddr.Visible = !host;
        _hostInfo.Visible = host;
        var ip = Setup.LanAddress();
        _hostInfo.Text = ip == null
            ? "Aucun réseau local détecté : l'autre joueur ne pourra pas vous rejoindre."
            : $"Votre adresse à donner à l'autre joueur :  {ip}";
    }

    void LoadForm()
    {
        _ryujinx.Text = _s.RyujinxExe;
        _base.Text = _s.BaseNsp;
        _update.Text = _s.UpdateNsp;
        _relayAddr.Text = _s.RelayAddress;
        _password.Text = _s.Password;
        _fixCfg.Checked = _s.FixIgnoreMissingServices;
        SetRole(_s.Host);
    }

    void ReadForm()
    {
        _s.RyujinxExe = _ryujinx.Text.Trim().Trim('"');
        _s.BaseNsp = _base.Text.Trim().Trim('"');
        _s.UpdateNsp = _update.Text.Trim().Trim('"');
        _s.Host = _hostBtn.Selected;
        _s.RelayAddress = _relayAddr.Text.Trim();
        _s.Password = _password.Text.Trim();
        _s.FixIgnoreMissingServices = _fixCfg.Checked;
    }

    /// <summary>Ryujinx choisi : on propose la mise a jour deja selectionnee dans Ryujinx, s'il y en a une.</summary>
    void OnRyujinxPicked()
    {
        ReadForm();
        try
        {
            var upd = Path.Combine(_s.RyujinxDataDir(_dataDirOverride), "games", Setup.TitleId, "updates.json");
            if (string.IsNullOrEmpty(_update.Text) && File.Exists(upd))
            {
                var sel = (string)JsonNode.Parse(File.ReadAllText(upd))?["selected"];
                if (!string.IsNullOrEmpty(sel) && File.Exists(sel))
                    _update.Text = sel;
            }
        }
        catch (Exception)
        {
            // Lecture facultative.
        }
    }

    void Log(string line)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Log(line));
            return;
        }
        _log.AppendText(line + Environment.NewLine);
        var t = line.Trim();
        if (t.Length > 0 && !t.StartsWith("Téléchargement : http", StringComparison.Ordinal))
            _lastLine.Text = t;
    }

    void Progress(int value)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Progress(value));
            return;
        }
        _bar.Value = value;
    }

    Setup NewSetup() => new(Log, Progress);

    void RefreshStatus()
    {
        var installed = _s.InstalledVersion;
        if (string.IsNullOrEmpty(installed))
        {
            _status.Dot = Theme.Muted;
            _status.Text = IsConfigured() ? "Mod non installé" : "À configurer : indiquez vos fichiers dans les paramètres";
        }
        else if (_latest != null && _latest.Tag != installed)
        {
            _status.Dot = Theme.Warn;
            _status.Text = $"Mod {installed} · nouvelle version {_latest.Tag} installée au clic sur Jouer";
        }
        else
        {
            _status.Dot = Theme.Ok;
            _status.Text = _latest == null ? $"Mod installé · {installed}" : $"Mod installé · {installed} · à jour";
        }
        _status.Invalidate();
        _install.Text = string.IsNullOrEmpty(installed) ? "Installer le mod" : "Réinstaller / Mettre à jour";
        _versionLabel.Text = _latest == null ? "" : $"Dernière version publiée : {_latest.Tag}";
    }

    async Task OnShownAsync()
    {
        LoadGameIconAsync();
        if (!string.IsNullOrEmpty(_localZip))
            Log($"Archive locale : {_localZip}");
        else
        {
            try
            {
                _latest = await NewSetup().GetLatestReleaseAsync();
                if (_latest == null)
                    Log("Aucune version publiée pour l'instant.");
                else if (await NewSetup().SelfUpdateAsync(_latest))
                {
                    Close();
                    return;
                }
                else if (!string.IsNullOrEmpty(_s.InstalledVersion) && _s.InstalledVersion != _latest.Tag)
                    Log($"Nouvelle version du mod : {_latest.Tag}. Elle sera installée au clic sur « Jouer ».");
            }
            catch (Exception e)
            {
                Log("Vérification des mises à jour impossible : " + e.Message);
            }
        }
        RefreshStatus();
        await LoadCapturesAsync();
    }

    /// <summary>Icone du jeu, lue dans le .nsp de l'utilisateur (une fois, puis gardee en cache).</summary>
    async void LoadGameIconAsync()
    {
        if (_hero.Icon != null || !File.Exists(_s.BaseNsp))
            return;
        var keys = Path.Combine(_s.RyujinxDataDir(_dataDirOverride), "system", "prod.keys");
        var nsp = _s.BaseNsp;
        try
        {
            var path = await Task.Run(() => Setup.EnsureGameIcon(keys, nsp));
            if (path != null && _hero.Icon == null)
                _hero.Icon = LoadImage(path);
        }
        catch (Exception e)
        {
            Log("Icône du jeu non lue : " + e.Message);
        }
    }

    /// <summary>Captures du README (deja publiees dans le depot), a la version de la derniere release.</summary>
    async Task LoadCapturesAsync()
    {
        var items = new (string File, string Caption)[]
        {
            ("2026-11-13_pantin-link_J2-voit-J1-grimper.png", "L'autre joueur est un Link animé"),
            ("2026-11-14_equipement_deux-instances_J2-voit-J1-equipe.png", "Il porte son armure et ses armes"),
            ("2026-11-15_ennemi-anime-par-J1_vu-par-J2_eb3.png", "Les ennemis sont partagés"),
            ("2026-11-15_teinture-vue-par-J2_zoom_dy5.png", "Les teintures aussi"),
            ("2026-11-11_phaseC_J1-ramasse-le-champignon.png", "Un objet ramassé disparaît chez l'autre"),
        };
        var gitRef = _latest?.Tag ?? "main";
        foreach (var (file, caption) in items)
        {
            var path = await Setup.CachedRepoImageAsync(file, gitRef) ?? await Setup.CachedRepoImageAsync(file, "main");
            if (path == null)
                continue;
            try
            {
                _carousel.Add(CropCapture(LoadImage(path)), caption);
            }
            catch (Exception)
            {
                // Image illisible : ignoree.
            }
        }
    }

    /// <summary>Image chargee sans garder le fichier ouvert.</summary>
    static Image LoadImage(string path)
    {
        using var ms = new MemoryStream(File.ReadAllBytes(path));
        using var img = Image.FromStream(ms);
        return new Bitmap(img);
    }

    /// <summary>Captures de fenetre Ryujinx 960x630 : on retire la barre de titre et la barre d'etat.</summary>
    static Image CropCapture(Image img)
    {
        if (img.Width != 960 || img.Height != 630)
            return img;
        var crop = new Bitmap(944, 532);
        using (var g = Graphics.FromImage(crop))
            g.DrawImage(img, new Rectangle(0, 0, 944, 532), new Rectangle(8, 52, 944, 532), GraphicsUnit.Pixel);
        img.Dispose();
        return crop;
    }

    async Task Busy(Func<Task> work)
    {
        foreach (var b in _busyButtons)
            b.Enabled = false;
        try
        {
            await work();
        }
        catch (Exception e)
        {
            Log("ERREUR : " + e.Message);
            MessageBox.Show(this, e.Message, "BOTW Coop", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            foreach (var b in _busyButtons)
                b.Enabled = true;
            RefreshStatus();
        }
    }

    async Task InstallAsync()
    {
        ReadForm();
        _s.Save();
        if (!IsConfigured())
        {
            ShowPage(_settingsPage);
            throw new InvalidOperationException("Indiquez d'abord Ryujinx.exe, le jeu de base et la mise à jour 1.9.0.");
        }
        CheckPartyInputs();
        await Task.Run(() => NewSetup().InstallAsync(new Setup.Options
        {
            Settings = _s, DataDirOverride = _dataDirOverride, LocalModZip = _localZip, Release = _latest,
        }));
        Log("Installation terminée.");
    }

    void CheckPartyInputs()
    {
        if (!_s.Host && string.IsNullOrEmpty(_s.RelayAddress))
            throw new InvalidOperationException("Indiquez l'adresse de l'ami qui héberge (elle s'affiche dans son launcher quand il choisit « J'héberge »).");
        if (_s.Password.Length > 63 || _s.Password.Any(c => c <= ' ' || c > '~'))
            throw new InvalidOperationException("Mot de passe : 63 caractères au plus, sans espace ni accent.");
    }

    /// <summary>
    /// Jouer : installe d'abord le mod (premiere fois) ou la derniere version s'il y en a une (les deux
    /// joueurs doivent avoir la meme), recrit la partie (hote / invite, mot de passe), puis lance le
    /// relais (hote) et Ryujinx.
    /// </summary>
    async Task PlayAsync()
    {
        ReadForm();
        _s.Save();
        if (string.IsNullOrEmpty(_s.InstalledVersion))
        {
            await InstallAsync();
        }
        else
        {
            CheckPartyInputs();
            var options = new Setup.Options { Settings = _s, DataDirOverride = _dataDirOverride, LocalModZip = _localZip };
            var updated = await Task.Run(() => NewSetup().UpdateIfNeededAsync(options));
            if (updated)
            {
                // Le relais en cours vient de l'ancienne version : on le relance.
                StopRelay();
                _relay = null;
            }
            else
            {
                // Role ou mot de passe changes depuis l'installation : on reecrit la configuration du mod.
                NewSetup().ApplyPartySettings(_s, _dataDirOverride);
            }
        }
        if (_s.Host && (_relay == null || _relay.HasExited))
            _relay = NewSetup().StartRelay(_s);
        NewSetup().StartGame(_s);
        Progress(0);
    }

    void Uninstall()
    {
        ReadForm();
        var target = Setup.ModTarget(_s.RyujinxDataDir(_dataDirOverride));
        if (MessageBox.Show(this, $"Supprimer définitivement le dossier du mod ?\n\n{target}\n\nVos sauvegardes normales ne sont pas touchées.",
                "BOTW Coop", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;
        try
        {
            NewSetup().Uninstall(_s, _dataDirOverride);
        }
        catch (Exception e)
        {
            Log("ERREUR : " + e.Message);
        }
        RefreshStatus();
    }

    void StopRelay()
    {
        try
        {
            if (_relay != null && !_relay.HasExited)
                _relay.Kill();
        }
        catch (Exception)
        {
            // Deja arrete.
        }
    }

    static void CleanOldExe()
    {
        try
        {
            var old = Environment.ProcessPath + ".old";
            if (File.Exists(old))
                File.Delete(old);
        }
        catch (Exception)
        {
            // L'ancienne version tourne encore : on reessaiera au prochain lancement.
        }
    }

    /// <summary>Essai : rend la fenetre dans un PNG apres le chargement (option --screenshot).</summary>
    public async Task SaveScreenshotAsync(string path, int delayMs, string page)
    {
        await Task.Delay(delayMs);
        if (page == "settings")
            ShowPage(_settingsPage);
        else if (page == "log")
            ShowPage(_logPage);
        await Task.Delay(300);
        using var bmp = new Bitmap(Width, Height);
        DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        Close();
    }
}
