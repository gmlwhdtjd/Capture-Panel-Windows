namespace CapturePanel.App.Models;

public sealed record AudioDeviceInfo(
    string Id,
    string Name,
    int InputChannels,
    int OutputChannels,
    double SampleRate,
    bool Available,
    string Status)
{
    public string DisplayName => Available ? Name : $"{Name} (Unavailable)";
    public bool CanCapture => Available && InputChannels > 0 && OutputChannels > 0;
}

public sealed record AudioChannelInfo(int Index, string Name)
{
    public string DisplayName
    {
        get
        {
            var trimmed = Name.Trim();
            return trimmed.Length == 0 ? Index.ToString() : $"{Index} : {trimmed}";
        }
    }
}

public sealed record DeviceChannels(
    string DeviceId,
    string DeviceName,
    double SampleRate,
    IReadOnlyList<AudioChannelInfo> Inputs,
    IReadOnlyList<AudioChannelInfo> Outputs);
