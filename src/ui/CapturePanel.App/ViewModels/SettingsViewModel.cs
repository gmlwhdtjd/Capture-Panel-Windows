using System.Diagnostics;
using System.IO;
using System.Reflection;
using CapturePanel.App.Infrastructure;
using CapturePanel.App.Services;

namespace CapturePanel.App.ViewModels;

public sealed class SettingsViewModel : ObservableObject
{
    private const string GplLicenseFileName = "GPL-3.0.txt";
    private const string ThirdPartyNoticesFileName = "THIRD_PARTY_NOTICES.md";
    private const string AsioLicenseFileName = "Steinberg-ASIO-SDK-LICENSE.txt";

    public SettingsViewModel(string workerPath)
    {
        WorkerPath = workerPath;
        VersionText = $"Version {Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "-"}";
        OpenLicenseCommand = new RelayCommand(
            () => OpenBundledLicense(GplLicenseFileName),
            () => LicenseAvailable);
        OpenThirdPartyNoticesCommand = new RelayCommand(
            () => OpenBundledLicense(ThirdPartyNoticesFileName),
            () => ThirdPartyNoticesAvailable);
        OpenAsioLicenseCommand = new RelayCommand(
            () => OpenBundledLicense(AsioLicenseFileName),
            () => AsioLicenseAvailable);
    }

    public string VersionText { get; }
    public event EventHandler<string>? ErrorRaised;
    public string WorkerPath { get; }
    public bool WorkerAvailable => File.Exists(WorkerPath);
    public string WorkerStatusText => WorkerAvailable ? "Bundled worker available" : "Bundled worker missing";
    public bool LicenseAvailable => BundledLicenseExists(GplLicenseFileName);
    public bool ThirdPartyNoticesAvailable => BundledLicenseExists(ThirdPartyNoticesFileName);
    public bool AsioLicenseAvailable => BundledLicenseExists(AsioLicenseFileName);
    public RelayCommand OpenLicenseCommand { get; }
    public RelayCommand OpenThirdPartyNoticesCommand { get; }
    public RelayCommand OpenAsioLicenseCommand { get; }

    private static bool BundledLicenseExists(string fileName)
        => File.Exists(AppBundlePaths.LicensePath(fileName));

    private void OpenBundledLicense(string fileName)
    {
        var path = AppBundlePaths.LicensePath(fileName);
        try
        {
            if (!File.Exists(path))
            {
                ErrorRaised?.Invoke(this, $"The bundled file is missing: {fileName}");
                return;
            }

            if (Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }) is null)
            {
                ErrorRaised?.Invoke(this, $"Windows could not open {Path.GetFileName(path)}.");
            }
        }
        catch (Exception exception) when (exception is InvalidOperationException
            or IOException
            or UnauthorizedAccessException
            or System.ComponentModel.Win32Exception)
        {
            ErrorRaised?.Invoke(this, $"Could not open {Path.GetFileName(path)}: {exception.Message}");
        }
    }
}
