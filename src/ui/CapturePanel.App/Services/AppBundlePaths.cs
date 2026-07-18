using System.IO;

namespace CapturePanel.App.Services;

public static class AppBundlePaths
{
    public const string BinDirectoryName = "bin";
    public const string DocsDirectoryName = "docs";
    public const string LicensesDirectoryName = "licenses";
    public const string WorkerFileName = "capture-panel.exe";

    public static string BinDirectory => Path.Combine(
        AppContext.BaseDirectory,
        BinDirectoryName);

    public static string DocsDirectory => Path.Combine(
        AppContext.BaseDirectory,
        DocsDirectoryName);

    public static string LicensesDirectory => Path.Combine(
        AppContext.BaseDirectory,
        LicensesDirectoryName);

    public static string WorkerPath => CombineUnder(BinDirectory, WorkerFileName);

    public static string DocumentPath(string relativePath)
        => CombineUnder(DocsDirectory, relativePath);

    public static string LicensePath(string relativePath)
        => CombineUnder(LicensesDirectory, relativePath);

    private static string CombineUnder(string directory, string relativePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(relativePath);
        if (Path.IsPathRooted(relativePath))
        {
            throw new ArgumentException("A bundled file path must be relative.", nameof(relativePath));
        }

        var directoryRoot = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar);
        var path = Path.GetFullPath(Path.Combine(directoryRoot, relativePath));
        if (!path.StartsWith(
                directoryRoot + Path.DirectorySeparatorChar,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new ArgumentException("A bundled file path cannot leave its assigned directory.", nameof(relativePath));
        }

        return path;
    }
}
