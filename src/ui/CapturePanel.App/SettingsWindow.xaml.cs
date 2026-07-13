using System.Windows;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class SettingsWindow : Window
{
    private readonly SettingsViewModel _model;

    public SettingsWindow(SettingsViewModel model)
    {
        InitializeComponent();
        _model = model;
        DataContext = model;
        model.ErrorRaised += Model_ErrorRaised;
        Closed += SettingsWindow_Closed;
    }

    private void Model_ErrorRaised(object? sender, string message)
        => MessageBox.Show(this, message, "Capture Panel", MessageBoxButton.OK, MessageBoxImage.Error);

    private void SettingsWindow_Closed(object? sender, EventArgs e)
    {
        _model.ErrorRaised -= Model_ErrorRaised;
        Closed -= SettingsWindow_Closed;
    }
}
