using Microsoft.Win32;

namespace CapturePanel.App.Services;

public interface IFileDialogService
{
    string? ChooseSourceWav();
    string? ChooseCaptureDestination(string defaultFilename);
}

public sealed class WpfFileDialogService : IFileDialogService
{
    public string? ChooseSourceWav()
    {
        var dialog = new OpenFileDialog
        {
            Title = "Choose WAV Source",
            Filter = "WAV audio (*.wav)|*.wav|All files (*.*)|*.*",
            CheckFileExists = true,
            Multiselect = false,
        };
        return dialog.ShowDialog() == true ? dialog.FileName : null;
    }

    public string? ChooseCaptureDestination(string defaultFilename)
    {
        var dialog = new SaveFileDialog
        {
            Title = "Save Capture",
            Filter = "WAV audio (*.wav)|*.wav",
            AddExtension = true,
            DefaultExt = ".wav",
            FileName = defaultFilename,
            OverwritePrompt = true,
        };
        return dialog.ShowDialog() == true ? dialog.FileName : null;
    }
}
