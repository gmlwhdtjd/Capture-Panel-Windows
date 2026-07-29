using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace CapturePanel.App.Services;

public sealed class WindowsCaptureNotificationService : ICaptureNotificationService, IDisposable
{
    private const int NotificationCallbackMessage = 0x8001;
    private const uint NotifyIconVersion4 = 4;

    private const uint NimAdd = 0;
    private const uint NimModify = 1;
    private const uint NimDelete = 2;
    private const uint NimSetVersion = 4;

    private const uint NifMessage = 0x00000001;
    private const uint NifIcon = 0x00000002;
    private const uint NifTip = 0x00000004;
    private const uint NifInfo = 0x00000010;
    private const uint NifShowTip = 0x00000080;

    private const uint NiifInfo = 0x00000001;
    private const uint NiifError = 0x00000003;
    private const uint NiifUser = 0x00000004;
    private const uint NiifLargeIcon = 0x00000020;

    private const uint NinBalloonHide = 0x0403;
    private const uint NinBalloonTimeout = 0x0404;
    private const uint NinBalloonUserClick = 0x0405;

    private static readonly TimeSpan CleanupDelay = TimeSpan.FromMinutes(1);

    private readonly Dispatcher _dispatcher;
    private readonly int _taskbarCreatedMessage;
    private HwndSource? _windowSource;
    private IntPtr _windowHandle;
    private ushort _nextNotificationIconId;
    private ushort _notificationIconId;
    private IntPtr _notificationIcon;
    private IntPtr _balloonIcon;
    private string? _activeNotificationTitle;
    private string? _activeNotificationInfo;
    private uint _activeNotificationInfoFlags;
    private bool _ownsExtractedIcons;
    private bool _iconAdded;
    private DispatcherTimer? _cleanupTimer;
    private bool _disposed;

    public WindowsCaptureNotificationService(Dispatcher dispatcher)
    {
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _taskbarCreatedMessage = unchecked((int)RegisterWindowMessage("TaskbarCreated"));
    }

    public void PrepareForCapture()
    {
        RunBestEffort(() => _ = EnsureWindowHook());
    }

    public void NotifyCaptureSaved(string filename)
    {
        var safeFilename = string.IsNullOrWhiteSpace(filename)
            ? "Capture"
            : filename;
        RunBestEffort(() => ShowNotification(
            "Capture complete",
            BuildCaptureSavedInfo(safeFilename),
            isFailure: false));
    }

    public void NotifyCaptureFailed(string filename, string reason)
    {
        var safeFilename = string.IsNullOrWhiteSpace(filename)
            ? "Capture"
            : filename;
        var safeReason = string.IsNullOrWhiteSpace(reason)
            ? "The capture could not be completed."
            : reason;
        RunBestEffort(() => ShowNotification(
            "Capture failed",
            BuildCaptureFailedInfo(safeFilename, safeReason),
            isFailure: true));
    }

    private static string BuildCaptureSavedInfo(string filename)
    {
        const string suffix = " was saved successfully.";
        return TruncateUtf16WithEllipsis(filename, 255 - suffix.Length) + suffix;
    }

    private static string BuildCaptureFailedInfo(string filename, string reason)
    {
        const string separator = ": ";
        const int filenameLimit = 96;
        var shortFilename = TruncateUtf16WithEllipsis(filename, filenameLimit);
        var reasonLimit = 255 - shortFilename.Length - separator.Length;
        return shortFilename + separator + TruncateUtf16WithEllipsis(reason, reasonLimit);
    }

    private void RunBestEffort(Action action)
    {
        try
        {
            if (_disposed || _dispatcher.HasShutdownStarted || _dispatcher.HasShutdownFinished)
            {
                return;
            }

            if (_dispatcher.CheckAccess())
            {
                TryRun(action);
                return;
            }

            _dispatcher.BeginInvoke(
                () =>
                {
                    if (!_disposed
                        && !_dispatcher.HasShutdownStarted
                        && !_dispatcher.HasShutdownFinished)
                    {
                        TryRun(action);
                    }
                },
                DispatcherPriority.Normal);
        }
        catch (Exception)
        {
            // A notification is supplementary and must never change the capture result.
        }
    }

    private static void TryRun(Action action)
    {
        try
        {
            action();
        }
        catch (Exception)
        {
            // Notifications are best-effort and must not alter capture state.
        }
    }

    private void ShowNotification(string title, string info, bool isFailure)
    {
        if (_disposed || !EnsureWindowHook())
        {
            return;
        }

        RemoveNotificationIcon();
        try
        {
            ExtractApplicationIcons();
            _notificationIconId = NextNotificationIconId();
            _activeNotificationTitle = TruncateUtf16(title, 63);
            _activeNotificationInfo = TruncateUtf16(info, 255);
            _activeNotificationInfoFlags = isFailure
                ? NiifError
                : _balloonIcon != IntPtr.Zero
                    ? NiifUser | NiifLargeIcon
                    : NiifInfo;

            if (!AddCurrentNotificationIcon())
            {
                RemoveNotificationIcon();
                return;
            }

            StartCleanupTimer();
        }
        catch (Exception)
        {
            RemoveNotificationIcon();
            throw;
        }
    }

