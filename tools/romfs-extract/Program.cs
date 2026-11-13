// Copie des fichiers de la RomFS de BOTW 1.9.0 (base + mise a jour appliquee) vers un dossier local.
//
//   RomfsExtract <prod.keys> <base.nsp> <maj.nsp> <dossier de sortie> <prefixe> [<prefixe> ...]
//   RomfsExtract <prod.keys> <base.nsp> <maj.nsp> --list <filtre>
//
// Les cles et les NSP sont ceux de l'utilisateur, ouverts en lecture seule. Les cles de titre
// viennent des tickets contenus dans les NSP. La mise a jour s'applique comme dans Ryujinx
// (Nca.OpenStorageWithPatch). Les chemins de sortie gardent l'arborescence de la RomFS.

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

if (args.Length < 5)
{
    Console.Error.WriteLine("usage : RomfsExtract <prod.keys> <base.nsp> <maj.nsp> <sortie> <prefixe>... | --list <filtre>");
    return 2;
}

var keys = new KeySet();
ExternalKeyReader.ReadKeyFile(keys, args[0], null, null, null, null);

Nca program = ProgramNca(args[1], keys);
Nca update = ProgramNca(args[2], keys);
var romfs = new RomFsFileSystem(program.OpenStorageWithPatch(update, NcaSectionType.Data, IntegrityCheckLevel.None));

if (args[3] == "--list")
{
    foreach (DirectoryEntryEx e in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        if (e.Type == DirectoryEntryType.File && e.FullPath.Contains(args[4], StringComparison.OrdinalIgnoreCase))
            Console.WriteLine($"{e.Size,12} {e.FullPath}");
    return 0;
}

string outDir = args[3];
var prefixes = args.Skip(4).Select(p => p.StartsWith('/') ? p : "/" + p).ToArray();
int count = 0;
long total = 0;
foreach (DirectoryEntryEx e in romfs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
{
    if (e.Type != DirectoryEntryType.File || !prefixes.Any(p => e.FullPath.StartsWith(p, StringComparison.Ordinal)))
        continue;
    string dest = Path.Combine(outDir, e.FullPath.TrimStart('/').Replace('/', Path.DirectorySeparatorChar));
    Directory.CreateDirectory(Path.GetDirectoryName(dest));
    using var file = new UniqueRef<IFile>();
    romfs.OpenFile(ref file.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
    using var output = File.Create(dest);
    var buffer = new byte[1 << 22];
    long at = 0;
    while (at < e.Size)
    {
        file.Get.Read(out long got, at, buffer.AsSpan(0, (int)Math.Min(buffer.Length, e.Size - at))).ThrowIfFailure();
        if (got <= 0)
            throw new IOException($"{e.FullPath} : lecture arretee a {at} sur {e.Size}");
        output.Write(buffer, 0, (int)got);
        at += got;
    }
    count++;
    total += e.Size;
}
Console.WriteLine($"{count} fichiers, {total} octets -> {outDir}");
return count > 0 ? 0 : 1;

// NCA de type Program d'un NSP ; les cles de titre des tickets du NSP sont ajoutees au jeu de cles.
static Nca ProgramNca(string nsp, KeySet keys)
{
    var pfs = new PartitionFileSystem();
    pfs.Initialize(new LocalStorage(nsp, FileAccess.Read, FileMode.Open)).ThrowIfFailure();
    foreach (DirectoryEntryEx t in pfs.EnumerateEntries("/", "*.tik"))
    {
        using var tik = new UniqueRef<IFile>();
        if (pfs.OpenFile(ref tik.Ref, t.FullPath.ToU8Span(), OpenMode.Read).IsFailure())
            continue;
        var ticket = new Ticket(tik.Get.AsStream());
        byte[] titleKey = ticket.GetTitleKey(keys);
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
    throw new InvalidOperationException($"pas de NCA Program dans {nsp}");
}
