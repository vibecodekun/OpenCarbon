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

// usage: nspx <prod.keys> <title.keys> <outdir> <base.nsp> [update.nsp]
var keySet = KeySet.CreateDefaultKeySet();
ExternalKeyReader.ReadKeyFile(keySet, args[0], args[1], null, null);
string outDir = args[2];

(Nca program, Nca control, IFileSystem pfs) Open(string nspPath)
{
    var stream = File.OpenRead(nspPath);
    var pfs = new PartitionFileSystem();
    pfs.Initialize(stream.AsStorage()).ThrowIfFailure();
    foreach (var e in pfs.EnumerateEntries("/", "*.tik"))
    {
        using var f = new UniqueRef<IFile>();
        pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var data = new byte[0x2C0];
        f.Get.Read(out long _, 0, data).ThrowIfFailure();
        var t = new Ticket(new MemoryStream(data));
        var tk = t.GetTitleKey(keySet);
        if (tk != null) keySet.ExternalKeySet.Add(new LibHac.Fs.RightsId(t.RightsId), new AccessKey(tk));
        Console.WriteLine($"  ticket {e.Name}");
    }
    Nca program = null, control = null;
    foreach (var e in pfs.EnumerateEntries("/", "*.nca"))
    {
        using var f = new UniqueRef<IFile>();
        pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var nca = new Nca(keySet, f.Release().AsStorage());
        Console.WriteLine($"  {e.Name}  type={nca.Header.ContentType} tid={nca.Header.TitleId:X16} sdk={nca.Header.SdkVersion} size={e.Size}");
        if (nca.Header.ContentType == NcaContentType.Program) program = nca;
        if (nca.Header.ContentType == NcaContentType.Control) control = nca;
        if (nca.Header.ContentType == NcaContentType.Manual && Environment.GetEnvironmentVariable("NSPX_MANUAL") is string md)
            Dump(nca.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.None), Path.Combine(md, e.Name));
    }
    return (program, control, pfs);
}

void Dump(IFileSystem fs, string dest)
{
    foreach (var e in fs.EnumerateEntries().Where(x => x.Type == DirectoryEntryType.File))
    {
        string p = Path.Combine(dest, e.FullPath.TrimStart('/'));
        Directory.CreateDirectory(Path.GetDirectoryName(p));
        using var f = new UniqueRef<IFile>();
        fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        f.Get.GetSize(out long size).ThrowIfFailure();
        using var o = File.Create(p);
        var buf = new byte[1 << 20];
        for (long off = 0; off < size;)
        {
            f.Get.Read(out long n, off, buf, ReadOption.None).ThrowIfFailure();
            if (n == 0) break;
            o.Write(buf, 0, (int)n); off += n;
        }
    }
    Console.WriteLine($"  -> {dest}");
}

Console.WriteLine($"[base] {args[3]}");
var (baseProg, baseCtrl, _) = Open(args[3]);
var lvl = IntegrityCheckLevel.None;
Dump(baseProg.OpenFileSystem(NcaSectionType.Code, lvl), Path.Combine(outDir, "base", "exefs"));
Dump(new RomFsFileSystem(baseProg.OpenStorage(NcaSectionType.Data, lvl)), Path.Combine(outDir, "base", "romfs"));
if (baseCtrl != null) Dump(baseCtrl.OpenFileSystem(NcaSectionType.Data, lvl), Path.Combine(outDir, "base", "control"));

if (args.Length > 4)
{
    Console.WriteLine($"[update] {args[4]}");
    var (patchProg, patchCtrl, _) = Open(args[4]);
    Dump(baseProg.OpenFileSystemWithPatch(patchProg, NcaSectionType.Code, lvl), Path.Combine(outDir, "patched", "exefs"));
    Dump(new RomFsFileSystem(baseProg.OpenStorageWithPatch(patchProg, NcaSectionType.Data, lvl)), Path.Combine(outDir, "patched", "romfs"));
    if (patchCtrl != null) Dump(patchCtrl.OpenFileSystem(NcaSectionType.Data, lvl), Path.Combine(outDir, "patched", "control"));
}