    private bool AddCurrentNotificationIcon()
    {
        if (_notificationIconId == 0
            || _activeNotificationTitle is null
            || _activeNotificationInfo is null)
        {
            return false;
        }

        var data = CreateNotifyIconData(_notificationIconId);
        if (!ShellNotifyIcon(NimAdd, ref data))
        {
            return false;
        }
        _iconAdded = true;

        var versionData = data;
        versionData.TimeoutOrVersion = NotifyIconVersion4;
        _ = ShellNotifyIcon(NimSetVersion, ref versionData);

        data.Flags |= NifInfo;
        data.Info = _activeNotificationInfo;
        data.InfoTitle = _activeNotificationTitle;
        data.InfoFlags = _activeNotificationInfoFlags;
        return ShellNotifyIcon(NimModify, ref data);
    }

    private void StartCleanupTimer()
    {
        _cleanupTimer?.Stop();
        var iconId = _notificationIconId;
        _cleanupTimer = new DispatcherTimer(
            CleanupDelay,
            DispatcherPriority.Background,
            (_, _) =>
            {
                if (_notificationIconId == iconId)
                {
                    RemoveNotificationIcon();
                }
            },
            _dispatcher);
        _cleanupTimer.Start();
    }

    private bool EnsureWindowHook()
    {
        if (_disposed || _dispatcher.HasShutdownStarted || _dispatcher.HasShutdownFinished)
        {
            return false;
        }

        if (_windowSource is not null)
        {
            return true;
        }

        var window = Application.Current?.MainWindow;
        if (window is null)
        {
            return false;
        }

        var handle = new WindowInteropHelper(window).EnsureHandle();
        var source = HwndSource.FromHwnd(handle);
        if (source is null)
        {
            return false;
        }

        source.AddHook(WindowMessageHook);
        _windowHandle = handle;
        _windowSource = source;
        return true;
    }

    private IntPtr WindowMessageHook(
        IntPtr windowHandle,
        int message,
        IntPtr wordParameter,
        IntPtr longParameter,
        ref bool handled)
    {
        if (_taskbarCreatedMessage != 0 && message == _taskbarCreatedMessage)
        {
            TryRun(RestoreNotificationIconAfterTaskbarRestart);
            return IntPtr.Zero;
        }

        if (message != NotificationCallbackMessage)
        {
            return IntPtr.Zero;
        }

        if (!TryDecodeCurrentNotificationCallback(
                wordParameter,
                longParameter,
                out var notificationCode))
        {
            return IntPtr.Zero;
        }

        TryRun(() =>
        {
            if (notificationCode == NinBalloonUserClick)
            {
                var window = Application.Current?.MainWindow;
                if (window is not null)
                {
                    if (window.WindowState == WindowState.Minimized)
                    {
                        window.WindowState = WindowState.Normal;
                    }
                    _ = window.Activate();
                }
            }

            if (notificationCode is NinBalloonHide or NinBalloonTimeout or NinBalloonUserClick)
            {
                RemoveNotificationIcon();
            }
        });

        return IntPtr.Zero;
    }

    private bool TryDecodeCurrentNotificationCallback(
        IntPtr wordParameter,
        IntPtr longParameter,
        out uint notificationCode)
    {
        var rawLongParameter = unchecked((uint)longParameter.ToInt64());
        var version4Code = rawLongParameter & 0xFFFF;
        var version4IconId = rawLongParameter >> 16;
        if (version4IconId == _notificationIconId
            && IsCompletionNotificationCode(version4Code))
        {
            notificationCode = version4Code;
            return true;
        }

        var legacyIconId = unchecked((uint)wordParameter.ToInt64());
        if (legacyIconId == _notificationIconId
            && IsCompletionNotificationCode(rawLongParameter))
        {
            notificationCode = rawLongParameter;
            return true;
        }

        notificationCode = 0;
        return false;
    }

    private static bool IsCompletionNotificationCode(uint notificationCode)
        => notificationCode is NinBalloonHide or NinBalloonTimeout or NinBalloonUserClick;

    private void RestoreNotificationIconAfterTaskbarRestart()
    {
        if (_disposed
            || !_iconAdded
            || _activeNotificationTitle is null
            || _activeNotificationInfo is null)
        {
            return;
        }

        try
        {
            // Explorer discarded the old icon. A new ID also makes any delayed callback
            // from that incarnation harmless.
            _iconAdded = false;
            _notificationIconId = NextNotificationIconId();
            if (!AddCurrentNotificationIcon())
            {
                RemoveNotificationIcon();
                return;
            }
            StartCleanupTimer();
        }
        catch (Exception)
        {
            RemoveNotificationIcon();
            throw;
        }
    }

    private ushort NextNotificationIconId()
    {
        _nextNotificationIconId++;
        if (_nextNotificationIconId == 0)
        {
            _nextNotificationIconId = 1;
        }
        return _nextNotificationIconId;
    }

