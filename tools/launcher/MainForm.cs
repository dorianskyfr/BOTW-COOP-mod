using System.Diagnostics;
using System.Text.Json.Nodes;

namespace BotwCoopLauncher;

/// <summary>Fenetre unique : chemins, role (hote / invite), installation, mise a jour, lancement.</summary>
public sealed class MainForm : Form
{
    readonly Settings _s = Settings.Load();
    readonly string _dataDirOverride;
    readonly string _localZip;
    Release _latest;
    Process _relay;

    readonly TextBox _ryujinx = new() { Width = 430 };
    readonly TextBox _base = new() { Width = 430 };
    readonly TextBox _update = new() { Width = 430 };
    readonly RadioButton _host = new() { Text = "J'héberge la partie", AutoSize = true };
    readonly RadioButton _guest = new() { Text = "Je rejoins un ami :", AutoSize = true };
    readonly TextBox _relayAddr = new() { Width = 170, PlaceholderText = "192.168.1.20" };
    readonly TextBox _password = new() { Width = 170, PlaceholderText = "facultatif" };
    readonly CheckBox _fixCfg = new() { Text = "Activer « Ignorer les services manquants » dans Ryujinx (nécessaire)", AutoSize = true };
    readonly Label _status = new() { AutoSize = true, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold) };
    readonly ProgressBar _bar = new() { Width = 560, Height = 14 };
    readonly TextBox _log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Width = 560, Height = 190, Font = new Font(FontFamily.GenericMonospace, 8.5f) };
    readonly Button _install = new() { Text = "Installer / Mettre à jour", Width = 180, Height = 34 };
    readonly Button _play = new() { Text = "Jouer", Width = 120, Height = 34 };
    readonly Button _uninstall = new() { Text = "Désinstaller", Width = 110, Height = 34 };

    public MainForm(string dataDirOverride, string localZip)
    {
        _dataDirOverride = dataDirOverride;
        _localZip = localZip;
        Text = $"BOTW Coop — launcher {Setup.LauncherVersion.ToString(3)}";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(12);

        var root = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false };
        root.Controls.Add(new Label { Text = "1. Vos fichiers (rien n'est envoyé : le mod est fabriqué à partir de votre jeu, sur ce PC)", AutoSize = true });
        root.Controls.Add(Row("Ryujinx.exe", _ryujinx, "Ryujinx|Ryujinx.exe;Ryujinx.Ava.exe|Programme|*.exe", OnRyujinxPicked));
        root.Controls.Add(Row("Jeu de base (.nsp)", _base, "BOTW (.nsp)|*.nsp", null));
        root.Controls.Add(Row("Mise à jour 1.9.0 (.nsp)", _update, "Mise à jour (.nsp)|*.nsp", null));
        root.Controls.Add(new Label { Text = "2. Partie", AutoSize = true, Margin = new Padding(0, 10, 0, 0) });
        var role = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        role.Controls.Add(_host);
        role.Controls.Add(_guest);
        role.Controls.Add(_relayAddr);
        root.Controls.Add(role);
        var pw = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        pw.Controls.Add(new Label { Text = "Mot de passe de la partie :", AutoSize = true, Margin = new Padding(3, 6, 3, 0) });
        pw.Controls.Add(_password);
        root.Controls.Add(pw);
        root.Controls.Add(_fixCfg);
        var buttons = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 10, 0, 4) };
        buttons.Controls.Add(_install);
        buttons.Controls.Add(_play);
        buttons.Controls.Add(_uninstall);
        root.Controls.Add(buttons);
        root.Controls.Add(_status);
        root.Controls.Add(_bar);
        root.Controls.Add(_log);
        Controls.Add(root);

        _ryujinx.Text = _s.RyujinxExe;
        _base.Text = _s.BaseNsp;
        _update.Text = _s.UpdateNsp;
        _host.Checked = _s.Host;
        _guest.Checked = !_s.Host;
        _relayAddr.Text = _s.RelayAddress;
        _password.Text = _s.Password;
        _fixCfg.Checked = _s.FixIgnoreMissingServices;
        _relayAddr.Enabled = _guest.Checked;
        _guest.CheckedChanged += (_, _) => _relayAddr.Enabled = _guest.Checked;

        _install.Click += async (_, _) => await Busy(InstallAsync);
        _play.Click += (_, _) => Play();
        _uninstall.Click += (_, _) => Uninstall();
        Shown += async (_, _) => await CheckUpdatesAsync();
        FormClosing += (_, _) => StopRelay();

        CleanOldExe();
        RefreshStatus();
    }

    Control Row(string label, TextBox box, string filter, Action after)
    {
        var p = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        p.Controls.Add(new Label { Text = label, Width = 150, Margin = new Padding(3, 6, 3, 0) });
        p.Controls.Add(box);
        var b = new Button { Text = "…", Width = 32 };
        b.Click += (_, _) =>
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
        p.Controls.Add(b);
        return p;
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

    void ReadForm()
    {
        _s.RyujinxExe = _ryujinx.Text.Trim().Trim('"');
        _s.BaseNsp = _base.Text.Trim().Trim('"');
        _s.UpdateNsp = _update.Text.Trim().Trim('"');
        _s.Host = _host.Checked;
        _s.RelayAddress = _relayAddr.Text.Trim();
        _s.Password = _password.Text.Trim();
        _s.FixIgnoreMissingServices = _fixCfg.Checked;
    }

    void Log(string line)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Log(line));
            return;
        }
        _log.AppendText(line + Environment.NewLine);
    }

    void Progress(int value)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Progress(value));
            return;
        }
        _bar.Value = Math.Clamp(value, 0, 100);
    }

    Setup NewSetup() => new(Log, Progress);

    void RefreshStatus()
    {
        var installed = string.IsNullOrEmpty(_s.InstalledVersion) ? "non installé" : $"installé ({_s.InstalledVersion})";
        var latest = _latest == null ? "" : $" — dernière version : {_latest.Tag}";
        _status.Text = $"Mod : {installed}{latest}";
    }

    async Task Busy(Func<Task> work)
    {
        foreach (var b in new[] { _install, _play, _uninstall })
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
            foreach (var b in new[] { _install, _play, _uninstall })
                b.Enabled = true;
            RefreshStatus();
        }
    }

    async Task CheckUpdatesAsync()
    {
        if (!string.IsNullOrEmpty(_localZip))
        {
            Log($"Archive locale : {_localZip}");
            return;
        }
        try
        {
            _latest = await NewSetup().GetLatestReleaseAsync();
            if (_latest == null)
            {
                Log("Aucune version publiée pour l'instant.");
                return;
            }
            if (await NewSetup().SelfUpdateAsync(_latest))
            {
                Close();
                return;
            }
            if (!string.IsNullOrEmpty(_s.InstalledVersion) && _s.InstalledVersion != _latest.Tag)
                Log($"Nouvelle version du mod : {_latest.Tag}. Cliquez sur « Installer / Mettre à jour ».");
        }
        catch (Exception e)
        {
            Log("Vérification des mises à jour impossible : " + e.Message);
        }
        RefreshStatus();
    }

    async Task InstallAsync()
    {
        ReadForm();
        _s.Save();
        if (!_s.Host && string.IsNullOrEmpty(_s.RelayAddress))
            throw new InvalidOperationException("Indiquez l'adresse de l'ami qui héberge (elle s'affiche dans son launcher quand il clique sur « Jouer »).");
        if (_s.Password.Length > 63 || _s.Password.Any(c => c <= ' ' || c > '~'))
            throw new InvalidOperationException("Mot de passe : 63 caractères au plus, sans espace ni accent.");
        await Task.Run(() => NewSetup().InstallAsync(new Setup.Options
        {
            Settings = _s, DataDirOverride = _dataDirOverride, LocalModZip = _localZip, Release = _latest,
        }));
        Log("Installation terminée.");
    }

    void Play()
    {
        try
        {
            ReadForm();
            _s.Save();
            if (string.IsNullOrEmpty(_s.InstalledVersion))
                throw new InvalidOperationException("Installez d'abord le mod.");
            if (_s.Host && (_relay == null || _relay.HasExited))
                _relay = NewSetup().StartRelay(_s);
            NewSetup().StartGame(_s);
        }
        catch (Exception e)
        {
            Log("ERREUR : " + e.Message);
            MessageBox.Show(this, e.Message, "BOTW Coop", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
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
}
