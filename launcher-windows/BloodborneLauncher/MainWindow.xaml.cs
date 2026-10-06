using Microsoft.Win32;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media.Imaging;

namespace BloodborneLauncher;

public partial class MainWindow : Window
{
    sealed class LauncherState
    {
        public string GamePath { get; set; } = "";
        public string Resolution { get; set; } = "1920x1080";
        public bool Fullscreen { get; set; } = true;
        public bool ShowFps { get; set; }
        public string PresentMode { get; set; } = "";
        public string Upscaler { get; set; } = "fsr3";
        public string Preset { get; set; } = "1";
        public string Fps { get; set; } = "60";
        public string Language { get; set; } = "1";
        public string LiveResolution { get; set; } = "0";
        public string ModelLod { get; set; } = "0";
        public bool Sharpen { get; set; } = true;
        public bool Chromatic { get; set; } = true;
        public bool Dof { get; set; } = true;
        public bool MotionBlur { get; set; } = true;
        public bool Ssao { get; set; } = true;
        public bool GameAa { get; set; } = true;
        public bool Shadows { get; set; } = true;
        public bool Ssr { get; set; }
        public bool SkipIntro { get; set; }
        public int GamepadIndex { get; set; }
        public bool ModsEnabled { get; set; } = true;
        public bool DeveloperMode { get; set; }
        public bool DetailedLogs { get; set; }
        public bool FrameStats { get; set; }
        public bool AudioStats { get; set; }
        public bool FsrProfile { get; set; }
    }

    readonly string root;
    readonly string statePath;
    LauncherState state = new();
    bool syncing;

    public MainWindow()
    {
        InitializeComponent();
        root = FindProjectRoot();
        statePath = Path.Combine(root, "launcher-settings.json");
        ModsFolderText.Text = Path.Combine(root, "mods");
        LoadState();
        ApplyStateToUi();
        InitializePresentation();
        LoadHeroImage();
        RefreshStatus();
    }