    private NotifyIconData CreateNotifyIconData(uint iconId)
    {
        var flags = NifMessage | NifTip | NifShowTip;
        if (_notificationIcon != IntPtr.Zero)
        {
            flags |= NifIcon;
        }

        return new NotifyIconData
        {
            Size = checked((uint)Marshal.SizeOf<NotifyIconData>()),
            WindowHandle = _windowHandle,
            Id = iconId,
            Flags = flags,
            CallbackMessage = NotificationCallbackMessage,
            IconHandle = _notificationIcon,
            Tip = "Capture Panel",
            State = 0,
            StateMask = 0,
            Info = string.Empty,
            TimeoutOrVersion = 0,
            InfoTitle = string.Empty,
            InfoFlags = 0,
            ItemGuid = Guid.Empty,
            BalloonIconHandle = _balloonIcon,
        };
    }

    private void ExtractApplicationIcons()
    {
        var executablePath = Environment.ProcessPath;
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            return;
        }

        if (ExtractIconEx(
                executablePath,
                0,
                out var largeIcon,
                out var smallIcon,
                1) == 0)
        {
            return;
        }

        _notificationIcon = smallIcon != IntPtr.Zero ? smallIcon : largeIcon;
        _balloonIcon = largeIcon != IntPtr.Zero ? largeIcon : smallIcon;
        _ownsExtractedIcons = _notificationIcon != IntPtr.Zero || _balloonIcon != IntPtr.Zero;
    }

    private void RemoveNotificationIcon()
    {
        _cleanupTimer?.Stop();
        _cleanupTimer = null;

        var iconAdded = _iconAdded;
        var iconId = _notificationIconId;
        _iconAdded = false;
        _notificationIconId = 0;
        _activeNotificationTitle = null;
        _activeNotificationInfo = null;
        _activeNotificationInfoFlags = 0;

        try
        {
            if (iconAdded)
            {
                var data = CreateNotifyIconData(iconId);
                _ = ShellNotifyIcon(NimDelete, ref data);
            }
        }
        finally
        {
            DestroyExtractedIcons();
        }
    }

    private void DestroyExtractedIcons()
    {
        if (_ownsExtractedIcons)
        {
            if (_notificationIcon != IntPtr.Zero)
            {
                _ = DestroyIcon(_notificationIcon);
            }
            if (_balloonIcon != IntPtr.Zero && _balloonIcon != _notificationIcon)
            {
                _ = DestroyIcon(_balloonIcon);
            }
        }

        _notificationIcon = IntPtr.Zero;
        _balloonIcon = IntPtr.Zero;
        _ownsExtractedIcons = false;
    }

    private static string TruncateUtf16(string value, int maximumLength)
    {
        if (value.Length <= maximumLength)
        {
            return value;
        }

        var length = maximumLength;
        if (length > 0 && char.IsHighSurrogate(value[length - 1]))
        {
            length--;
        }
        return value[..length];
    }

    private static string TruncateUtf16WithEllipsis(string value, int maximumLength)
    {
        if (value.Length <= maximumLength)
        {
            return value;
        }
        if (maximumLength <= 1)
        {
            return maximumLength == 1 ? "…" : string.Empty;
        }
        return TruncateUtf16(value, maximumLength - 1) + "…";
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        void DisposeOnDispatcher()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            RemoveNotificationIcon();
            if (_windowSource is not null)
            {
                _windowSource.RemoveHook(WindowMessageHook);
                _windowSource = null;
            }
            _windowHandle = IntPtr.Zero;
        }

        try
        {
            if (_dispatcher.CheckAccess())
            {
                TryRun(DisposeOnDispatcher);
            }
            else if (!_dispatcher.HasShutdownStarted && !_dispatcher.HasShutdownFinished)
            {
                _dispatcher.Invoke(() => TryRun(DisposeOnDispatcher));
            }
        }
        catch (Exception)
        {
            // Process shutdown will release any remaining native shell resources.
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public uint Size;
        public IntPtr WindowHandle;
        public uint Id;
        public uint Flags;
        public uint CallbackMessage;
        public IntPtr IconHandle;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Tip;

        public uint State;
        public uint StateMask;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Info;

        public uint TimeoutOrVersion;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string InfoTitle;

        public uint InfoFlags;
        public Guid ItemGuid;
        public IntPtr BalloonIconHandle;
    }

    [DllImport(
        "shell32.dll",
        EntryPoint = "Shell_NotifyIconW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShellNotifyIcon(uint message, ref NotifyIconData data);

    [DllImport(
        "shell32.dll",
        EntryPoint = "ExtractIconExW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true)]
    private static extern uint ExtractIconEx(
        string file,
        int iconIndex,
        out IntPtr largeIcon,
        out IntPtr smallIcon,
        uint iconCount);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyIcon(IntPtr icon);

    [DllImport(
        "user32.dll",
        EntryPoint = "RegisterWindowMessageW",
        CharSet = CharSet.Unicode,
        ExactSpelling = true,
        SetLastError = true)]
    private static extern uint RegisterWindowMessage(string message);
}
