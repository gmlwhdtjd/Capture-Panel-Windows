using System.Windows;
using CapturePanel.App.ViewModels;

namespace CapturePanel.App;

public partial class SettingsWindow : Window
{
    public SettingsWindow(SettingsViewModel model)
    {
        InitializeComponent();
        DataContext = model;
    }
}
