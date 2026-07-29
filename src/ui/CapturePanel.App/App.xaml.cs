using System.Windows;
using CapturePanel.App.Services;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class App : Application
{
    private WindowsCaptureNotificationService? _notificationService;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var showDevelopmentDevices = string.Equals(
            Environment.GetEnvironmentVariable("CAPTURE_PANEL_SHOW_FAKE"),
            "1",
            StringComparison.Ordinal);
        var worker = new CaptureWorkerClient(AppBundlePaths.WorkerPath);
        ICaptureNotificationService notificationService = DisabledCaptureNotificationService.Instance;
        if (!showDevelopmentDevices)
        {
            _notificationService = new WindowsCaptureNotificationService(Dispatcher);
            notificationService = _notificationService;
        }
        var model = new MainViewModel(
            worker,
            new JsonAppSettingsStore(),
            new WpfFileDialogService(),
            showDevelopmentDevices: showDevelopmentDevices,
            notificationService: notificationService);
        var window = new MainWindow(model);
        MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _notificationService?.Dispose();
        _notificationService = null;
        base.OnExit(e);
    }
}
