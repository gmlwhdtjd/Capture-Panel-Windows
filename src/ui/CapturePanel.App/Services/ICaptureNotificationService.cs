namespace CapturePanel.App.Services;

public interface ICaptureNotificationService
{
    void PrepareForCapture();
    void NotifyCaptureSaved(string filename);
    void NotifyCaptureFailed(string filename, string reason);
}

public sealed class DisabledCaptureNotificationService : ICaptureNotificationService
{
    public static DisabledCaptureNotificationService Instance { get; } = new();

    private DisabledCaptureNotificationService()
    {
    }

    public void PrepareForCapture()
    {
    }

    public void NotifyCaptureSaved(string filename)
    {
    }

    public void NotifyCaptureFailed(string filename, string reason)
    {
    }
}
