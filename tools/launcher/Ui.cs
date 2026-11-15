using System.ComponentModel;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;

namespace BotwCoopLauncher;

/// <summary>Couleurs et polices du launcher (theme sombre).</summary>
static class Theme
{
    public static readonly Color Bg = Color.FromArgb(13, 16, 21);
    public static readonly Color Surface = Color.FromArgb(22, 27, 34);
    public static readonly Color Surface2 = Color.FromArgb(32, 39, 49);
    public static readonly Color Border = Color.FromArgb(46, 56, 70);
    public static readonly Color Text = Color.FromArgb(232, 237, 242);
    public static readonly Color Muted = Color.FromArgb(140, 152, 166);
    public static readonly Color Accent = Color.FromArgb(232, 176, 75);
    public static readonly Color AccentHover = Color.FromArgb(244, 198, 110);
    public static readonly Color AccentText = Color.FromArgb(30, 22, 8);
    public static readonly Color Sheikah = Color.FromArgb(64, 190, 232);
    public static readonly Color Ok = Color.FromArgb(92, 204, 128);
    public static readonly Color Warn = Color.FromArgb(240, 180, 70);
    public static readonly Color Danger = Color.FromArgb(232, 92, 84);

    public static Font Font(float size, FontStyle style = FontStyle.Regular, bool semibold = false) =>
        new(semibold ? "Segoe UI Semibold" : "Segoe UI", size, style, GraphicsUnit.Point);

    public static GraphicsPath Round(RectangleF r, float radius)
    {
        var p = new GraphicsPath();
        var d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
        if (d <= 0.5f)
        {
            p.AddRectangle(r);
            return p;
        }
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    public static void Smooth(Graphics g)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
    }

    /// <summary>Barre de titre sombre (Windows 10 2004+ / 11 ; ignore ailleurs).</summary>
    public static void DarkTitleBar(IntPtr hwnd)
    {
        try
        {
            int on = 1;
            DwmSetWindowAttribute(hwnd, 20, ref on, sizeof(int));
        }
        catch (Exception)
        {
            // Pas de DWM : barre de titre normale.
        }
    }

    [DllImport("dwmapi.dll")]
    static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
}

/// <summary>Controle dessine par nos soins, double tampon.</summary>
abstract class Painted : Control
{
    protected Painted()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer
                 | ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
    }
}

/// <summary>Bouton plat arrondi : principal (or), secondaire, discret ou danger.</summary>
sealed class FlatButton : Painted
{
    public enum Kinds { Primary, Secondary, Ghost, DangerGhost, Segment }
    public Kinds Kind = Kinds.Secondary;
    public bool Selected;
    public float Radius = 8;
    bool _hover, _down;

    public FlatButton()
    {
        Cursor = Cursors.Hand;
        Font = Theme.Font(10f, semibold: true);
    }

    protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hover = _down = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { _down = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { _down = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        Color fill, text, border = Color.Empty;
        switch (Kind)
        {
            case Kinds.Primary:
                fill = _hover ? Theme.AccentHover : Theme.Accent;
                text = Theme.AccentText;
                break;
            case Kinds.Segment:
                fill = Selected ? Theme.Surface2 : (_hover ? Color.FromArgb(26, 32, 41) : Theme.Surface);
                text = Selected ? Theme.Text : Theme.Muted;
                border = Selected ? Theme.Sheikah : Theme.Border;
                break;
            case Kinds.Ghost:
                fill = _hover ? Theme.Surface2 : Color.Empty;
                text = _hover ? Theme.Text : Theme.Muted;
                break;
            case Kinds.DangerGhost:
                fill = _hover ? Color.FromArgb(48, 24, 26) : Color.Empty;
                text = _hover ? Theme.Danger : Color.FromArgb(180, 110, 105);
                break;
            default:
                fill = _hover ? Color.FromArgb(42, 51, 64) : Theme.Surface2;
                text = Theme.Text;
                border = Theme.Border;
                break;
        }
        if (_down && fill != Color.Empty)
            fill = ControlPaint.Dark(fill, 0.05f);
        if (!Enabled)
        {
            fill = fill == Color.Empty ? fill : Color.FromArgb(120, fill);
            text = Color.FromArgb(110, text);
        }
        var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
        using var path = Theme.Round(r, Radius);
        if (fill != Color.Empty)
        {
            using var b = new SolidBrush(fill);
            g.FillPath(b, path);
        }
        if (border != Color.Empty)
        {
            using var pen = new Pen(Enabled ? border : Color.FromArgb(90, border), Kind == Kinds.Segment && Selected ? 1.6f : 1f);
            g.DrawPath(pen, path);
        }
        TextRenderer.DrawText(g, Text, Font, ClientRectangle, text,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
    }
}

/// <summary>Champ de saisie sombre a bord arrondi (TextBox sans bord a l'interieur).</summary>
sealed class InputBox : Painted
{
    public readonly TextBox Box = new()
    {
        BorderStyle = BorderStyle.None, BackColor = Theme.Surface2, ForeColor = Theme.Text, Font = Theme.Font(10.5f),
    };

    public InputBox()
    {
        Controls.Add(Box);
        Box.GotFocus += (_, _) => Invalidate();
        Box.LostFocus += (_, _) => Invalidate();
        Cursor = Cursors.IBeam;
        Click += (_, _) => Box.Focus();
    }

    public override string Text { get => Box.Text; set => Box.Text = value; }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string Placeholder { get => Box.PlaceholderText; set => Box.PlaceholderText = value; }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        var h = Box.PreferredHeight;
        Box.SetBounds(12, Math.Max(0, (Height - h) / 2), Math.Max(10, Width - 24), h);
    }

    protected override void OnEnabledChanged(EventArgs e)
    {
        base.OnEnabledChanged(e);
        Box.ForeColor = Enabled ? Theme.Text : Theme.Muted;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        using var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 8);
        using var b = new SolidBrush(Theme.Surface2);
        g.FillPath(b, path);
        using var pen = new Pen(Box.Focused ? Theme.Sheikah : Theme.Border, Box.Focused ? 1.6f : 1f);
        g.DrawPath(pen, path);
    }
}

