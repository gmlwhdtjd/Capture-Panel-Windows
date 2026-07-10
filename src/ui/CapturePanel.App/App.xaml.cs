using System.IO;
using System.Windows;
using CapturePanel.App.Services;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var workerPath = Path.Combine(AppContext.BaseDirectory, "capture-panel.exe");
        var worker = new CaptureWorkerClient(workerPath);
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
