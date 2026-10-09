param([string]$PreviewDirectory, [string]$PackageCache)
$ErrorActionPreference = 'Stop'
# Keep the verification host outside the repository: Play can only reach
# synthetic fixtures, never the user's runtime, game dump or saved settings.
$verificationRoot = Join-Path ([IO.Path]::GetTempPath()) ('BloodborneLauncher-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $verificationRoot | Out-Null
if (!$PreviewDirectory) { $PreviewDirectory = Join-Path $verificationRoot 'previews' }
$PreviewDirectory = [IO.Path]::GetFullPath($PreviewDirectory)
$projectReference = [Security.SecurityElement]::Escape((Join-Path $PSScriptRoot 'BloodborneLauncher.csproj'))
$project = '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0-windows</TargetFramework><UseWPF>true</UseWPF><ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable></PropertyGroup><ItemGroup><ProjectReference Include="'+$projectReference+'"/></ItemGroup></Project>'
Set-Content -Encoding utf8 -LiteralPath (Join-Path $verificationRoot 'LauncherChecks.csproj') -Value $project
$program = @'
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using BloodborneLauncher;

static class Program
{
    static MainWindow window = null!;
    static int assertions;
    static T Get<T>(string name) where T : class => (T)window.FindName(name);
    static void Call(string name, params object[] args) => typeof(MainWindow).GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(window, args);
    static void Check(bool value, string message) { if (!value) throw new Exception(message); assertions++; }
    static string Tag(string name) => ((ComboBoxItem)Get<ComboBox>(name).SelectedItem).Tag?.ToString() ?? "";
    static void Select(string name, string tag) {
        var box = Get<ComboBox>(name);
        box.SelectedItem = box.Items.Cast<ComboBoxItem>().First(x => x.Tag?.ToString() == tag);
    }
    static void ClickNav(string page) => Call("Nav_Click", Get<Button>("Nav" + page), new RoutedEventArgs());
    static void Pump() => window.Dispatcher.Invoke(() => { }, System.Windows.Threading.DispatcherPriority.ApplicationIdle);
    static void Render(string file, double width=1500, double height=920, double dpi=96) {
        window.Width=width; window.Height=height;
        window.UpdateLayout(); Pump();
        var element = (FrameworkElement)window.Content;
        element.Measure(new Size(width-2,height-2)); element.Arrange(new Rect(0,0,width-2,height-2)); element.UpdateLayout();
        var bitmap = new RenderTargetBitmap((int)((width-2)*dpi/96), (int)((height-2)*dpi/96), dpi, dpi, PixelFormats.Pbgra32);
        bitmap.Render(element);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var output = File.Create(file); encoder.Save(output);
    }
    [STAThread] static int Main(string[] args) {
        try { Run(args[0]); return 0; } catch(Exception ex) { Console.Error.WriteLine(ex); return 1; }
    }
    static void Run(string output) {
        Directory.CreateDirectory(output);
        var root = AppContext.BaseDirectory;
        string game = Path.Combine(root, "Test game");
        Directory.CreateDirectory(Path.Combine(game,"sce_sys"));
        File.WriteAllText(Path.Combine(game,"eboot.bin"),"launcher test fixture");
        File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),"01.09");
        File.WriteAllText(Path.Combine(root,"run.bat"),"@echo off\r\nset BB_ > launch-env.txt\r\necho %* > launch-args.txt\r\n");
        File.WriteAllText(Path.Combine(root,"bbport.ini"),"# keep this comment\r\nunrelated_setting=keep\r\nupscaler=off\r\n");
        foreach (string key in Environment.GetEnvironmentVariables().Keys)
            if(key.StartsWith("BB_")) Environment.SetEnvironmentVariable(key,null);
        if (File.Exists(Path.Combine(root,"launcher-settings.json"))) File.Delete(Path.Combine(root,"launcher-settings.json"));
        foreach(var stale in new[]{"out/game_dir.txt","launch-env.txt","launch-args.txt"}) { var f=Path.Combine(root,stale); if(File.Exists(f)) File.Delete(f); }
        var app = new App();
        app.InitializeComponent();

        window = new MainWindow { ShowInTaskbar=false, ShowActivated=false, WindowStartupLocation=WindowStartupLocation.Manual, Left=-20000, Top=-20000 };
        window.Show(); Pump();
        Check(!Get<Button>("PlayButton").IsEnabled, "Play must be disabled without a dump");
        Check(Get<TextBlock>("RuntimeStatus").Text=="Not built","Missing runtime status");
        Check(Get<TextBlock>("DlssStatus").Text=="Model DLL not installed","Missing DLSS status");
        Check(Get<TextBlock>("Fsr4Status").Text=="Assets not installed","Missing FSR4 status");
        Check(Get<Image>("HeroImage").Source is BitmapSource, "Embedded hero not loaded");
        Render(Path.Combine(output,"launcher-play-unconfigured.png"));
        Get<TextBox>("PathBox").Text=game; Call("RefreshStatus");
        Check(Get<Button>("PlayButton").IsEnabled,"Valid fixture should enable Play");
        Check(Get<TextBlock>("GameVersionStatus").Text.Contains("1.09"),"1.09 detection");
        File.Move(Path.Combine(game,"eboot.bin"),Path.Combine(game,"eboot.hidden"));
        Call("RefreshStatus"); Check(!Get<Button>("PlayButton").IsEnabled,"Missing eboot disables Play");
        File.Move(Path.Combine(game,"eboot.hidden"),Path.Combine(game,"eboot.bin"));
        Directory.Move(Path.Combine(game,"sce_sys"),Path.Combine(game,"sce_hidden"));
        Call("RefreshStatus"); Check(!Get<Button>("PlayButton").IsEnabled,"Missing sce_sys disables Play");
        Directory.Move(Path.Combine(game,"sce_hidden"),Path.Combine(game,"sce_sys"));
        foreach(var version in new[]{"01.08","01.00","unknown"}) {
            File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),version); Call("RefreshStatus");
            Check(Get<Button>("PlayButton").IsEnabled,"Preserve dump validation for "+version);
            Check(!Get<TextBlock>("GameVersionStatus").Text.Contains("1.09 ✓"),"No false 1.09 for "+version);
        }
        File.WriteAllText(Path.Combine(game,"sce_sys","param.sfo"),"01.09");
        Directory.CreateDirectory(Path.Combine(root,"out"));
        File.WriteAllText(Path.Combine(root,"out","bb-probe.exe"),"status fixture only");
        File.WriteAllText(Path.Combine(root,"out","nvngx_dlss.dll"),"status fixture only");
        Directory.CreateDirectory(Path.Combine(root,"fsr4_shaders")); Call("RefreshStatus");
        Check(Get<TextBlock>("RuntimeStatus").Text=="Ready","Runtime existence detection");
        Check(Get<TextBlock>("DlssStatus").Text=="Available","DLSS existence detection");
        Check(Get<TextBlock>("Fsr4Status").Text=="v07 available","FSR4 legacy detection");
        Directory.CreateDirectory(Path.Combine(root,"fsr4_411")); Call("RefreshStatus");
        Check(Get<TextBlock>("Fsr4Status").Text=="4.1.1 available","FSR4 4.1.1 detection");
        File.Delete(Path.Combine(root,"out","bb-probe.exe")); File.Delete(Path.Combine(root,"out","nvngx_dlss.dll"));
        Directory.Delete(Path.Combine(root,"fsr4_shaders")); Directory.Delete(Path.Combine(root,"fsr4_411"));
        Get<TextBox>("PathBox").Text=Path.Combine(root,"bad"); Call("RefreshStatus");
        Check(!Get<Button>("PlayButton").IsEnabled,"Invalid path must disable Play");
        Get<TextBox>("PathBox").Text=game; Call("RefreshStatus");
        foreach(var pair in new[] { ("UpscalerCombo","QuickUpscaler"),("PresetCombo","QuickPreset"),("FpsCombo","QuickFps"),("ResolutionCombo","QuickResolution") }) {
            var detailed=Get<ComboBox>(pair.Item1); var quick=Get<ComboBox>(pair.Item2);
            for(int i=0;i<detailed.Items.Count;i++) { detailed.SelectedIndex=i; Check(quick.SelectedIndex==i,pair.Item1+" to quick "+i); }
            for(int i=quick.Items.Count-1;i>=0;i--) { quick.SelectedIndex=i; Check(detailed.SelectedIndex==i,pair.Item2+" to detailed "+i); }
        }
        Get<CheckBox>("FullscreenCheck").IsChecked=false; Check(Get<ComboBox>("QuickDisplay").SelectedIndex==1,"Display sync to quick");
        Get<ComboBox>("QuickDisplay").SelectedIndex=0; Check(Get<CheckBox>("FullscreenCheck").IsChecked==true,"Display sync from quick");
        Select("CameraFovCombo","1.25"); Select("CameraDistanceCombo","0.80"); Select("CameraHeightCombo","1.20");
        Select("GraphicsAoCombo","0.40"); Select("GraphicsShadowsCombo","1.40");
        Select("GraphicsBloomCombo","0.80"); Select("GraphicsSaturationCombo","1.20");
        Get<CheckBox>("RebirthCheck").IsChecked=true;
        Get<CheckBox>("GraphicsControlsCheck").IsChecked=true;
        Get<CheckBox>("GraphicsVignetteCheck").IsChecked=false;
        Select("UpscalerCombo","fsr411"); Select("PresetCombo","3"); Select("FpsCombo","90"); Select("LanguageCombo","3");
        Select("PresentCombo","FifoRelaxed"); Select("LiveResolutionCombo","auto"); Select("ModelLodCombo","-2");
        Get<ComboBox>("ResolutionCombo").SelectedIndex=2; Get<ComboBox>("ControllerCombo").SelectedIndex=2;
        foreach(var name in new[]{"CameraControlsCheck","ChangeAppearanceCheck","SharpenCheck","ShowFpsCheck","SsrCheck","SkipIntroCheck","DeveloperModeCheck","DetailedLogsCheck","FrameStatsCheck","AudioStatsCheck","FsrProfileCheck"}) Get<CheckBox>(name).IsChecked=true;
        foreach(var name in new[]{"ChromaticCheck","DofCheck","MotionBlurCheck","SsaoCheck","GameAaCheck","ShadowsCheck","ModsEnabledCheck"}) Get<CheckBox>(name).IsChecked=false;
        Call("SaveSettings");
        string ini=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        foreach(var expected in new[]{"rebirth=1","graphics_controls=1","graphics_vignette=0","graphics_ao_strength=0.40","graphics_shadow_scale=1.40","graphics_bloom=0.80","graphics_saturation=1.20"})
            Check(ini.Split("\r\n").Contains(expected),"Native engine option "+expected);
        foreach(var expected in new[]{"# keep this comment","unrelated_setting=keep","upscaler=fsr411","preset=3","sharpen=1","show_fps=1","output_res=2560x1440","fullscreen=1","live_resolution=auto","model_lod=-2","effect_chromatic_aberration=0","effect_dof=0","effect_motion_blur=0","effect_ssao=0","effect_game_aa=0","effect_dynamic_shadows=0","effect_ssr=1","skip_intro=1","change_appearance=1","camera_controls=1","camera_fov_scale=1.25","camera_distance_scale=0.80","camera_height_scale=1.20"}) Check(ini.Split("\r\n").Contains(expected),"INI "+expected);
        using(var json=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json")))) {
            Check(json.RootElement.GetProperty("GamePath").GetString()==game,"JSON game path");
            Check(json.RootElement.GetProperty("GamepadIndex").GetInt32()==2,"JSON controller");
            Check(json.RootElement.GetProperty("Fps").GetString()=="90","JSON fps");
            Check(!json.RootElement.GetProperty("ModsEnabled").GetBoolean(),"JSON mods");
        }
        Check(File.ReadAllText(Path.Combine(root,"out","game_dir.txt"))==game,"Remembered path");
        Call("Play_Click", Get<Button>("PlayButton"),new RoutedEventArgs());
        var envPath=Path.Combine(root,"launch-env.txt");
        for(int i=0;i<100 && !File.Exists(envPath);i++) Thread.Sleep(50);
        string env=File.ReadAllText(envPath);
        foreach(var expected in new[]{"BB_PREBUILT=1","BB_DATA_DIR="+root.TrimEnd(Path.DirectorySeparatorChar),"BB_CONFIG="+Path.Combine(root.TrimEnd(Path.DirectorySeparatorChar),"bbport.ini"),"BB_GAME_DIR="+game,"BB_FPS=90","BB_FPS_LIMIT=90","BB_VBLANK_HZ=90","BB_LANGUAGE=3","BB_GAMEPAD_INDEX=2","BB_MODS_ENABLED=0","BB_PRESENT_MODE=FifoRelaxed","BB_DLSS_LOG=1","BB_FRAME_STATS=1","BB_AUDIO_STATS=1","BB_FSR4_PROFILE=1"}) Check(env.Split("\r\n").Contains(expected),"Launch environment "+expected);
        Check(File.ReadAllText(Path.Combine(root,"launch-args.txt")).Contains("--game-dir \""+game+"\""),"Launch path quoting");
        Render(Path.Combine(output,"launcher-play.png"));
        foreach(var page in new[]{"General","Graphics","Performance","Controller","Mods","Advanced"}) {
            ClickNav(page); Pump();
            Check(Get<FrameworkElement>(page+"Page").Visibility==Visibility.Visible,"Navigation "+page);
            Check(Get<FrameworkElement>("HomePage").Visibility==Visibility.Collapsed,"Home hidden "+page);
            Render(Path.Combine(output,"launcher-"+page.ToLowerInvariant()+".png"));
        }
        ClickNav("Graphics");
        var upscaler = Get<ComboBox>("UpscalerCombo");
        upscaler.IsDropDownOpen = true; Pump(); window.UpdateLayout();
        var popup = (System.Windows.Controls.Primitives.Popup)upscaler.Template.FindName("PART_Popup",upscaler);
        Check(popup.IsOpen && popup.Child != null,"Styled dropdown opens");
        Check(((FrameworkElement)popup.Child!).ActualWidth >= upscaler.ActualWidth,"Dropdown fits its selector");
        var menu = (FrameworkElement)popup.Child!;
        var menuBitmap = new RenderTargetBitmap((int)Math.Ceiling(menu.ActualWidth),(int)Math.Ceiling(menu.ActualHeight),96,96,PixelFormats.Pbgra32);
        menuBitmap.Render(menu);
        var menuEncoder = new PngBitmapEncoder(); menuEncoder.Frames.Add(BitmapFrame.Create(menuBitmap));
        using(var menuOutput=File.Create(Path.Combine(output,"launcher-dropdown.png"))) menuEncoder.Save(menuOutput);
        upscaler.IsDropDownOpen=false; Pump();
        ClickNav("Home"); Render(Path.Combine(output,"launcher-compact.png"),1000,680);
        Get<ScrollViewer>("HomePage").ScrollToBottom(); Pump();
        Render(Path.Combine(output,"launcher-compact-scrolled.png"),1000,680);
        Check(Get<ScrollViewer>("HomePage").VerticalOffset>0,"Compact home scrolls to quick controls");
        Get<ScrollViewer>("HomePage").ScrollToTop(); Pump();
        ClickNav("Graphics"); Render(Path.Combine(output,"launcher-graphics-compact.png"),1000,680);
        ClickNav("Home"); Render(Path.Combine(output,"launcher-150-percent.png"),1100,720,144);
        window.MinWidth=900; window.MinHeight=520;
        Render(Path.Combine(output,"launcher-200-percent.png"),900,520,192);
        Get<ScrollViewer>("SidebarNavigation").ScrollToBottom(); Pump();
        Check(Get<ScrollViewer>("SidebarNavigation").VerticalOffset>0,"Short-screen sidebar remains scrollable");
        ClickNav("Advanced");
        Check(Get<FrameworkElement>("AdvancedPage").Visibility==Visibility.Visible,"Advanced reachable on short screen");
        Render(Path.Combine(output,"launcher-advanced-200-percent.png"),900,520,192);
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("UpscalerCombo")=="fsr411" && Tag("FpsCombo")=="90" && Tag("PresetCombo")=="3","Settings reload");
        Check(Get<ComboBox>("QuickUpscaler").SelectedIndex==1 && Get<ComboBox>("QuickFps").SelectedIndex==2,"Quick settings reload");
        Check(Get<ComboBox>("FpsCombo").Items.Count==3 && Get<ComboBox>("QuickFps").Items.Count==3,"Only supported FPS presets");
        foreach(string legacy in new[]{"uncap","Unlimited","0","480","bad"}) {
            string saved=File.ReadAllText(Path.Combine(root,"launcher-settings.json"));
            File.WriteAllText(Path.Combine(root,"launcher-settings.json"),saved.Replace("\"Fps\": \"90\"","\"Fps\": \""+legacy+"\""));
            Call("LoadState"); Call("ApplyStateToUi");
            Check(Tag("FpsCombo")=="60" && Get<ComboBox>("QuickFps").SelectedIndex==1,"Legacy FPS fallback "+legacy);
            Call("SaveSettings");
            using(var migrated=JsonDocument.Parse(File.ReadAllText(Path.Combine(root,"launcher-settings.json"))))
                Check(migrated.RootElement.GetProperty("Fps").GetString()=="60","Persist FPS migration "+legacy);
            File.WriteAllText(Path.Combine(root,"launcher-settings.json"),saved);
        }
        Check(Get<CheckBox>("ChangeAppearanceCheck").IsChecked==true,"Mirror option reload");
        string mirrorIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),mirrorIni.Replace("change_appearance=1","change_appearance=0"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Get<CheckBox>("ChangeAppearanceCheck").IsChecked==false,"Runtime INI owns mirror setting");
        Check(Tag("CameraFovCombo")=="1.25" && Tag("CameraDistanceCombo")=="0.80" && Tag("CameraHeightCombo")=="1.20", "Camera runtime settings reload");
        string cameraIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),cameraIni.Replace("camera_fov_scale=1.25","camera_fov_scale=1.23"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("CameraFovCombo")=="1.23","Preserve in-game intermediate FOV");
        Check(Get<CheckBox>("RebirthCheck").IsChecked==true && Get<CheckBox>("GraphicsControlsCheck").IsChecked==true,"Enhancements reload from INI");
        Check(Tag("GraphicsAoCombo")=="0.40" && Tag("GraphicsShadowsCombo")=="1.40" && Tag("GraphicsBloomCombo")=="0.80" && Tag("GraphicsSaturationCombo")=="1.20","Live graphics values reload");
        string runtimeIni=File.ReadAllText(Path.Combine(root,"bbport.ini"));
        File.WriteAllText(Path.Combine(root,"bbport.ini"),runtimeIni.Replace("graphics_bloom=0.80","graphics_bloom=0.60").Replace("rebirth=1","rebirth=0").Replace("effect_ssao=0","effect_ssao=1"));
        Call("LoadState"); Call("ApplyStateToUi");
        Check(Tag("GraphicsBloomCombo")=="0.60" && Get<CheckBox>("RebirthCheck").IsChecked==false && Get<CheckBox>("SsaoCheck").IsChecked==true,"Native pages remain authoritative over stale launcher JSON");
        Console.WriteLine($"PASS: {assertions} assertions; settings, navigation, all quick options, launch environment and artwork.");
        window.Close(); app.Shutdown();
    }
}




'@
Set-Content -Encoding utf8 -LiteralPath (Join-Path $verificationRoot 'Program.cs') -Value $program
$arguments = @('run','--project',(Join-Path $verificationRoot 'LauncherChecks.csproj'),'--configuration','Release')
if ($PackageCache) { $arguments += "--property:RestorePackagesPath=$([IO.Path]::GetFullPath($PackageCache))" }
& dotnet @arguments -- $PreviewDirectory
if ($LASTEXITCODE -ne 0) { throw 'Launcher verification failed.' }
Write-Host "Preview images: $PreviewDirectory"
Write-Host "Isolated test fixtures: $verificationRoot"
