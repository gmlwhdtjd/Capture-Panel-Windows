using System.Diagnostics;
using System.IO;
using System.Reflection;
using CapturePanel.App.Infrastructure;

namespace CapturePanel.App.ViewModels;

public sealed class SettingsViewModel : ObservableObject
{
    public SettingsViewModel(string workerPath)
    {
        WorkerPath = workerPath;
        VersionText = $"Version {Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "-"}";
        OpenLicenseCommand = new RelayCommand(() => OpenBundledFile("LICENSE"), () => LicenseAvailable);
        OpenThirdPartyNoticesCommand = new RelayCommand(
            () => OpenBundledFile("THIRD_PARTY_NOTICES.md"),
            () => ThirdPartyNoticesAvailable);
        OpenAsioLicenseCommand = new RelayCommand(
            () => OpenBundledFile(Path.Combine("licenses", "Steinberg-ASIO-SDK-LICENSE.txt")),
            () => AsioLicenseAvailable);
    }

    public string VersionText { get; }
    public string WorkerPath { get; }
    public bool WorkerAvailable => File.Exists(WorkerPath);
    public string WorkerStatusText => WorkerAvailable ? "Bundled worker available" : "Bundled worker missing";
    public bool LicenseAvailable => BundledFileExists("LICENSE");
    public bool ThirdPartyNoticesAvailable => BundledFileExists("THIRD_PARTY_NOTICES.md");
    public bool AsioLicenseAvailable => BundledFileExists(Path.Combine("licenses", "Steinberg-ASIO-SDK-LICENSE.txt"));
    public RelayCommand OpenLicenseCommand { get; }
    public RelayCommand OpenThirdPartyNoticesCommand { get; }
    public RelayCommand OpenAsioLicenseCommand { get; }

    private static bool BundledFileExists(string relativePath)
        => File.Exists(Path.Combine(AppContext.BaseDirectory, relativePath));

    private static void OpenBundledFile(string relativePath)
    {
        var path = Path.Combine(AppContext.BaseDirectory, relativePath);
        if (!File.Exists(path))
        {
            return;
        }

        Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
    }
}