    static string FindProjectRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir != null; ++i, dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "run.bat")) &&
                File.Exists(Path.Combine(dir.FullName, "build.sh")))
                return dir.FullName;
        }
        return AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    }

    void LoadState()
    {
        try
        {
            if (File.Exists(statePath))
                state = JsonSerializer.Deserialize<LauncherState>(File.ReadAllText(statePath)) ?? new();
            else
            {
                string remembered = Path.Combine(root, "out", "game_dir.txt");
                if (File.Exists(remembered)) state.GamePath = File.ReadAllText(remembered).Trim();
            }
        }
        catch { state = new(); }
    }

    void ApplyStateToUi()
    {
        syncing = true;
        PathBox.Text = state.GamePath;
        SetComboContent(ResolutionCombo, state.Resolution);
        SetComboContent(QuickResolution, state.Resolution);
        SetComboTag(PresentCombo, state.PresentMode);
        FullscreenCheck.IsChecked = state.Fullscreen;
        QuickDisplay.SelectedIndex = state.Fullscreen ? 0 : 1;
        ShowFpsCheck.IsChecked = state.ShowFps;

        SetComboTag(UpscalerCombo, state.Upscaler);
        SetQuickUpscaler(state.Upscaler);
        SetComboTag(PresetCombo, state.Preset);
        QuickPreset.SelectedIndex = int.TryParse(state.Preset, out int p) ? Math.Clamp(p, 0, 4) : 1;
        SetComboTag(FpsCombo, state.Fps);
        SetQuickFps(state.Fps);

        SetComboTag(LanguageCombo, state.Language);
        SetComboTag(LiveResolutionCombo, state.LiveResolution);
        SetComboTag(ModelLodCombo, state.ModelLod);
        SharpenCheck.IsChecked = state.Sharpen;
        ChromaticCheck.IsChecked = state.Chromatic;
        DofCheck.IsChecked = state.Dof;
        MotionBlurCheck.IsChecked = state.MotionBlur;
        SsaoCheck.IsChecked = state.Ssao;
        GameAaCheck.IsChecked = state.GameAa;
        ShadowsCheck.IsChecked = state.Shadows;
        SsrCheck.IsChecked = state.Ssr;
        SkipIntroCheck.IsChecked = state.SkipIntro;

        ControllerCombo.SelectedIndex = Math.Clamp(state.GamepadIndex, 0, 3);
        ModsEnabledCheck.IsChecked = state.ModsEnabled;
        DeveloperModeCheck.IsChecked = state.DeveloperMode;
        DetailedLogsCheck.IsChecked = state.DetailedLogs;
        FrameStatsCheck.IsChecked = state.FrameStats;
        AudioStatsCheck.IsChecked = state.AudioStats;
        FsrProfileCheck.IsChecked = state.FsrProfile;
        syncing = false;
    }

    void CollectUi()
    {
        state.GamePath = PathBox.Text.Trim();
        state.Resolution = ComboText(ResolutionCombo, "1920x1080");
        state.Fullscreen = FullscreenCheck.IsChecked == true;
        state.ShowFps = ShowFpsCheck.IsChecked == true;
        state.PresentMode = ComboTag(PresentCombo);
        state.Upscaler = ComboTag(UpscalerCombo, "fsr3");
        state.Preset = ComboTag(PresetCombo, "1");
        state.Fps = ComboTag(FpsCombo, "60");
        state.Language = ComboTag(LanguageCombo, "1");
        state.LiveResolution = ComboTag(LiveResolutionCombo, "0");
        state.ModelLod = ComboTag(ModelLodCombo, "0");
        state.Sharpen = SharpenCheck.IsChecked == true;
        state.Chromatic = ChromaticCheck.IsChecked == true;
        state.Dof = DofCheck.IsChecked == true;
        state.MotionBlur = MotionBlurCheck.IsChecked == true;
        state.Ssao = SsaoCheck.IsChecked == true;
        state.GameAa = GameAaCheck.IsChecked == true;
        state.Shadows = ShadowsCheck.IsChecked == true;
        state.Ssr = SsrCheck.IsChecked == true;
        state.SkipIntro = SkipIntroCheck.IsChecked == true;
        state.GamepadIndex = Math.Max(0, ControllerCombo.SelectedIndex);
        state.ModsEnabled = ModsEnabledCheck.IsChecked == true;
        state.DeveloperMode = DeveloperModeCheck.IsChecked == true;
        state.DetailedLogs = DetailedLogsCheck.IsChecked == true;
        state.FrameStats = FrameStatsCheck.IsChecked == true;
        state.AudioStats = AudioStatsCheck.IsChecked == true;
        state.FsrProfile = FsrProfileCheck.IsChecked == true;
    }

    void SaveSettings()
    {
        CollectUi();
        Directory.CreateDirectory(Path.Combine(root, "out"));
        File.WriteAllText(statePath, JsonSerializer.Serialize(state, new JsonSerializerOptions { WriteIndented = true }));
        if (!string.IsNullOrWhiteSpace(state.GamePath))
            File.WriteAllText(Path.Combine(root, "out", "game_dir.txt"), state.GamePath);

        var ini = new Dictionary<string, string>
        {
            ["upscaler"] = state.Upscaler,
            ["preset"] = state.Preset,
            ["sharpen"] = state.Sharpen ? "1" : "0",
            ["show_fps"] = state.ShowFps ? "1" : "0",
            ["output_res"] = state.Resolution,
            ["fullscreen"] = state.Fullscreen ? "1" : "0",
            ["live_resolution"] = state.LiveResolution,
            ["model_lod"] = state.ModelLod,
            ["effect_chromatic_aberration"] = state.Chromatic ? "1" : "0",
            ["effect_dof"] = state.Dof ? "1" : "0",
            ["effect_motion_blur"] = state.MotionBlur ? "1" : "0",
            ["effect_ssao"] = state.Ssao ? "1" : "0",
            ["effect_game_aa"] = state.GameAa ? "1" : "0",
            ["effect_dynamic_shadows"] = state.Shadows ? "1" : "0",
            ["effect_ssr"] = state.Ssr ? "1" : "0",
            ["skip_intro"] = state.SkipIntro ? "1" : "0"
        };
        RewriteIni(Path.Combine(root, "bbport.ini"), ini);
        FooterMessage.Text = "Settings saved";
        RefreshStatus();
    }

    static void RewriteIni(string path, Dictionary<string, string> values)
    {
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : new List<string>();
        var written = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < lines.Count; ++i)
        {
            string line = lines[i];
            int eq = line.IndexOf('=');
            if (eq <= 0 || line.TrimStart().StartsWith("#")) continue;
            string key = line[..eq].Trim();
            if (values.TryGetValue(key, out string? value))
            {
                lines[i] = key + "=" + value;
                written.Add(key);
            }
        }
        if (lines.Count == 0) lines.Add("# bbport settings - Bloodborne PC Launcher");
        foreach (var kv in values)
            if (!written.Contains(kv.Key)) lines.Add(kv.Key + "=" + kv.Value);
        File.WriteAllText(path, string.Join("\r\n", lines) + "\r\n");
    }

    void RefreshStatus()
    {
        string game = PathBox.Text.Trim();
        bool eboot = File.Exists(Path.Combine(game, "eboot.bin"));
        bool sce = Directory.Exists(Path.Combine(game, "sce_sys"));
        string version = DetectGameVersion(game);
        bool valid = eboot && sce;

        GamePathStatus.Text = valid ? game : "Select Bloodborne folder";
        GameVersionStatus.Text = valid ? version : "Not configured";
        GameVersionStatus.Foreground = valid && version.Contains("1.09") ?
            new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(117, 207, 121)) :
            new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(210, 107, 112));
        PathValidationText.Text = valid ? $"Game dump detected · {version}" : "Choose the folder containing eboot.bin and sce_sys.";
        PlayButton.IsEnabled = valid;
        PlayHint.Text = valid ? "Ready. Settings are saved automatically when Play is pressed." : "Select a valid Bloodborne dump before playing.";

        string probe = Path.Combine(root, "out", "bb-probe.exe");
        RuntimeStatus.Text = File.Exists(probe) ? "Ready" : "Not built";
        RuntimeStatus.Foreground = File.Exists(probe) ? Green() : Red();

        string dlss = Path.Combine(root, "out", "nvngx_dlss.dll");
        if (!File.Exists(dlss)) dlss = Path.Combine(root, "nvngx_dlss.dll");
        DlssStatus.Text = File.Exists(dlss) ? "Available" : "Model DLL not installed";
        DlssStatus.Foreground = File.Exists(dlss) ? Green() : Muted();

        bool fsr411 = Directory.Exists(Path.Combine(root, "fsr4_411"));
        bool fsr4 = Directory.Exists(Path.Combine(root, "fsr4_shaders"));
        Fsr4Status.Text = fsr411 ? "4.1.1 available" : fsr4 ? "v07 available" : "Assets not installed";
        Fsr4Status.Foreground = (fsr411 || fsr4) ? Green() : Muted();

        DetectGpu();
        FooterFps.Text = $"   |   {(state.Fps == "uncap" ? "Unlimited" : state.Fps + " FPS")}";
    }

    static System.Windows.Media.Brush Green() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(117, 207, 121));
    static System.Windows.Media.Brush Red() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(210, 107, 112));
    static System.Windows.Media.Brush Muted() => new System.Windows.Media.SolidColorBrush(System.Windows.Media.Color.FromRgb(142, 150, 158));

    static string DetectGameVersion(string game)
    {
        try
        {
            string sfo = Path.Combine(game, "sce_sys", "param.sfo");
            if (!File.Exists(sfo)) return "Dump found · version unknown";
            string text = Encoding.UTF8.GetString(File.ReadAllBytes(sfo));
            if (text.Contains("01.09", StringComparison.Ordinal)) return "1.09 ✓";
            if (text.Contains("01.08", StringComparison.Ordinal)) return "1.08";
            if (text.Contains("01.00", StringComparison.Ordinal)) return "1.00";
            return "Dump found · verify 1.09";
        }
        catch { return "Dump found · version unknown"; }
    }

    async void DetectGpu()
    {
        string caps = Path.Combine(root, "out", "bb-gpu-capabilities.exe");
        if (!File.Exists(caps))
        {
            GpuStatus.Text = "Vulkan probe not built";
            FooterGpu.Text = "   |   GPU unknown";
            return;
        }
        try
        {
            var psi = new ProcessStartInfo(caps, "--live-resolution")
            {
                WorkingDirectory = root,
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                CreateNoWindow = true
            };
            using var p = Process.Start(psi);
            if (p == null) return;
            string stderr = await p.StandardError.ReadToEndAsync();
            await p.WaitForExitAsync();
            string gpu = stderr.Split('\n')
                .Select(x => x.Trim())
                .FirstOrDefault(x => x.StartsWith("GPU:", StringComparison.OrdinalIgnoreCase)) ?? "GPU detected by Vulkan";
            if (gpu.StartsWith("GPU:")) gpu = gpu[4..].Split(',')[0].Trim();
            GpuStatus.Text = gpu;
            FooterGpu.Text = "   |   " + gpu;
        }
        catch
        {
            GpuStatus.Text = "Vulkan available";
            FooterGpu.Text = "   |   Vulkan GPU";
        }
    }

    void LoadHeroImage()
    {
        HeroImage.Source = new BitmapImage(new Uri("pack://application:,,,/BloodborneLauncher;component/Assets/hunter-background.png"));
        string game = PathBox.Text.Trim();
        string[] candidates =
        {
            Path.Combine(game, "sce_sys", "pic1.png"),
            Path.Combine(game, "sce_sys", "pic0.png"),
            Path.Combine(root, "assets", "hero.jpg"),
            Path.Combine(root, "launcher-assets", "hero.jpg"),
            Path.Combine(AppContext.BaseDirectory, "assets", "hero.jpg"),
            Path.Combine(game, "sce_sys", "icon0.png")
        };
        foreach (string path in candidates)
        {
            if (!File.Exists(path)) continue;
            try
            {
                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                using var fs = File.OpenRead(path);
                bitmap.StreamSource = fs;
                bitmap.EndInit();
                bitmap.Freeze();
                HeroImage.Source = bitmap;
                return;
            }
            catch { }
        }
    }

    void Play_Click(object sender, RoutedEventArgs e)
    {
        SaveSettings();
        if (!File.Exists(Path.Combine(state.GamePath, "eboot.bin")))
        {
            MessageBox.Show("Select a valid Bloodborne folder first.", "Bloodborne PC", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        string runBat = Path.Combine(root, "run.bat");
        if (!File.Exists(runBat))
        {
            MessageBox.Show("run.bat was not found next to the runtime.", "Bloodborne PC", MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        try
        {
            string logsDir = Path.Combine(root, "logs");
            Directory.CreateDirectory(logsDir);
            string logPath = Path.Combine(logsDir, $"bbport-{DateTime.Now:yyyy-MM-dd_HH-mm-ss}.log");

            var psi = new ProcessStartInfo("cmd.exe")
            {
                WorkingDirectory = root,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                Arguments = $"/c \"\"{runBat}\" --game-dir \"{state.GamePath}\"\""
            };
            psi.Environment["BB_PREBUILT"] = "1";
            psi.Environment["BB_DATA_DIR"] = root;
            psi.Environment["BB_CONFIG"] = Path.Combine(root, "bbport.ini");
            psi.Environment["BB_GAME_DIR"] = state.GamePath;
            psi.Environment["BB_FPS"] = state.Fps;
            psi.Environment["BB_LANGUAGE"] = state.Language;
            psi.Environment["BB_GAMEPAD_INDEX"] = state.GamepadIndex.ToString();
            psi.Environment["BB_MODS_ENABLED"] = state.ModsEnabled ? "1" : "0";
            psi.Environment["BB_LOG_FILE"] = logPath;
            psi.Environment["PYTHONUNBUFFERED"] = "1";
            if (!string.IsNullOrEmpty(state.PresentMode)) psi.Environment["BB_PRESENT_MODE"] = state.PresentMode;
            if (state.DeveloperMode && state.DetailedLogs)
            {
                psi.Environment["BB_DLSS_LOG"] = "1";
                psi.Environment["BB_IMAGE_OVERLAP_LOG"] = "1";
                psi.Environment["BB_LOADING_UI_LOG"] = "1";
                psi.Environment["BB_DISABLE_SKIP_INTRO_PATCH"] = "1";
                psi.Environment["BB_DIRECT_IMAGE_BARRIERS"] = "1";
            }
            if (state.DeveloperMode && state.FrameStats) psi.Environment["BB_FRAME_STATS"] = "1";
            if (state.DeveloperMode && state.AudioStats) psi.Environment["BB_AUDIO_STATS"] = "1";
            if (state.DeveloperMode && state.FsrProfile) psi.Environment["BB_FSR4_PROFILE"] = "1";
            var gameProcess = new Process
            {
                StartInfo = psi,
                EnableRaisingEvents = true
            };
            gameProcess.Exited += (_, _) =>
            {
                Dispatcher.BeginInvoke(() =>
                {
                    Show();
                    WindowState = WindowState.Normal;
                    Activate();
                    FooterMessage.Text = "Bloodborne closed";
                    gameProcess.Dispose();
                });
            };
            gameProcess.Start();
            FooterMessage.Text = $"Started · log: {Path.GetFileName(logPath)}";
            Hide();
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "Unable to start Bloodborne", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    void Save_Click(object sender, RoutedEventArgs e) => SaveSettings();

    void BrowseGame_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Select the Bloodborne v1.09 folder" };
        if (!string.IsNullOrWhiteSpace(PathBox.Text) && Directory.Exists(PathBox.Text))
            dialog.InitialDirectory = PathBox.Text;
        if (dialog.ShowDialog() == true)
        {
            PathBox.Text = dialog.FolderName;
            state.GamePath = dialog.FolderName;
            LoadHeroImage();
            RefreshStatus();
        }
    }

    void OpenMods_Click(object sender, RoutedEventArgs e)
    {
        string mods = Path.Combine(root, "mods");
        Directory.CreateDirectory(mods);
        Process.Start(new ProcessStartInfo("explorer.exe", mods) { UseShellExecute = true });
    }

    void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        string logs = Path.Combine(root, "logs");
        Directory.CreateDirectory(logs);
        Process.Start(new ProcessStartInfo("explorer.exe", logs) { UseShellExecute = true });
    }

    void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button button) return;
        string page = button.CommandParameter?.ToString() ?? "Home";
        foreach (var b in new[] { NavHome, NavGeneral, NavGraphics, NavPerformance, NavController, NavMods, NavAdvanced }) b.Tag = null;
        button.Tag = "active";
        HomePage.Visibility = page == "Home" ? Visibility.Visible : Visibility.Collapsed;
        SettingsScrim.Visibility = page == "Home" ? Visibility.Collapsed : Visibility.Visible;
        GeneralPage.Visibility = page == "General" ? Visibility.Visible : Visibility.Collapsed;
        GraphicsPage.Visibility = page == "Graphics" ? Visibility.Visible : Visibility.Collapsed;
        PerformancePage.Visibility = page == "Performance" ? Visibility.Visible : Visibility.Collapsed;
        ControllerPage.Visibility = page == "Controller" ? Visibility.Visible : Visibility.Collapsed;
        ModsPage.Visibility = page == "Mods" ? Visibility.Visible : Visibility.Collapsed;
        AdvancedPage.Visibility = page == "Advanced" ? Visibility.Visible : Visibility.Collapsed;
    }

    void QuickResolution_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || QuickResolution.SelectedItem is not ComboBoxItem item) return;
        SetComboContent(ResolutionCombo, item.Content?.ToString() ?? "1920x1080");
    }
    void QuickDisplay_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing) return;
        FullscreenCheck.IsChecked = QuickDisplay.SelectedIndex == 0;
    }
    void QuickUpscaler_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing) return;
        string[] values = { "dlss", "fsr411", "fsr4", "fsr3", "taa", "off" };
        if (QuickUpscaler.SelectedIndex >= 0) SetComboTag(UpscalerCombo, values[QuickUpscaler.SelectedIndex]);
    }
    void QuickPreset_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || QuickPreset.SelectedIndex < 0) return;
        if (QuickPreset.SelectedIndex >= 0) SetComboTag(PresetCombo, QuickPreset.SelectedIndex.ToString());
    }
    void QuickFps_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (syncing || QuickFps.SelectedItem is not ComboBoxItem item) return;
        string v = item.Content?.ToString() == "Unlimited" ? "uncap" : item.Content?.ToString() ?? "60";
        SetComboTag(FpsCombo, v);
        FooterFps.Text = $"   |   {(v == "uncap" ? "Unlimited" : v + " FPS")}";
    }

    void SetQuickUpscaler(string value)
    {
        QuickUpscaler.SelectedIndex = value switch { "dlss" => 0, "fsr411" => 1, "fsr4" => 2, "fsr3" => 3, "taa" => 4, _ => 5 };
    }
    void SetQuickFps(string value)
    {
        QuickFps.SelectedIndex = value switch { "30" => 0, "60" => 1, "90" => 2, _ => 3 };
    }

    static string ComboTag(ComboBox box, string fallback = "") =>
        (box.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? fallback;
    static string ComboText(ComboBox box, string fallback = "") =>
        (box.SelectedItem as ComboBoxItem)?.Content?.ToString() ?? fallback;

    static void SetComboTag(ComboBox box, string tag)
    {
        foreach (var obj in box.Items)
            if (obj is ComboBoxItem item && (item.Tag?.ToString() ?? "") == tag) { box.SelectedItem = item; return; }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }

    static void SetComboContent(ComboBox box, string value)
    {
        foreach (var obj in box.Items)
            if (obj is ComboBoxItem item && string.Equals(item.Content?.ToString(), value, StringComparison.OrdinalIgnoreCase)) { box.SelectedItem = item; return; }
        if (box.Items.Count > 0) box.SelectedIndex = 0;
    }

    void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2) Maximize_Click(sender, e);
        else DragMove();
    }
    void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    void Maximize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    void Close_Click(object sender, RoutedEventArgs e) => Close();
}
