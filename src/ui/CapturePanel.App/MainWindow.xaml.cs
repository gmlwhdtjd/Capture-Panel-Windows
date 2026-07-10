using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Threading;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class MainWindow : Window
{
    private readonly MainViewModel _model;
    private readonly Stopwatch _statusAnnouncementClock = Stopwatch.StartNew();
    private bool _initialized;
    private bool _statusAnnouncementPending;
    private bool _verificationAnnouncementPending;
    private SettingsWindow? _settingsWindow;

    public MainWindow(MainViewModel model)
    {
        InitializeComponent();
        _model = model;
        DataContext = model;
        model.ErrorRaised += Model_ErrorRaised;
        model.PropertyChanged += Model_PropertyChanged;
        Loaded += MainWindow_Loaded;
        Closing += MainWindow_Closing;
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        if (_initialized)
        {
            return;
        }
        _initialized = true;
        await _model.InitializeAsync();
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        _settingsWindow = new SettingsWindow(new SettingsViewModel(_model.WorkerPath))
        {
            Owner = this,
        };
        _settingsWindow.Closed += (_, _) => _settingsWindow = null;
        _settingsWindow.Show();
    }

    private void Model_ErrorRaised(object? sender, string message)
    {
        Dispatcher.InvokeAsync(() => MessageBox.Show(
            this,
            message,
            "Capture Panel",
            MessageBoxButton.OK,
            MessageBoxImage.Error));
    }

    private void Model_PropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainViewModel.StatusTitle))
        {
            ScheduleStatusAnnouncement(force: true);
        }
        else if (e.PropertyName == nameof(MainViewModel.StatusMessage))
        {
            ScheduleStatusAnnouncement(force: false);
        }

        if (e.PropertyName is nameof(MainViewModel.VerificationFooterText)
            or nameof(MainViewModel.LevelText)
            or nameof(MainViewModel.LatencyText)
            or nameof(MainViewModel.StabilityText))
        {
            ScheduleVerificationAnnouncement();
        }
    }

    private void ScheduleStatusAnnouncement(bool force)
    {
        if (_statusAnnouncementPending
            || (!force && _statusAnnouncementClock.Elapsed < TimeSpan.FromSeconds(1)))
        {
            return;
        }

        _statusAnnouncementPending = true;
        Dispatcher.BeginInvoke(() =>
        {
            _statusAnnouncementPending = false;
            _statusAnnouncementClock.Restart();
            RaiseLiveRegionChanged(StatusMessageRegion);
        }, DispatcherPriority.Background);
    }

    private void ScheduleVerificationAnnouncement()
    {
        if (_verificationAnnouncementPending)
        {
            return;
        }

        _verificationAnnouncementPending = true;
        Dispatcher.BeginInvoke(() =>
        {
            _verificationAnnouncementPending = false;
            RaiseLiveRegionChanged(VerificationRegion);
        }, DispatcherPriority.Background);
    }

    private static void RaiseLiveRegionChanged(UIElement element)
    {
        var peer = UIElementAutomationPeer.FromElement(element)
            ?? UIElementAutomationPeer.CreatePeerForElement(element);
        peer?.RaiseAutomationEvent(AutomationEvents.LiveRegionChanged);
    }

    private void MainWindow_Closing(object? sender, CancelEventArgs e)
    {
        _model.ErrorRaised -= Model_ErrorRaised;
        _model.PropertyChanged -= Model_PropertyChanged;
        _model.Dispose();
    }
}
