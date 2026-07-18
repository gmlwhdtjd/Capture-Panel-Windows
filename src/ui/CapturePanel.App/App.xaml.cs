using System.Windows;
using CapturePanel.App.Services;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var worker = new CaptureWorkerClient(AppBundlePaths.WorkerPath);
        var model = new MainViewModel(
            worker,
            new JsonAppSettingsStore(),
            new WpfFileDialogService(),
            showDevelopmentDevices: string.Equals(
                Environment.GetEnvironmentVariable("CAPTURE_PANEL_SHOW_FAKE"),
                "1",
                StringComparison.Ordinal));
        var window = new MainWindow(model);
        MainWindow = window;
        window.Show();
    }
}