/// <summary>Panneau arrondi (carte) ; ses enfants prennent sa couleur de fond.</summary>
sealed class Card : Panel
{
    public Card()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Surface;
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        using var path = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 14);
        using var b = new SolidBrush(Theme.Surface);
        g.FillPath(b, path);
        using var pen = new Pen(Color.FromArgb(34, 42, 54));
        g.DrawPath(pen, path);
    }
}

/// <summary>Barre de progression fine.</summary>
sealed class ThinProgress : Painted
{
    int _value;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int Value { get => _value; set { _value = Math.Clamp(value, 0, 100); Invalidate(); } }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        using (var track = Theme.Round(new RectangleF(0, 0, Width, Height), Height / 2f))
        using (var b = new SolidBrush(Theme.Surface2))
            g.FillPath(b, track);
        if (_value <= 0)
            return;
        var w = Math.Max(Height, Width * _value / 100f);
        using var fill = Theme.Round(new RectangleF(0, 0, w, Height), Height / 2f);
        using var lg = new LinearGradientBrush(new RectangleF(0, 0, Math.Max(1, w), Height), Theme.Sheikah, Theme.Accent, LinearGradientMode.Horizontal);
        g.FillPath(lg, fill);
    }
}

/// <summary>Pastille de couleur + texte (etat du mod).</summary>
sealed class StatusLine : Painted
{
    public Color Dot = Theme.Muted;

    public StatusLine() => Font = Theme.Font(10.5f, semibold: true);

    protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Bg);
        var d = Height * 0.36f;
        var y = (Height - d) / 2;
        using (var glow = new SolidBrush(Color.FromArgb(50, Dot)))
            g.FillEllipse(glow, 0, y - d * 0.35f, d * 1.7f, d * 1.7f);
        using (var b = new SolidBrush(Dot))
            g.FillEllipse(b, d * 0.35f, y, d, d);
        var r = new Rectangle((int)(d * 2.4f), 0, Width - (int)(d * 2.4f), Height);
        TextRenderer.DrawText(g, Text, Font, r, Theme.Text, TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
    }
}

