using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using CapturePanel.App.Infrastructure;
using CapturePanel.App.Models;
using CapturePanel.App.Services;

namespace CapturePanel.App.ViewModels;

public sealed class MainViewModel : ObservableObject, IDisposable
{
    private const double NormalInputLowerBoundDbfs = -24.0;
    private const double HotInputLowerBoundDbfs = -6.0;

    private readonly ICaptureWorkerClient _worker;
    private readonly IAppSettingsStore _settingsStore;
    private readonly IFileDialogService _fileDialogs;
    private readonly bool _showDevelopmentDevices;
    private readonly TimeSpan _setupTestTimeout;
    private AppSettings _settings;
    private AudioDeviceInfo? _selectedOutputDevice;
    private AudioChannelInfo? _selectedPlaybackChannel;
    private AudioChannelInfo? _selectedRecordChannel;
    private string? _sourcePath;
    private WavMetadata? _sourceMetadata;
    private WavFileSnapshot? _sourceSnapshot;
    private int _sourceSelectionVersion;
    private double _outputTrimDb;
    private double _inputTrimDb;
    private double? _outputPeakDb;
    private double? _inputPeakDb;
    private double? _latencyMilliseconds;
    private CaptureStabilityLevel _stabilityLevel;
    private string? _stabilityReason;
    private bool _clippingDetected;
    private string _assessment = "Run a test to enable capture.";
    private bool _captureSetupVerified;
    private bool _isRefreshingDevices;
    private bool _isTesting;
    private bool _isCapturing;
    private bool _isCancelling;
    private bool _deviceDiscoveryFailed;
    private double? _progressFraction;
    private string _progressLabel = "Idle";
    private double? _remainingSeconds;
    private string? _lastOutputPath;
    private string? _captureWarningMessage;
    private string? _recoveryOutputPath;
    private CancellationTokenSource? _operationCancellation;
    private CancellationTokenSource? _deviceCancellation;
    private CancellationTokenSource? _channelCancellation;
    private bool _suppressConfigurationChanges;
    private bool _disposed;

    public MainViewModel(
        ICaptureWorkerClient worker,
        IAppSettingsStore settingsStore,
        IFileDialogService fileDialogs,
        bool showDevelopmentDevices = false,
        TimeSpan? setupTestTimeout = null)
    {
        _worker = worker;
        _settingsStore = settingsStore;
        _fileDialogs = fileDialogs;
        _showDevelopmentDevices = showDevelopmentDevices;
        _setupTestTimeout = setupTestTimeout ?? TimeSpan.FromSeconds(30);
        if (_setupTestTimeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(setupTestTimeout));
        }
        _settings = settingsStore.Load();
        _outputTrimDb = Math.Clamp(Math.Round(_settings.OutputTrimDb), -24, 0);
        _inputTrimDb = Math.Clamp(Math.Round(_settings.InputTrimDb), -18, 12);

        var savedSourcePath = _settings.SourcePath;
        if (!string.IsNullOrWhiteSpace(savedSourcePath))
        {
            try
            {
                _sourcePath = Path.GetFullPath(savedSourcePath);
            }
            catch (Exception)
            {
                _sourcePath = null;
                _sourceMetadata = null;
            }
        }