/// <summary>
/// Bandeau du haut : l'icone du jeu (lue dans le .nsp de l'utilisateur) en fond flou et en vignette,
/// titre, sous-titre et pastille de version.
/// </summary>
sealed class Hero : Painted
{
    Image _icon;
    Bitmap _blur;
    public string Title = "BOTW Coop";
    public string Subtitle = "";
    public string Badge = "";

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Image Icon
    {
        get => _icon;
        set
        {
            _icon = value;
            _blur?.Dispose();
            _blur = null;
            if (value != null)
            {
                // Flou bon marche : reduction a 20 px puis agrandissement lisse au dessin.
                _blur = new Bitmap(20, 20);
                using var g = Graphics.FromImage(_blur);
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(value, 0, 0, 20, 20);
            }
            Invalidate();
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        var full = new Rectangle(0, 0, Width, Height);
        if (_blur != null)
        {
            // Image en « couverture » : la largeur remplit le bandeau, centree verticalement.
            var side = Math.Max(Width, Height);
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            g.DrawImage(_blur, new Rectangle(-side / 20, (Height - side) / 2 - side / 20, side + side / 10, side + side / 10));
            using var shade = new SolidBrush(Color.FromArgb(85, Theme.Bg));
            g.FillRectangle(shade, full);
        }
        else
        {
            using var lg = new LinearGradientBrush(full, Color.FromArgb(18, 44, 56), Theme.Bg, LinearGradientMode.ForwardDiagonal);
            g.FillRectangle(lg, full);
        }
        using (var fade = new LinearGradientBrush(new Rectangle(0, Height / 3, Width, Height - Height / 3 + 1),
                   Color.FromArgb(0, Theme.Bg), Theme.Bg, LinearGradientMode.Vertical))
            g.FillRectangle(fade, 0, Height / 3, Width, Height - Height / 3 + 1);

        var pad = Height * 0.16f;
        var cardSize = Height * 0.58f;
        var card = new RectangleF(pad, Height - pad - cardSize, cardSize, cardSize);
        float textX;
        if (_icon != null)
        {
            using (var shadow = Theme.Round(new RectangleF(card.X + 2, card.Y + 6, card.Width, card.Height), 16))
            using (var sb = new SolidBrush(Color.FromArgb(90, 0, 0, 0)))
                g.FillPath(sb, shadow);
            using var clip = Theme.Round(card, 16);
            var state = g.Save();
            g.SetClip(clip);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(_icon, card);
            g.Restore(state);
            using var pen = new Pen(Color.FromArgb(70, 255, 255, 255), 1.2f);
            g.DrawPath(pen, clip);
            textX = card.Right + pad * 0.8f;
        }
        else
        {
            textX = pad;
        }

        using var titleFont = Theme.Font(30f, FontStyle.Bold);
        using var subFont = Theme.Font(11.5f);
        using var badgeFont = Theme.Font(9f, semibold: true);
        var titleSize = TextRenderer.MeasureText(g, Title, titleFont);
        var titleY = card.Y + card.Height * 0.12f;
        TextRenderer.DrawText(g, Title, titleFont, new Point((int)textX - 4, (int)titleY), Theme.Text);
        var subY = titleY + titleSize.Height + 2;
        TextRenderer.DrawText(g, Subtitle, subFont, new Point((int)textX, (int)subY), Color.FromArgb(200, 214, 226));
        if (!string.IsNullOrEmpty(Badge))
        {
            var bs = TextRenderer.MeasureText(g, Badge, badgeFont);
            var br = new RectangleF(textX, subY + TextRenderer.MeasureText(g, "Ag", subFont).Height + 12, bs.Width + 18, bs.Height + 8);
            using var bp = Theme.Round(br, br.Height / 2);
            using var bb = new SolidBrush(Color.FromArgb(60, Theme.Sheikah));
            g.FillPath(bb, bp);
            using var bpen = new Pen(Color.FromArgb(140, Theme.Sheikah));
            g.DrawPath(bpen, bp);
            TextRenderer.DrawText(g, Badge, badgeFont, Rectangle.Round(br), Color.FromArgb(200, 236, 250),
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _blur?.Dispose();
        base.Dispose(disposing);
    }
}

/// <summary>Carrousel de captures : une image, sa legende, des points ; change toutes les 5 s ou au clic.</summary>
sealed class Carousel : Painted
{
    readonly List<(Image Image, string Caption)> _items = new();
    readonly System.Windows.Forms.Timer _timer = new() { Interval = 5000 };
    int _index;
    public string Empty = "";

    public Carousel()
    {
        Cursor = Cursors.Hand;
        _timer.Tick += (_, _) => Next();
        _timer.Start();
        Click += (_, _) => Next();
    }

    public void Add(Image image, string caption)
    {
        _items.Add((image, caption));
        Invalidate();
    }

    void Next()
    {
        if (_items.Count > 1)
        {
            _index = (_index + 1) % _items.Count;
            Invalidate();
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Theme.Smooth(g);
        g.Clear(Parent?.BackColor ?? Theme.Surface);
        var captionH = 54;
        var area = new RectangleF(0, 0, Width, Height - captionH);
        using var clip = Theme.Round(area, 10);
        if (_items.Count == 0)
        {
            using var b = new SolidBrush(Theme.Surface2);
            g.FillPath(b, clip);
            using var f = Theme.Font(9.5f);
            TextRenderer.DrawText(g, Empty, f, Rectangle.Round(area), Theme.Muted,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak);
            return;
        }
        var (img, caption) = _items[_index % _items.Count];
        // Remplissage « couverture » de la zone.
        var scale = Math.Max(area.Width / img.Width, area.Height / img.Height);
        var w = img.Width * scale;
        var h = img.Height * scale;
        var state = g.Save();
        g.SetClip(clip);
        g.DrawImage(img, new RectangleF((area.Width - w) / 2, (area.Height - h) / 2, w, h));
        g.Restore(state);
        using (var pen = new Pen(Color.FromArgb(40, 255, 255, 255)))
            g.DrawPath(pen, clip);

        using var capFont = Theme.Font(10f, semibold: true);
        TextRenderer.DrawText(g, caption, capFont, new Rectangle(2, (int)area.Bottom + 8, Width - 4, 22), Theme.Text,
            TextFormatFlags.EndEllipsis);
        var dot = 7f;
        var gap = 6f;
        var x = 4f;
        for (int i = 0; i < _items.Count; i++)
        {
            using var b = new SolidBrush(i == _index % _items.Count ? Theme.Accent : Theme.Border);
            var dw = i == _index % _items.Count ? dot * 2.4f : dot;
            using var p = Theme.Round(new RectangleF(x, area.Bottom + 36, dw, dot), dot / 2);
            g.FillPath(b, p);
            x += dw + gap;
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Dispose();
            foreach (var (image, _) in _items)
                image.Dispose();
        }
        base.Dispose(disposing);
    }
}