        ChooseSourceCommand = new RelayCommand(ChooseSource, () => !ControlsLocked);
        RefreshDevicesCommand = new AsyncRelayCommand(
            () => RefreshDevicesAsync(),
            () => !ControlsLocked,
            ReportUnexpectedError);
        TestCommand = new AsyncRelayCommand(
            RunSetupTestAsync,
            () => CanTest,
            ReportUnexpectedError);
        CaptureCommand = new RelayCommand(CaptureOrCancel, () => IsCapturing ? CanCancelCapture : CanCapture);
    }

    public event EventHandler<string>? ErrorRaised;

    public ObservableCollection<AudioDeviceInfo> Devices { get; } = [];
    public ObservableCollection<AudioChannelInfo> PlaybackChannels { get; } = [];
    public ObservableCollection<AudioChannelInfo> RecordChannels { get; } = [];

    public RelayCommand ChooseSourceCommand { get; }
    public AsyncRelayCommand RefreshDevicesCommand { get; }
    public AsyncRelayCommand TestCommand { get; }
    public RelayCommand CaptureCommand { get; }

    public string WorkerPath => _worker.WorkerPath;

    public AudioDeviceInfo? SelectedOutputDevice
    {
        get => _selectedOutputDevice;
        set
        {
            if (!SetProperty(ref _selectedOutputDevice, value))
            {
                return;
            }

            OnPropertiesChanged(
                nameof(SelectedInputDevice),
                nameof(OutputDeviceUnavailableMessage),
                nameof(InputDeviceUnavailableMessage));

            if (!_suppressConfigurationChanges)
            {
                _settings = _settings with
                {
                    Device = value is null ? null : new SavedDevice(value.Id, value.Name),
                };
                InvalidateVerification("Device changed. Test again.");
                SaveSettings();
                LoadSelectedDeviceChannelsInBackground();
            }

            NotifyStateChanged();
        }
    }

    public AudioDeviceInfo? SelectedInputDevice => SelectedOutputDevice;

    public AudioChannelInfo? SelectedPlaybackChannel
    {
        get => _selectedPlaybackChannel;
        set
        {
            if (!SetProperty(ref _selectedPlaybackChannel, value) || _suppressConfigurationChanges)
            {
                return;
            }

            _settings = _settings with { PlaybackChannel = value?.Index };
            InvalidateVerification("Output channel changed. Test again.");
            SaveSettings();
            NotifyStateChanged();
        }
    }

    public AudioChannelInfo? SelectedRecordChannel
    {
        get => _selectedRecordChannel;
        set
        {
            if (!SetProperty(ref _selectedRecordChannel, value) || _suppressConfigurationChanges)
            {
                return;
            }

            _settings = _settings with { RecordChannel = value?.Index };
            InvalidateVerification("Input channel changed. Test again.");
            SaveSettings();
            NotifyStateChanged();
        }
    }

    public string? SourcePath
    {
        get => _sourcePath;
        private set
        {
            if (SetProperty(ref _sourcePath, value))
            {
                OnPropertiesChanged(nameof(SourceName), nameof(SourceReady), nameof(SourceDescription));
                NotifyStateChanged();
            }
        }
    }

    public string SourceName => SourcePath is null
        ? "Choose WAV"
        : MiddleTruncate(Path.GetFileName(SourcePath), 32);
    public bool SourceReady => SourcePath is not null && _sourceMetadata is not null && _sourceSnapshot is not null;

    public string SourceDescription => _sourceMetadata is null
        ? "Select the signal you want to capture"
        : string.Create(
            CultureInfo.InvariantCulture,
            $"{_sourceMetadata.SampleRate / 1000.0:0.#} kHz · {_sourceMetadata.Channels} ch · {_sourceMetadata.BitsPerSample}-bit");

    public double OutputTrimDb
    {
        get => _outputTrimDb;
        set
        {
            var normalized = Math.Clamp(Math.Round(value), -24, 0);
            if (!SetProperty(ref _outputTrimDb, normalized) || _suppressConfigurationChanges)
            {
                return;
            }

            _settings = _settings with { OutputTrimDb = normalized };
            OnPropertyChanged(nameof(OutputTrimText));
            InvalidateVerification("Output level changed. Test again.");
            SaveSettings();
            NotifyStateChanged();
        }
    }

    public double InputTrimDb
    {
        get => _inputTrimDb;
        set
        {
            var normalized = Math.Clamp(Math.Round(value), -18, 12);
            if (!SetProperty(ref _inputTrimDb, normalized) || _suppressConfigurationChanges)
            {
                return;
            }

            _settings = _settings with { InputTrimDb = normalized };
            OnPropertyChanged(nameof(InputTrimText));
            InvalidateVerification("Input level changed. Test again.");
            SaveSettings();
            NotifyStateChanged();
        }
    }

    public string OutputTrimText => $"{OutputTrimDb:0} dB";
    public string InputTrimText => InputTrimDb == 0 ? "0 dB" : $"{InputTrimDb:+0;-0} dB";

    public double? OutputPeakDb
    {
        get => _outputPeakDb;
        private set
        {
            if (SetProperty(ref _outputPeakDb, value))
            {
                OnPropertiesChanged(nameof(OutputPeakText), nameof(OutputMeterPercent), nameof(OutputMeterState));
            }
        }
    }

    public double? InputPeakDb
    {
        get => _inputPeakDb;
        private set
        {
            if (SetProperty(ref _inputPeakDb, value))
            {
                OnPropertiesChanged(
                    nameof(InputPeakText),
                    nameof(InputMeterPercent),
                    nameof(InputMeterState),
                    nameof(LevelText),
                    nameof(LevelState));
            }
        }
    }

    public string OutputPeakText => FormatDbfs(OutputPeakDb);
    public string InputPeakText => FormatDbfs(InputPeakDb);
    public double OutputMeterPercent => MeterPercent(OutputPeakDb);
    public double InputMeterPercent => MeterPercent(InputPeakDb);
    public MeterState OutputMeterState => MeterStateFor(OutputPeakDb);
    public MeterState InputMeterState => MeterStateFor(InputPeakDb);

    public double? LatencyMilliseconds
    {
        get => _latencyMilliseconds;
        private set
        {
            if (SetProperty(ref _latencyMilliseconds, value))
            {
                OnPropertyChanged(nameof(LatencyText));
            }
        }
    }

    public string LatencyText => LatencyMilliseconds is null
        ? "-"
        : $"{LatencyMilliseconds.Value:0.00} ms";

    public CaptureStabilityLevel StabilityLevel
    {
        get => _stabilityLevel;
        private set
        {
            if (SetProperty(ref _stabilityLevel, value))
            {
                OnPropertiesChanged(nameof(StabilityText), nameof(VerificationFooterText));
            }
        }
    }

    public string StabilityText => StabilityLevel == CaptureStabilityLevel.Unknown
        ? "-"
        : StabilityLevel.ToString();

    public string? StabilityReason
    {
        get => _stabilityReason;
        private set
        {
            if (SetProperty(ref _stabilityReason, value))
            {
                OnPropertyChanged(nameof(VerificationFooterText));
            }
        }
    }

    public string Assessment
    {
        get => _assessment;
        private set
        {
            if (SetProperty(ref _assessment, value))
            {
                OnPropertiesChanged(nameof(StatusMessage), nameof(VerificationFooterText));
            }
        }
    }

    public bool CaptureSetupVerified
    {
        get => _captureSetupVerified;
        private set
        {
            if (SetProperty(ref _captureSetupVerified, value))
            {
                NotifyStateChanged();
            }
        }
    }

    public bool IsRefreshingDevices
    {
        get => _isRefreshingDevices;
        private set
        {
            if (SetProperty(ref _isRefreshingDevices, value))
            {
                NotifyStateChanged();
            }
        }
    }

    public bool IsTesting
    {
        get => _isTesting;
        private set
        {
            if (SetProperty(ref _isTesting, value))
            {
                OnPropertyChanged(nameof(VerificationFooterText));
                NotifyStateChanged();
            }
        }
    }

    public bool IsCapturing
    {
        get => _isCapturing;
        private set
        {
            if (SetProperty(ref _isCapturing, value))
            {
                OnPropertyChanged(nameof(CaptureButtonText));
                NotifyStateChanged();
            }
        }
    }

    public bool IsCancelling
    {
        get => _isCancelling;
        private set
        {
            if (SetProperty(ref _isCancelling, value))
            {
                NotifyStateChanged();
            }
        }
    }

    public double? ProgressFraction
    {
        get => _progressFraction;
        private set
        {
            if (SetProperty(ref _progressFraction, value))
            {
                OnPropertiesChanged(nameof(ProgressPercent), nameof(ProgressVisible), nameof(StatusMessage));
            }
        }
    }

    public double ProgressPercent => Math.Clamp((ProgressFraction ?? 0) * 100, 0, 100);
    public bool ProgressVisible => ProgressFraction is not null;

    public string ProgressLabel
    {
        get => _progressLabel;
        private set
        {
            if (SetProperty(ref _progressLabel, value))
            {
                OnPropertyChanged(nameof(StatusMessage));
            }
        }
    }

    public double? RemainingSeconds
    {
        get => _remainingSeconds;
        private set
        {
            if (SetProperty(ref _remainingSeconds, value))
            {
                OnPropertyChanged(nameof(StatusMessage));
            }
        }
    }

    public string? LastOutputPath
    {
        get => _lastOutputPath;
        private set => SetProperty(ref _lastOutputPath, value);
    }

    public string? CaptureWarningMessage
    {
        get => _captureWarningMessage;
        private set
        {
            if (SetProperty(ref _captureWarningMessage, value))
            {
                NotifyStateChanged();
            }
        }
    }

    public string? RecoveryOutputPath
    {
        get => _recoveryOutputPath;
        private set => SetProperty(ref _recoveryOutputPath, value);
    }

    public bool ControlsLocked => IsRefreshingDevices || IsTesting || IsCapturing;
    public bool AudioReady => !_deviceDiscoveryFailed
        && SelectedOutputDevice?.CanCapture == true
        && SelectedPlaybackChannel is not null
        && SelectedRecordChannel is not null;
    public bool CanTest => AudioReady && SourceReady && !ControlsLocked;
    public bool CanCapture => AudioReady && SourceReady && CaptureSetupVerified && !ControlsLocked;
    public bool CanCancelCapture => IsCapturing && !IsCancelling;
    public string CaptureButtonText => IsCapturing ? "Cancel" : "Capture";
    public SetupInputLevel LevelState
    {
        get
        {
            if (InputPeakDb is null) return SetupInputLevel.Unavailable;
            if (IsClipping) return SetupInputLevel.Clipping;
            if (InputPeakDb < NormalInputLowerBoundDbfs) return SetupInputLevel.Low;
            if (InputPeakDb < HotInputLowerBoundDbfs) return SetupInputLevel.Normal;
            return SetupInputLevel.Hot;
        }
    }
    public string LevelText => LevelState switch
    {
        SetupInputLevel.Unavailable => "-",
        SetupInputLevel.Low => "Low",
        SetupInputLevel.Normal => "Normal",
        SetupInputLevel.Hot => "Hot",
        SetupInputLevel.Clipping => "Clipping",
        _ => "-",
    };
    public bool IsClipping => _clippingDetected;

    public string? OutputDeviceUnavailableMessage => SelectedOutputDevice is { Available: false }
        ? string.IsNullOrWhiteSpace(SelectedOutputDevice.Status)
            ? $"{SelectedOutputDevice.Name} is not available."
            : SelectedOutputDevice.Status
        : null;
    public string? InputDeviceUnavailableMessage => OutputDeviceUnavailableMessage;

    public string StatusTitle
    {
        get
        {
            if (IsCancelling) return "Cancelling";
            if (IsCapturing) return "Capturing";
            if (IsTesting) return "Testing";
            if (IsRefreshingDevices) return "Refreshing Devices";
            if (CaptureWarningMessage is not null) return "Saved with Warning";
            if (Assessment.StartsWith("Capture failed:", StringComparison.Ordinal)) return "Capture Failed";
            if (Assessment.StartsWith("The WAV source", StringComparison.Ordinal)) return "Source Error";
            if (CanCapture) return "Ready";
            if (SelectedOutputDevice is null || !AudioReady) return "Select Device";
            if (!SourceReady) return "Choose Source";
            return "Run Test";
        }
    }

    public string StatusMessage
    {
        get
        {
            if (IsCancelling) return "Stopping the isolated audio worker...";
            if (IsRefreshingDevices) return "Refreshing ASIO devices...";
            if (IsCapturing || IsTesting)
            {
                var remaining = RemainingSeconds is > 0 ? $" {FormatDuration(RemainingSeconds.Value)} left" : string.Empty;
                var percent = ProgressFraction is not null ? $" {Math.Floor(ProgressPercent):0}%" : string.Empty;
                return $"{ProgressLabel}{percent}{remaining}";
            }
            if (Assessment.StartsWith("The WAV source", StringComparison.Ordinal)) return Assessment;
            if (OutputDeviceUnavailableMessage is not null) return OutputDeviceUnavailableMessage;
            if (SelectedOutputDevice is null) return "Choose an ASIO driver.";
            if (!AudioReady) return "Choose playback and recording channels.";
            if (!SourceReady) return "Choose a WAV source.";
            if (CaptureWarningMessage is not null) return CaptureWarningMessage;
            if (Assessment == "Capture saved." && LastOutputPath is not null)
            {
                return $"Saved {Path.GetFileName(LastOutputPath)}.";
            }
            if (Assessment == "Capture cancelled." || Assessment.StartsWith("Capture failed:", StringComparison.Ordinal))
            {
                return Assessment;
            }
            if (Assessment == "Test cancelled."
                || Assessment.StartsWith("Test timed out", StringComparison.Ordinal)
                || Assessment.StartsWith("Test failed:", StringComparison.Ordinal))
            {
                return Assessment;
            }
            if (!CaptureSetupVerified) return "Run the test to enable capture.";
            return "Ready to capture.";
        }
    }

    public StatusKind StatusKind
    {
        get
        {
            if (IsTesting || IsCapturing || IsCancelling || IsRefreshingDevices) return StatusKind.Working;
            if (Assessment.StartsWith("Capture failed:", StringComparison.Ordinal)) return StatusKind.Error;
            if (Assessment.StartsWith("The WAV source", StringComparison.Ordinal)) return StatusKind.Error;
            if (CanCapture)
            {
                return StabilityLevel == CaptureStabilityLevel.Caution || CaptureWarningMessage is not null
                    ? StatusKind.Warning
                    : StatusKind.Ready;
            }
            if (_deviceDiscoveryFailed
                || OutputDeviceUnavailableMessage is not null
                || StabilityLevel is CaptureStabilityLevel.Unstable or CaptureStabilityLevel.Failed
                || Assessment.StartsWith("Test failed:", StringComparison.Ordinal)
                || Assessment.StartsWith("Test timed out", StringComparison.Ordinal)
                || Assessment.StartsWith("Capture failed:", StringComparison.Ordinal))
            {
                return StatusKind.Error;
            }
            return StatusKind.Neutral;
        }
    }

    public string VerificationFooterText
    {
        get
        {
            if (IsTesting) return "Testing...";
            if (StabilityReason is not null) return StabilityReason;
            return StabilityLevel switch
            {
                CaptureStabilityLevel.Unknown => "Run a test to enable capture.",
                CaptureStabilityLevel.Excellent => "Excellent stability. Ready to capture.",
                CaptureStabilityLevel.Good => "Stable. Ready to capture.",
                CaptureStabilityLevel.Caution => "Capture may need adjustment.",
                CaptureStabilityLevel.Unstable => "Capture is unstable. Adjust setup and test again.",
                CaptureStabilityLevel.Failed => Assessment,
                _ => Assessment,
            };
        }
    }

    public async Task InitializeAsync()
    {
        var sourceLoad = LoadSavedSourceAsync();
        await RefreshDevicesAsync(preserveVerification: true);
        await sourceLoad;
    }

    private async Task LoadSavedSourceAsync()
    {
        var path = SourcePath;
        var selectionVersion = _sourceSelectionVersion;
        if (path is null)
        {
            return;
        }

        try
        {
            var snapshot = await Task.Run(() => WavMetadataReader.ReadMetadataSnapshot(path));
            if (!_disposed
                && selectionVersion == _sourceSelectionVersion
                && string.Equals(SourcePath, path, StringComparison.OrdinalIgnoreCase))
            {
                SetSourceSnapshot(snapshot);
            }
        }
        catch (Exception exception)
        {
            if (!_disposed
                && selectionVersion == _sourceSelectionVersion
                && string.Equals(SourcePath, path, StringComparison.OrdinalIgnoreCase))
            {
                SetSourceSnapshot(null);
                Assessment = $"The WAV source is no longer available: {exception.Message}";
            }
        }
    }

    public async Task RefreshDevicesAsync(bool preserveVerification = false)
    {
        if (ControlsLocked || IsRefreshingDevices)
        {
            return;
        }

        IsRefreshingDevices = true;
        _deviceCancellation?.Cancel();
        _deviceCancellation?.Dispose();
        var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        _deviceCancellation = cancellation;
        try
        {
            var discovered = await _worker.GetDevicesAsync(cancellation.Token);
            var visible = discovered
                .Where(device => device.Id.StartsWith("asio:", StringComparison.OrdinalIgnoreCase)
                    || (_showDevelopmentDevices && device.Id.StartsWith("fake:", StringComparison.OrdinalIgnoreCase)))
                .ToList();

            var desiredId = SelectedOutputDevice?.Id ?? _settings.Device?.Id;
            if (!string.IsNullOrWhiteSpace(desiredId)
                && visible.All(device => !string.Equals(device.Id, desiredId, StringComparison.OrdinalIgnoreCase)))
            {
                visible.Add(new AudioDeviceInfo(
                    desiredId,
                    _settings.Device?.Name ?? "Selected driver",
                    0,
                    0,
                    0,
                    false,
                    "The saved ASIO driver is not installed."));
            }

            _deviceDiscoveryFailed = false;
            _suppressConfigurationChanges = true;
            try
            {
                Devices.Clear();
                foreach (var device in visible)
                {
                    Devices.Add(device);
                }

                SelectedOutputDevice = visible.FirstOrDefault(device =>
                    string.Equals(device.Id, desiredId, StringComparison.OrdinalIgnoreCase))
                    ?? visible.FirstOrDefault(device => device.CanCapture);
            }
            finally
            {
                _suppressConfigurationChanges = false;
            }

            await ReloadChannelsAsync();
            if (!preserveVerification)
            {
                InvalidateVerification("Device setup refreshed. Test again.");
            }
            SaveSettings();
        }
        catch (OperationCanceledException)
        {
            if (!_disposed)
            {
                _deviceDiscoveryFailed = true;
                InvalidateVerification("ASIO device discovery timed out. Refresh devices to retry.");
                RaiseError("ASIO device discovery timed out. A driver may be unresponsive.");
            }
        }
        catch (Exception exception)
        {
            _deviceDiscoveryFailed = true;
            InvalidateVerification("Could not refresh ASIO devices.");
            RaiseError($"Could not refresh ASIO devices: {exception.Message}");
        }
        finally
        {
            if (ReferenceEquals(_deviceCancellation, cancellation))
            {
                _deviceCancellation = null;
                cancellation.Dispose();
            }
            IsRefreshingDevices = false;
            NotifyStateChanged();
        }
    }

    private void ChooseSource()
    {
        try
        {
            var path = _fileDialogs.ChooseSourceWav();
            if (path is null)
            {
                return;
            }
            var snapshot = WavMetadataReader.ReadMetadataSnapshot(path);
            var changed = _sourceSnapshot is null || !SourceSnapshotsMatch(_sourceSnapshot, snapshot);
            _sourceSelectionVersion++;
            SourcePath = snapshot.FullPath;
            SetSourceSnapshot(snapshot);
            _settings = _settings with { SourcePath = SourcePath };
            SaveSettings();
            if (changed)
            {
                InvalidateVerification("Source changed. Test again.");
            }
        }
        catch (Exception exception)
        {
            RaiseError($"Could not open the WAV source: {exception.Message}");
        }
    }

    private async Task RunSetupTestAsync()
    {
        if (!CanTest || SelectedOutputDevice is null || SelectedPlaybackChannel is null
            || SelectedRecordChannel is null || _sourceMetadata is null)
        {
            return;
        }

        var operationCancellation = new CancellationTokenSource(_setupTestTimeout);
        _operationCancellation = operationCancellation;
        var operationToken = operationCancellation.Token;
        BeginSetupTest();
        try
        {
            var sourceSnapshot = await RevalidateSourceAsync(operationToken);
            if (sourceSnapshot is null)
            {
                return;
            }
            var request = new SetupTestRequest(
                SelectedOutputDevice.Id,
                SelectedPlaybackChannel.Index,
                SelectedRecordChannel.Index,
                OutputTrimDb,
                InputTrimDb,
                sourceSnapshot.Metadata.SampleRate);
            var progress = new Progress<WorkerProgress>(value =>
            {
                if (ReferenceEquals(_operationCancellation, operationCancellation)
                    && !operationToken.IsCancellationRequested
                    && IsTesting)
                {
                    ApplyProgress(value, isTest: true);
                }
            });
            var result = await _worker.TestAsync(request, progress, operationToken);
            operationToken.ThrowIfCancellationRequested();
            if (await ReadSourceIfUnchangedAsync(sourceSnapshot, operationToken) is null)
            {
                return;
            }
            ValidateSetupTestResult(result, sourceSnapshot.Metadata.SampleRate);
            ApplySetupTestResult(result);
        }
        catch (OperationCanceledException)
        {
            var message = _disposed
                ? "Test cancelled."
                : $"Test timed out after {_setupTestTimeout.TotalSeconds:0} seconds.";
            InvalidateVerification(message);
            if (!_disposed)
            {
                StabilityLevel = CaptureStabilityLevel.Failed;
                StabilityReason = message;
                RaiseError(message);
            }
        }
        catch (Exception exception)
        {
            var message = $"Test failed: {exception.Message}";
            InvalidateVerification(message);
            StabilityLevel = CaptureStabilityLevel.Failed;
            StabilityReason = message;
            RaiseError(message);
        }
        finally
        {
            operationCancellation.Dispose();
            if (ReferenceEquals(_operationCancellation, operationCancellation))
            {
                _operationCancellation = null;
            }
            IsTesting = false;
            ProgressFraction = null;
            RemainingSeconds = null;
            NotifyStateChanged();
        }
    }

    private void CaptureOrCancel()
    {
        if (IsCapturing)
        {
            CancelCapture();
            return;
        }

        _ = ObserveAsync(RunCaptureAsync());
    }

    private async Task RunCaptureAsync()
    {
        if (!CanCapture || SelectedOutputDevice is null || SelectedPlaybackChannel is null
            || SelectedRecordChannel is null || SourcePath is null || _sourceSnapshot is null)
        {
            return;
        }

        var defaultName = $"{Path.GetFileNameWithoutExtension(SourcePath)}-captured.wav";
        var destinationPath = _fileDialogs.ChooseCaptureDestination(defaultName);
        if (destinationPath is null)
        {
            return;
        }

        var fullDestination = Path.GetFullPath(destinationPath);
        var directory = Path.GetDirectoryName(fullDestination)
            ?? throw new InvalidOperationException("The capture destination has no parent directory.");
        var temporaryPath = Path.Combine(
            directory,
            $".capture-panel-{Guid.NewGuid():N}.tmp.wav");

        var operationCancellation = new CancellationTokenSource();
        _operationCancellation = operationCancellation;
        var operationToken = operationCancellation.Token;
        var validatedOutput = false;
        var promotedOutput = false;
        IsCapturing = true;
        IsCancelling = false;
        CaptureWarningMessage = null;
        RecoveryOutputPath = null;
        ProgressFraction = 0;
        ProgressLabel = "Preparing...";
        RemainingSeconds = null;
        Assessment = "Capture started.";
        try
        {
            var sourceSnapshot = await RevalidateSourceAsync(operationToken);
            if (sourceSnapshot is null)
            {
                return;
            }
            if (WavMetadataReader.RefersToSameFile(sourceSnapshot, fullDestination))
            {
                ProgressLabel = "Failed";
                var sameFileMessage = "Capture failed: The destination must not overwrite the source WAV.";
                Assessment = sameFileMessage;
                RaiseError(sameFileMessage);
                return;
            }

            var request = new CaptureRequest(
                sourceSnapshot.FullPath,
                temporaryPath,
                SelectedOutputDevice.Id,
                SelectedPlaybackChannel.Index,
                SelectedRecordChannel.Index,
                OutputTrimDb,
                InputTrimDb);
            var progress = new Progress<WorkerProgress>(value =>
            {
                if (ReferenceEquals(_operationCancellation, operationCancellation)
                    && !operationToken.IsCancellationRequested
                    && IsCapturing)
                {
                    ApplyProgress(value, isTest: false);
                }
            });
            var completed = await _worker.CaptureAsync(request, progress, operationToken);
            operationToken.ThrowIfCancellationRequested();
            ValidateCaptureCompleted(completed, temporaryPath, sourceSnapshot.Metadata);
            if (await ReadSourceIfUnchangedAsync(sourceSnapshot, operationToken) is null)
            {
                return;
            }
            validatedOutput = true;

            File.Move(temporaryPath, fullDestination, overwrite: true);
            promotedOutput = true;
            LastOutputPath = fullDestination;
            CaptureWarningMessage = completed.Warnings.Count == 0
                ? null
                : $"Saved {Path.GetFileName(fullDestination)} with warning: {completed.Warnings[0].Message}";
            ProgressLabel = "Saved";
            Assessment = "Capture saved.";
        }
        catch (OperationCanceledException)
        {
            ProgressLabel = "Cancelled";
            Assessment = "Capture cancelled.";
        }
        catch (Exception) when (operationToken.IsCancellationRequested && !validatedOutput)
        {
            // A worker teardown can report its own I/O or driver error after cancellation wins
            // the race. Until an output has been validated, the user's cancellation is the
            // authoritative outcome and must not invalidate the previously verified route.
            ProgressLabel = "Cancelled";
            Assessment = "Capture cancelled.";
        }
        catch (Exception exception)
        {
            ProgressLabel = "Failed";
            var recovery = validatedOutput && File.Exists(temporaryPath)
                ? temporaryPath
                : null;
            RecoveryOutputPath = recovery;
            var message = recovery is null
                ? $"Capture failed: {exception.Message}"
                : $"Capture failed: {exception.Message} The validated capture was preserved at '{recovery}'.";
            if (!validatedOutput)
            {
                InvalidateVerification(message);
                StabilityLevel = CaptureStabilityLevel.Failed;
                StabilityReason = message;
            }
            else
            {
                Assessment = message;
            }
            RaiseError(message);
        }
        finally
        {
            TryDeleteNativeSiblingTemporaryFiles(temporaryPath);
            if (!validatedOutput || promotedOutput)
            {
                TryDelete(temporaryPath);
            }
            operationCancellation.Dispose();
            if (ReferenceEquals(_operationCancellation, operationCancellation))
            {
                _operationCancellation = null;
            }
            IsCapturing = false;
            IsCancelling = false;
            ProgressFraction = null;
            RemainingSeconds = null;
            NotifyStateChanged();
        }
    }

    private void CancelCapture()
    {
        if (!CanCancelCapture)
        {
            return;
        }

        IsCancelling = true;
        ProgressLabel = "Cancelling...";
        RemainingSeconds = null;
        _operationCancellation?.Cancel();
    }

    private void BeginSetupTest()
    {
        IsTesting = true;
        CaptureSetupVerified = false;
        CaptureWarningMessage = null;
        RecoveryOutputPath = null;
        SetClippingDetected(false);
        OutputPeakDb = null;
        InputPeakDb = null;
        LatencyMilliseconds = null;
        StabilityLevel = CaptureStabilityLevel.Unknown;
        StabilityReason = null;
        ProgressFraction = 0;
        ProgressLabel = "Testing...";
        RemainingSeconds = null;
        Assessment = "Testing...";
    }

    private void ApplySetupTestResult(SetupTestResult result)
    {
        SetClippingDetected(HasCode(result.Failures, "digital_clipping"));
        OutputPeakDb = result.OutputPeakDbfs;
        InputPeakDb = result.InputPeakDbfs;
        LatencyMilliseconds = result.LatencyMilliseconds;
        StabilityLevel = CalculateStability(result);
        StabilityReason = StabilityReasonFor(result);
        CaptureSetupVerified = result.Passed;

        if (HasCode(result.Failures, "digital_clipping"))
        {
            Assessment = "Clipping detected. Lower output or input, then test again.";
        }
        else if (result.Passed)
        {
            Assessment = result.Warnings.FirstOrDefault()?.Message ?? "Ready to capture.";
        }
        else
        {
            Assessment = result.Failures.FirstOrDefault()?.Message ?? "Could not verify this setup.";
        }

        ProgressFraction = null;
        RemainingSeconds = null;
        OnPropertiesChanged(
            nameof(IsClipping),
            nameof(LevelText),
            nameof(LevelState),
            nameof(VerificationFooterText));
    }

    private void ApplyProgress(WorkerProgress progress, bool isTest)
    {
        var stage = progress.Stage.Trim().ToLowerInvariant();
        var recordingFraction = Math.Clamp(progress.Fraction ?? 0, 0, 1);
        ProgressFraction = stage switch
        {
            "sample_rate_configuration" => 0.01,
            "recording" => recordingFraction * 0.88,
            "alignment" => 0.90,
            "verification" => 0.96,
            "output_writing" => 0.98,
            "complete" => 1.0,
            _ => ProgressFraction,
        };
        ProgressLabel = isTest
            ? "Testing..."
            : stage switch
            {
                "sample_rate_configuration" => "Preparing...",
                "recording" => "Recording...",
                "alignment" => "Aligning...",
                "verification" => "Verifying...",
                "output_writing" => "Saving...",
                _ => progress.Message ?? "Working...",
            };
        RemainingSeconds = progress.RemainingSeconds;
    }

    private async Task ReloadChannelsAsync()
    {
        _channelCancellation?.Cancel();
        _channelCancellation?.Dispose();
        var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        _channelCancellation = cancellation;
        var token = cancellation.Token;
        var selectedDevice = SelectedOutputDevice;

        try
        {
            _suppressConfigurationChanges = true;
            try
            {
                PlaybackChannels.Clear();
                RecordChannels.Clear();
                SelectedPlaybackChannel = null;
                SelectedRecordChannel = null;
            }
            finally
            {
                _suppressConfigurationChanges = false;
            }

            if (selectedDevice?.Available != true)
            {
                return;
            }

            var channels = await _worker.GetChannelsAsync(selectedDevice.Id, token);
            token.ThrowIfCancellationRequested();
            if (!string.Equals(
                    SelectedOutputDevice?.Id,
                    selectedDevice.Id,
                    StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            _suppressConfigurationChanges = true;
            try
            {
                foreach (var channel in channels.Outputs)
                {
                    PlaybackChannels.Add(channel);
                }
                foreach (var channel in channels.Inputs)
                {
                    RecordChannels.Add(channel);
                }

                SelectedPlaybackChannel = PlaybackChannels.FirstOrDefault(channel =>
                    channel.Index == _settings.PlaybackChannel) ?? PlaybackChannels.FirstOrDefault();
                SelectedRecordChannel = RecordChannels.FirstOrDefault(channel =>
                    channel.Index == _settings.RecordChannel) ?? RecordChannels.FirstOrDefault();
            }
            finally
            {
                _suppressConfigurationChanges = false;
            }

            _settings = _settings with
            {
                PlaybackChannel = SelectedPlaybackChannel?.Index,
                RecordChannel = SelectedRecordChannel?.Index,
            };
            SaveSettings();
        }
        catch (OperationCanceledException) when (!ReferenceEquals(_channelCancellation, cancellation))
        {
        }
        catch (OperationCanceledException)
        {
            if (!_disposed)
            {
                InvalidateVerification("ASIO channel discovery timed out. Select or refresh the driver to retry.");
                RaiseError("ASIO channel discovery timed out. The selected driver may be unresponsive.");
            }
        }
        catch (Exception exception)
        {
            InvalidateVerification("Could not load ASIO channels. Select or refresh the driver to retry.");
            RaiseError($"Could not load ASIO channels: {exception.Message}");
        }
        finally
        {
            if (ReferenceEquals(_channelCancellation, cancellation))
            {
                _channelCancellation = null;
                cancellation.Dispose();
            }
            NotifyStateChanged();
        }
    }

    private void LoadSelectedDeviceChannelsInBackground() => _ = ObserveAsync(ReloadChannelsAsync());

    private void InvalidateVerification(string message)
    {
        CaptureSetupVerified = false;
        CaptureWarningMessage = null;
        RecoveryOutputPath = null;
        SetClippingDetected(false);
        OutputPeakDb = null;
        InputPeakDb = null;
        LatencyMilliseconds = null;
        StabilityLevel = CaptureStabilityLevel.Unknown;
        StabilityReason = null;
        Assessment = message;
        OnPropertiesChanged(nameof(IsClipping), nameof(LevelText), nameof(LevelState));
    }

    private async Task<WavFileSnapshot?> RevalidateSourceAsync(CancellationToken cancellationToken)
    {
        var path = SourcePath;
        var expected = _sourceSnapshot;
        if (path is null || expected is null)
        {
            InvalidateVerification("Choose a valid WAV source and test again.");
            return null;
        }

        return await ReadSourceIfUnchangedAsync(expected, cancellationToken);
    }

    private async Task<WavFileSnapshot?> ReadSourceIfUnchangedAsync(
        WavFileSnapshot expected,
        CancellationToken cancellationToken)
    {
        WavFileSnapshot current;
        try
        {
            current = await WavMetadataReader.ReadSnapshotAsync(
                expected.FullPath,
                cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception exception)
        {
            SetSourceSnapshot(null);
            var message = $"The WAV source is no longer available: {exception.Message}";
            InvalidateVerification(message);
            RaiseError(message);
            return null;
        }

        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(SourcePath, expected.FullPath, StringComparison.OrdinalIgnoreCase)
            || !SourceSnapshotsMatch(expected, current))
        {
            SetSourceSnapshot(current);
            var message = "The WAV source changed on disk. Review it and run Test again.";
            InvalidateVerification(message);
            RaiseError(message);
            return null;
        }
        if (expected.ContentSha256.Length == 0)
        {
            SetSourceSnapshot(current);
        }
        return current;
    }

    private void SetSourceSnapshot(WavFileSnapshot? snapshot)
    {
        _sourceSnapshot = snapshot;
        _sourceMetadata = snapshot?.Metadata;
        OnPropertiesChanged(nameof(SourceReady), nameof(SourceDescription));
        NotifyStateChanged();
    }

    private static bool SourceSnapshotsMatch(WavFileSnapshot expected, WavFileSnapshot current)
        => string.Equals(expected.FullPath, current.FullPath, StringComparison.OrdinalIgnoreCase)
            && expected.Metadata == current.Metadata
            && expected.FileLength == current.FileLength
            && expected.LastWriteTimeUtc == current.LastWriteTimeUtc
            && expected.Identity == current.Identity
            && (expected.ContentSha256.Length == 0
                || string.Equals(
                    expected.ContentSha256,
                    current.ContentSha256,
                    StringComparison.Ordinal));

    private static void ValidateSetupTestResult(SetupTestResult result, int expectedSampleRate)
    {
        if (result.Failures is null || result.Warnings is null
            || result.Passed != (result.Failures.Count == 0))
        {
            throw new InvalidDataException("The setup-test pass flag disagrees with its diagnostics.");
        }
        if (!double.IsFinite(result.SampleRate)
            || result.SampleRate <= 0
            || Math.Abs(result.SampleRate - expectedSampleRate) > 0.5)
        {
            throw new InvalidDataException("The setup test returned an unexpected sample rate.");
        }
        if (!IsFiniteInRange(result.OutputPeakDbfs, -1_000, 1_000)
            || !IsFiniteInRange(result.InputPeakDbfs, -1_000, 1_000)
            || result.LatencyMilliseconds is { } latency
                && !IsFiniteInRange(latency, -3_600_000, 3_600_000)
            || result.TimingErrorFrames is { } timingError
                && !IsFiniteInRange(timingError, 0, long.MaxValue)
            || result.Reliability is not ("reliable" or "ambiguous" or "unmeasurable"))
        {
            throw new InvalidDataException("The setup test returned invalid measurements.");
        }
    }

    private static void ValidateCaptureCompleted(
        CaptureCompleted completed,
        string expectedOutputPath,
        WavMetadata sourceMetadata)
    {
        if (completed.Warnings is null)
        {
            throw new InvalidDataException("The capture result has no warning collection.");
        }
        if (!string.Equals(
                Path.GetFullPath(completed.OutputPath),
                Path.GetFullPath(expectedOutputPath),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("The audio worker reported a different output path.");
        }
        if (!File.Exists(expectedOutputPath))
        {
            throw new IOException("The audio worker completed without writing its output file.");
        }

        var fileLength = new FileInfo(expectedOutputPath).Length;
        if (completed.FileSize <= 0 || fileLength != completed.FileSize)
        {
            throw new InvalidDataException("The captured file size does not match the worker result.");
        }
        if (!IsFiniteInRange(
                completed.SampleRate,
                CaptureLimits.MinimumSampleRate,
                CaptureLimits.MaximumSampleRate)
            || !IsFiniteInRange(completed.ElapsedSeconds, 0, 7 * 24 * 60 * 60)
            || completed.Channels <= 0
            || completed.BitDepth is not (16 or 24 or 32)
            || completed.TargetFrames <= 0
            || completed.TrimmedFrames < 0
            || completed.TrimmedFrames > completed.TargetFrames)
        {
            throw new InvalidDataException("The audio worker returned invalid capture metadata.");
        }

        var outputMetadata = WavMetadataReader.Read(expectedOutputPath);
        if (completed.Channels != 1
            || completed.BitDepth != sourceMetadata.BitsPerSample
            || outputMetadata.Channels != completed.Channels
            || outputMetadata.BitsPerSample != completed.BitDepth
            || Math.Abs(outputMetadata.SampleRate - completed.SampleRate) > 0.5
            || outputMetadata.Frames != completed.TargetFrames
            || completed.TargetFrames != sourceMetadata.Frames
            || Math.Abs(completed.SampleRate - sourceMetadata.SampleRate) > 0.5)
        {
            throw new InvalidDataException("The captured WAV does not match the worker result or source format.");
        }
    }

    private static bool IsFiniteInRange(double value, double minimum, double maximum)
        => double.IsFinite(value) && value >= minimum && value <= maximum;

    private void SetClippingDetected(bool value)
    {
        if (_clippingDetected == value)
        {
            return;
        }

        _clippingDetected = value;
        OnPropertiesChanged(nameof(IsClipping), nameof(LevelText), nameof(LevelState));
    }

    private void SaveSettings()
    {
        try
        {
            var device = SelectedOutputDevice is null
                ? _settings.Device
                : new SavedDevice(SelectedOutputDevice.Id, SelectedOutputDevice.Name);
            _settings = _settings with
            {
                SourcePath = SourcePath,
                Device = device,
                PlaybackChannel = SelectedPlaybackChannel?.Index ?? _settings.PlaybackChannel,
                RecordChannel = SelectedRecordChannel?.Index ?? _settings.RecordChannel,
                OutputTrimDb = OutputTrimDb,
                InputTrimDb = InputTrimDb,
            };
            _settingsStore.Save(_settings);
        }
        catch (Exception exception)
        {
            RaiseError($"Could not save settings: {exception.Message}");
        }
    }

    private void NotifyStateChanged()
    {
        OnPropertiesChanged(
            nameof(ControlsLocked),
            nameof(AudioReady),
            nameof(CanTest),
            nameof(CanCapture),
            nameof(CanCancelCapture),
            nameof(StatusTitle),
            nameof(StatusMessage),
            nameof(StatusKind));
        ChooseSourceCommand.NotifyCanExecuteChanged();
        RefreshDevicesCommand.NotifyCanExecuteChanged();
        TestCommand.NotifyCanExecuteChanged();
        CaptureCommand.NotifyCanExecuteChanged();
    }

    private void RaiseError(string message)
    {
        ErrorRaised?.Invoke(this, message);
        NotifyStateChanged();
    }

    private void ReportUnexpectedError(Exception exception) => RaiseError(exception.Message);

    private async Task ObserveAsync(Task operation)
    {
        try
        {
            await operation;
        }
        catch (Exception exception)
        {
            ReportUnexpectedError(exception);
        }
    }

    private static CaptureStabilityLevel CalculateStability(SetupTestResult result)
    {
        if (HasCode(result.Failures, "digital_clipping")
            || HasCode(result.Failures, "verification_signal_missing"))
        {
            return CaptureStabilityLevel.Failed;
        }

        if (result.TimingErrorFrames is null || result.SampleRate <= 0)
        {
            return result.Failures.Count == 0
                ? CaptureStabilityLevel.Unknown
                : CaptureStabilityLevel.Failed;
        }

        var milliseconds = result.TimingErrorFrames.Value / result.SampleRate * 1000;
        var level = milliseconds switch
        {
            <= 0.5 => CaptureStabilityLevel.Excellent,
            <= 2 => CaptureStabilityLevel.Good,
            <= 5 => CaptureStabilityLevel.Caution,
            _ => CaptureStabilityLevel.Unstable,
        };

        if (HasCode(result.Failures, "verification_timing_mismatch"))
        {
            level = Max(level, CaptureStabilityLevel.Unstable);
        }
        else if (result.Failures.Count > 0)
        {
            level = CaptureStabilityLevel.Failed;
        }

        var stabilityWarningCodes = new[]
        {
            "equipment_decay_may_affect_capture",
            "marker_evidence_low",
            "alignment_fit_error_high",
            "verification_ambiguous",
        };
        if (result.Warnings.Any(warning => stabilityWarningCodes.Contains(warning.Code)))
        {
            level = Max(level, CaptureStabilityLevel.Caution);
        }
        return level;
    }

    private static string? StabilityReasonFor(SetupTestResult result)
    {
        var priorityCodes = new[]
        {
            "verification_signal_missing",
            "verification_timing_mismatch",
            "equipment_decay_may_affect_capture",
            "verification_ambiguous",
            "alignment_fit_error_high",
            "marker_evidence_low",
        };
        foreach (var code in priorityCodes)
        {
            var diagnostic = result.Failures.Concat(result.Warnings)
                .FirstOrDefault(item => string.Equals(item.Code, code, StringComparison.Ordinal));
            if (diagnostic is not null)
            {
                return diagnostic.Message;
            }
        }
        return null;
    }

    private static bool HasCode(IEnumerable<DiagnosticInfo> diagnostics, string code)
        => diagnostics.Any(item => string.Equals(item.Code, code, StringComparison.Ordinal));

    private static CaptureStabilityLevel Max(CaptureStabilityLevel left, CaptureStabilityLevel right)
        => left >= right ? left : right;

    private static string FormatDbfs(double? value)
        => value is null ? "-inf dBFS" : $"{value.Value:0.0} dBFS";

    private static double MeterPercent(double? value)
        => value is null ? 2 : (Math.Clamp(value.Value, -60, 0) + 60) / 60 * 100;

    private static MeterState MeterStateFor(double? value)
        => value switch
        {
            null => MeterState.Empty,
            > -1 => MeterState.Clipping,
            > -6 => MeterState.Hot,
            _ => MeterState.Normal,
        };

    private static string MiddleTruncate(string value, int maximumLength)
    {
        if (value.Length <= maximumLength)
        {
            return value;
        }

        var remaining = maximumLength - 1;
        var prefixLength = (remaining + 1) / 2;
        var suffixLength = remaining - prefixLength;
        return string.Concat(value.AsSpan(0, prefixLength), "…", value.AsSpan(value.Length - suffixLength));
    }

    private static string FormatDuration(double seconds)
    {
        var rounded = TimeSpan.FromSeconds(Math.Max(0, Math.Ceiling(seconds)));
        return rounded.TotalHours >= 1
            ? $"{(int)rounded.TotalHours}:{rounded.Minutes:00}:{rounded.Seconds:00}"
            : $"{(int)rounded.TotalMinutes}:{rounded.Seconds:00}";
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void TryDeleteNativeSiblingTemporaryFiles(string outputPath)
    {
        var directory = Path.GetDirectoryName(outputPath);
        if (directory is null)
        {
            return;
        }

        var prefix = Path.GetFileName(outputPath) + ".capture-panel.tmp.";
        try
        {
            foreach (var candidate in Directory.EnumerateFiles(
                         directory,
                         prefix + "*",
                         SearchOption.TopDirectoryOnly))
            {
                if (Path.GetFileName(candidate).StartsWith(
                        prefix,
                        StringComparison.OrdinalIgnoreCase))
                {
                    TryDelete(candidate);
                }
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        _operationCancellation?.Cancel();
        _operationCancellation?.Dispose();
        _deviceCancellation?.Cancel();
        _deviceCancellation?.Dispose();
        _channelCancellation?.Cancel();
        _channelCancellation?.Dispose();
    }
}
