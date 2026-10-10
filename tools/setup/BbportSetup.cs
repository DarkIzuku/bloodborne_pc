// SPDX-License-Identifier: GPL-2.0-or-later
// bbport setup for Windows (setup.bat compiles this with the C# compiler of .NET Framework 4,
// which every Windows 10/11 has: C# 5, no other dependencies).
//
// One window: the game folder and the settings, then Install / Update, which installs MSYS2
// (to C:\msys64, or BB_MSYS2) and the packages README "Windows" lists, gets the sources and their
// submodules, optionally downloads the DLSS and FSR 4 models, builds out\bb-probe.exe, and writes
// bbport.ini, out\game_dir.txt, Bloodborne.cmd (the launcher: frame rate, game language, present
// mode) and the shortcuts. Run again to change settings: Save settings writes them without
// building.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace BbportSetup {

static class Program {
    [DllImport("user32.dll")]
    static extern bool SetProcessDPIAware();

    [STAThread]
    static int Main(string[] args) {
        string root = null;
        for (int i = 0; i + 1 < args.Length; ++i) {
            if (args[i] == "--root") root = args[i + 1];
        }
        try { SetProcessDPIAware(); } catch (Exception) { }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm(root));
        return 0;
    }
}

/// A combo box entry: what the user reads and what is written.
class Choice {
    public readonly string Text, Value;
    public Choice(string text, string value) { Text = text; Value = value; }
    public override string ToString() { return Text; }
}

/// The options Install and Save write, read from the form on the UI thread.
class Options {
    public string Root, Game, Msys;
    public string RepoUrl, Branch;
    public Dictionary<string, string> Ini = new Dictionary<string, string>();
    public Dictionary<string, string> Env = new Dictionary<string, string>();
    public bool DlssModel, Fsr4Models, DesktopLink, StartMenuLink;
}

class SetupForm : Form {
    const string Title = "Bloodborne PC (bbport) setup";
    const string DefaultRepo = "https://github.com/deadinside28/bloodborne_pc.git";
    // README "Windows"; brace expansion by bash.
    const string Packages = "git mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,python,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg}";
    const string Msys2Installer = "https://repo.msys2.org/distrib/msys2-x86_64-latest.exe";

    // The in-game menu's choices (gpu/shim/bbport_overlay.cpp, bbport_settings.h).
    static readonly Choice[] Resolutions = {
        new Choice("1280 x 720", "1280x720"), new Choice("1920 x 1080", "1920x1080"),
        new Choice("2560 x 1440", "2560x1440"), new Choice("3840 x 2160", "3840x2160") };
    static readonly Choice[] FrameRates = {
        new Choice("Uncapped (follows the display)", "uncap"), new Choice("90 FPS", "90"),
        new Choice("60 FPS", "60"), new Choice("30 FPS (original)", "30") };
    static readonly Choice[] Presets = {
        new Choice("Native AA (x1.0)", "0"), new Choice("Quality (x1.5)", "1"),
        new Choice("Balanced (x1.7)", "2"), new Choice("Performance (x2.0)", "3"),
        new Choice("Ultra Performance (x3.0)", "4") };
    static readonly Choice[] LiveModes = {
        new Choice("Off (faster)", "0"), new Choice("Auto (by GPU)", "auto"), new Choice("On", "1") };
    static readonly Choice[] ModelDetail = {
        new Choice("Highest (-2)", "-2"), new Choice("As in the game", "0"),
        new Choice("Lower (1)", "1"), new Choice("Lowest (2)", "2") };
    static readonly Choice[] Languages = {
        new Choice("English", "1"), new Choice("French", "2"), new Choice("Spanish", "3"),
        new Choice("German", "4"), new Choice("Italian", "5"), new Choice("Russian", "8"),
        new Choice("Japanese", "0") };
    static readonly Choice[] PresentModes = {
        new Choice("Mailbox (default)", ""), new Choice("FIFO (VSync)", "Fifo"),
        new Choice("FIFO Relaxed", "FifoRelaxed"), new Choice("Immediate (tearing)", "Immediate") };
    // bbport_settings.h Effects: key, label, default.
    static readonly string[][] Effects = {
        new[] { "effect_chromatic_aberration", "Chromatic aberration", "1" },
        new[] { "effect_dof", "Depth of field (DoF)", "1" },
        new[] { "effect_motion_blur", "Motion blur", "1" },
        new[] { "effect_ssao", "SSAO ambient occlusion", "1" },
        new[] { "effect_game_aa", "The game's own anti-aliasing", "1" },
        new[] { "effect_dynamic_shadows", "Shadows from dynamic lights", "1" },
        new[] { "effect_ssr", "SSR reflections (not in the original game)", "0" },
        new[] { "skip_intro", "Skip intro videos at startup", "0" } };

    readonly float scale;
    readonly bool nvidia;
    string root;
    bool checkout; // root holds the sources (setup.bat in a clone)

    TextBox rootBox, gameBox, repoBox, branchBox, logBox;
    Label gameStatus, rootStatus, statusLabel;
    ComboBox resolution, frameRate, upscaler, preset, live, modelLod, language, presentMode;
    CheckBox fullscreen, showFps, sharpen, dlssModel, fsr4Models, desktopLink, startMenuLink;
    readonly List<KeyValuePair<string, CheckBox>> effects = new List<KeyValuePair<string, CheckBox>>();
    Button installButton, saveButton, playButton;
    TabControl tabs;
    TabPage logPage;
    ProgressBar progress;

    readonly ConcurrentQueue<string> pending = new ConcurrentQueue<string>();
    readonly System.Windows.Forms.Timer pump = new System.Windows.Forms.Timer();
    volatile bool busy;
    Process current;

    public SetupForm(string rootArgument) {
        using (Graphics g = CreateGraphics()) scale = g.DpiX / 96f;
        nvidia = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\NVIDIA Corporation\Global\NGXCore") != null;
        root = FindRoot(rootArgument);
        checkout = root != null && File.Exists(Path.Combine(root, "build.sh"));
        if (root == null) root = @"C:\bbport";

        Text = Title;
        Font = SystemFonts.MessageBoxFont;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(S(760), S(720));
        MinimumSize = new Size(S(640), S(560));
        Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        BuildUi();
        LoadSettings();
        pump.Interval = 100;
        pump.Tick += delegate { Drain(); };
        pump.Start();
        FormClosing += OnClosing;
    }

    int S(int value) { return (int)Math.Round(value * scale); }

    static string FindRoot(string argument) {
        if (!string.IsNullOrEmpty(argument)) return Path.GetFullPath(argument.TrimEnd('\\', '"'));
        // The executable in <checkout>\out\ or next to build.sh.
        string dir = Path.GetDirectoryName(Application.ExecutablePath);
        for (int i = 0; i < 3 && dir != null; ++i) {
            if (File.Exists(Path.Combine(dir, "build.sh")) && File.Exists(Path.Combine(dir, "run.bat"))) return dir;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    static string MsysRoot() {
        string env = Environment.GetEnvironmentVariable("BB_MSYS2");
        return string.IsNullOrEmpty(env) ? @"C:\msys64" : env;
    }

    // ---- layout ------------------------------------------------------------------------------

    void BuildUi() {
        tabs = new TabControl { Dock = DockStyle.Fill };
        var settingsPage = new TabPage("Settings") { AutoScroll = true, Padding = new Padding(S(8)) };
        logPage = new TabPage("Install log") { Padding = new Padding(S(4)) };
        tabs.TabPages.Add(settingsPage);
        tabs.TabPages.Add(logPage);

        var column = new TableLayoutPanel { ColumnCount = 1, Dock = DockStyle.Top, AutoSize = true,
                                            AutoSizeMode = AutoSizeMode.GrowAndShrink };
        column.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        settingsPage.Controls.Add(column);

        // Folders.
        var folders = Group("Folders", 3);
        rootBox = new TextBox { Dock = DockStyle.Fill, Text = root, ReadOnly = checkout };
        AddRow(folders, "Install folder", rootBox, checkout ? null : Browse(rootBox, "Folder for bbport (sources, build, saves)"));
        rootStatus = Note(checkout ? "Sources found here (setup.bat in a checkout): they are built in place."
                                   : "The sources are cloned here. Avoid spaces in the path.");
        AddSpan(folders, rootStatus);
        if (!checkout) {
            repoBox = new TextBox { Dock = DockStyle.Fill, Text = DefaultRepo };
            branchBox = new TextBox { Dock = DockStyle.Fill, Text = "master" };
            AddRow(folders, "Repository", repoBox, null);
            AddRow(folders, "Branch", branchBox, null);
        }
        gameBox = new TextBox { Dock = DockStyle.Fill };
        gameBox.TextChanged += delegate { CheckGame(); };
        AddRow(folders, "Game folder", gameBox, Browse(gameBox, "The Bloodborne v1.09 dump: the folder with eboot.bin and sce_sys"));
        gameStatus = Note("");
        AddSpan(folders, gameStatus);
        column.Controls.Add(folders.Parent);

        // Display.
        var display = Group("Display", 2);
        resolution = Combo(display, "Output resolution", Resolutions);
        frameRate = Combo(display, "Frame rate", FrameRates);
        presentMode = Combo(display, "Present mode", PresentModes);
        fullscreen = Check("Fullscreen (borderless, desktop size; F11 in game)");
        showFps = Check("FPS counter in the corner");
        AddSpan(display, Flow(fullscreen, showFps));
        column.Controls.Add(display.Parent);

        // Image quality.
        var image = Group("Image quality", 2);
        var upscalers = new List<Choice> { new Choice("FSR 3.1", "fsr3"), new Choice("FSR 4 (INT8 model)", "fsr4"),
                                           new Choice("TAA (native anti-aliasing)", "taa"), new Choice("Off", "off") };
        if (nvidia) upscalers.Insert(0, new Choice("DLSS (NVIDIA RTX)", "dlss"));
        upscaler = Combo(image, "Upscaler", upscalers.ToArray());
        upscaler.SelectedIndexChanged += delegate { OnUpscaler(); };
        preset = Combo(image, "Quality preset", Presets);
        live = Combo(image, "Live resolution changes", LiveModes);
        modelLod = Combo(image, "Model detail", ModelDetail);
        sharpen = Check("Sharpening (RCAS)");
        AddSpan(image, sharpen);
        AddSpan(image, Note("Presets below Native AA render the scene at a lower resolution and upscale it. " +
                            "\"Live resolution changes\" Off is fastest: preset and resolution changes then need a restart."));
        column.Controls.Add(image.Parent);

        // Effects.
        var effectGroup = Group("Game effects", 2);
        var effectGrid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Fill };
        effectGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        effectGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        foreach (string[] effect in Effects) {
            var box = Check(effect[1]);
            effects.Add(new KeyValuePair<string, CheckBox>(effect[0], box));
            effectGrid.Controls.Add(box);
        }
        AddSpan(effectGroup, effectGrid);
        column.Controls.Add(effectGroup.Parent);

        // Game language and extras.
        var extras = Group("Language and extras", 2);
        language = Combo(extras, "Game language", Languages);
        dlssModel = Check("Download NVIDIA's DLSS model (needed for DLSS)");
        dlssModel.Enabled = nvidia;
        fsr4Models = Check("Download the FSR 4 models (needed for FSR 4, ~300 MB)");
        desktopLink = Check("Desktop shortcut");
        startMenuLink = Check("Start menu shortcut");
        AddSpan(extras, dlssModel);
        AddSpan(extras, fsr4Models);
        AddSpan(extras, Flow(desktopLink, startMenuLink));
        column.Controls.Add(extras.Parent);

        // Log.
        logBox = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false,
                               Dock = DockStyle.Fill, Font = new Font("Consolas", 9f),
                               BackColor = SystemColors.Window };
        logPage.Controls.Add(logBox);

        // Bottom: status, progress and the buttons.
        var bottom = new TableLayoutPanel { Dock = DockStyle.Bottom, ColumnCount = 1, AutoSize = true,
                                            Padding = new Padding(S(8), S(4), S(8), S(8)) };
        statusLabel = new Label { AutoSize = true, Text = "Choose the game folder and the settings, then Install / Update." };
        progress = new ProgressBar { Dock = DockStyle.Fill, Height = S(14), Style = ProgressBarStyle.Continuous };
        var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.RightToLeft, Dock = DockStyle.Fill, AutoSize = true };
        var closeButton = new Button { Text = "Close", AutoSize = true };
        closeButton.Click += delegate { Close(); };
        playButton = new Button { Text = "Play", AutoSize = true };
        playButton.Click += delegate { Play(); };
        saveButton = new Button { Text = "Save settings", AutoSize = true };
        saveButton.Click += delegate { SaveOnly(); };
        installButton = new Button { Text = "Install / Update", AutoSize = true };
        installButton.Click += delegate { StartInstall(); };
        buttons.Controls.AddRange(new Control[] { closeButton, playButton, saveButton, installButton });
        bottom.Controls.Add(statusLabel);
        bottom.Controls.Add(progress);
        bottom.Controls.Add(buttons);

        Controls.Add(tabs);
        Controls.Add(bottom);
        AcceptButton = installButton;
        UpdateButtons();
    }

    TableLayoutPanel Group(string title, int columns) {
        var box = new GroupBox { Text = title, Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(S(8)),
                                 AutoSizeMode = AutoSizeMode.GrowAndShrink, Margin = new Padding(0, 0, 0, S(8)) };
        var grid = new TableLayoutPanel { ColumnCount = columns, Dock = DockStyle.Fill, AutoSize = true,
                                          AutoSizeMode = AutoSizeMode.GrowAndShrink };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        if (columns > 2) grid.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        box.Controls.Add(grid);
        return grid;
    }

    void AddRow(TableLayoutPanel grid, string label, Control control, Control extra) {
        grid.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, S(6), S(8), 0) });
        grid.Controls.Add(control);
        if (grid.ColumnCount > 2) grid.Controls.Add(extra ?? new Label { AutoSize = true });
    }

    static void AddSpan(TableLayoutPanel grid, Control control) {
        grid.Controls.Add(control);
        grid.SetColumnSpan(control, grid.ColumnCount);
    }

    ComboBox Combo(TableLayoutPanel grid, string label, Choice[] choices) {
        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(300), Anchor = AnchorStyles.Left };
        combo.Items.AddRange(choices);
        combo.SelectedIndex = 0;
        AddRow(grid, label, combo, null);
        return combo;
    }

    static CheckBox Check(string text) {
        return new CheckBox { Text = text, AutoSize = true };
    }

    Label Note(string text) {
        return new Label { Text = text, AutoSize = true, ForeColor = SystemColors.GrayText,
                           MaximumSize = new Size(S(680), 0), Margin = new Padding(0, S(2), 0, S(6)) };
    }

    static FlowLayoutPanel Flow(params Control[] controls) {
        var flow = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
                                         Dock = DockStyle.Fill, WrapContents = false };
        flow.Controls.AddRange(controls);
        return flow;
    }

    Button Browse(TextBox target, string description) {
        var button = new Button { Text = "Browse...", AutoSize = true };
        button.Click += delegate {
            using (var dialog = new FolderBrowserDialog { Description = description, ShowNewFolderButton = true }) {
                if (Directory.Exists(target.Text)) dialog.SelectedPath = target.Text;
                if (dialog.ShowDialog(this) == DialogResult.OK) target.Text = dialog.SelectedPath;
            }
        };
        return button;
    }

    static void SelectValue(ComboBox combo, string value) {
        for (int i = 0; i < combo.Items.Count; ++i) {
            if (((Choice)combo.Items[i]).Value == value) { combo.SelectedIndex = i; return; }
        }
    }

    static string Value(ComboBox combo) {
        return combo.SelectedItem == null ? "" : ((Choice)combo.SelectedItem).Value;
    }

    void OnUpscaler() {
        string u = Value(upscaler);
        preset.Enabled = u != "off" && u != "taa";
        if (u == "dlss" && !File.Exists(Path.Combine(rootBox.Text, @"out\nvngx_dlss.dll"))) dlssModel.Checked = true;
        if (u == "fsr4" && !Directory.Exists(Path.Combine(rootBox.Text, "fsr4_shaders"))) fsr4Models.Checked = true;
    }

    // ---- the game folder ---------------------------------------------------------------------

    bool CheckGame() {
        string dir = gameBox.Text.Trim().Trim('"');
        if (dir.Length == 0) { SetGameStatus("Choose the folder of your Bloodborne dump (it holds eboot.bin and sce_sys).", false); return false; }
        if (!File.Exists(Path.Combine(dir, "eboot.bin"))) { SetGameStatus("No eboot.bin in this folder.", true); return false; }
        Dictionary<string, string> sfo = ReadParamSfo(Path.Combine(dir, @"sce_sys\param.sfo"));
        string id = Get(sfo, "TITLE_ID"), version = Get(sfo, "APP_VER"), title = Get(sfo, "TITLE");
        if (id == null) { SetGameStatus("eboot.bin found; sce_sys\\param.sfo is missing, so the version is unknown (v01.09 is needed).", true); return true; }
        string text = string.Format("{0} {1}, version {2}", title ?? "?", id, version ?? "?");
        if (version != null && !version.StartsWith("01.09"))
            SetGameStatus(text + ": bbport needs v01.09 (install the 1.09 update into the dump).", true);
        else
            SetGameStatus(text + ": OK.", false);
        return true;
    }

    void SetGameStatus(string text, bool warning) {
        gameStatus.Text = text;
        gameStatus.ForeColor = warning ? Color.Firebrick : SystemColors.GrayText;
    }

    static string Get(Dictionary<string, string> map, string key) {
        string value;
        return map != null && map.TryGetValue(key, out value) ? value : null;
    }

    /// The PS4 param.sfo key/value table: magic "\0PSF", then the key and data table offsets and
    /// 16-byte entries (key offset, format, length, max length, data offset).
    static Dictionary<string, string> ReadParamSfo(string path) {
        try {
            byte[] d = File.ReadAllBytes(path);
            if (d.Length < 20 || d[0] != 0 || d[1] != (byte)'P' || d[2] != (byte)'S' || d[3] != (byte)'F') return null;
            int keys = BitConverter.ToInt32(d, 8), data = BitConverter.ToInt32(d, 12), count = BitConverter.ToInt32(d, 16);
            var result = new Dictionary<string, string>();
            for (int i = 0; i < count; ++i) {
                int e = 20 + i * 16;
                int keyOffset = BitConverter.ToUInt16(d, e), format = BitConverter.ToUInt16(d, e + 2);
                int length = BitConverter.ToInt32(d, e + 4), dataOffset = BitConverter.ToInt32(d, e + 12);
                int k = keys + keyOffset, end = k;
                while (end < d.Length && d[end] != 0) ++end;
                string key = Encoding.ASCII.GetString(d, k, end - k);
                if (format == 0x0204 || format == 0x0004) {
                    result[key] = Encoding.UTF8.GetString(d, data + dataOffset, length).TrimEnd('\0');
                } else if (format == 0x0404) {
                    result[key] = BitConverter.ToUInt32(d, data + dataOffset).ToString();
                }
            }
            return result;
        } catch (Exception) {
            return null;
        }
    }

    // ---- settings ----------------------------------------------------------------------------

    string IniPath() { return Path.Combine(rootBox.Text, "bbport.ini"); }
    string LauncherPath() { return Path.Combine(rootBox.Text, "Bloodborne.cmd"); }

    static Dictionary<string, string> ReadKeyValues(string path, bool launcher) {
        var values = new Dictionary<string, string>();
        if (!File.Exists(path)) return values;
        foreach (string raw in File.ReadAllLines(path)) {
            string line = raw.Trim();
            if (launcher) {
                // set "KEY=VALUE"
                if (!line.StartsWith("set \"", StringComparison.OrdinalIgnoreCase) || !line.EndsWith("\"")) continue;
                line = line.Substring(5, line.Length - 6);
            } else if (line.StartsWith("#")) {
                continue;
            }
            int eq = line.IndexOf('=');
            if (eq > 0) values[line.Substring(0, eq).Trim()] = line.Substring(eq + 1).Trim();
        }
        return values;
    }

    void LoadSettings() {
        var ini = ReadKeyValues(IniPath(), false);
        var env = ReadKeyValues(LauncherPath(), true);
        Func<string, string, string> ini_or = (key, fallback) => ini.ContainsKey(key) ? ini[key] : fallback;
        Func<string, string, string> env_or = (key, fallback) => env.ContainsKey(key) ? env[key] : fallback;

        SelectValue(resolution, ini_or("output_res", DefaultResolution()));
        SelectValue(frameRate, env_or("BB_FPS", "uncap"));
        SelectValue(presentMode, env_or("BB_PRESENT_MODE", ""));
        fullscreen.Checked = ini_or("fullscreen", "1") == "1";
        showFps.Checked = ini_or("show_fps", "0") == "1";
        SelectValue(upscaler, ini_or("upscaler", "fsr3"));
        if (upscaler.SelectedIndex < 0) SelectValue(upscaler, "fsr3");
        SelectValue(preset, ini_or("preset", "0"));
        SelectValue(live, ini_or("live_resolution", "0"));
        SelectValue(modelLod, ini_or("model_lod", "0"));
        sharpen.Checked = ini_or("sharpen", "1") == "1";
        foreach (var effect in effects) {
            string fallback = "0";
            foreach (string[] e in Effects) if (e[0] == effect.Key) fallback = e[2];
            effect.Value.Checked = ini_or(effect.Key, fallback) == "1";
        }
        SelectValue(language, env_or("BB_LANGUAGE", "1"));
        string remembered = Path.Combine(rootBox.Text, @"out\game_dir.txt");
        if (File.Exists(remembered)) gameBox.Text = File.ReadAllText(remembered).Trim();
        desktopLink.Checked = true;
        startMenuLink.Checked = true;
        OnUpscaler();
        CheckGame();
    }

    static string DefaultResolution() {
        Rectangle screen = Screen.PrimaryScreen.Bounds;
        if (screen.Width >= 3840 && screen.Height >= 2160) return "3840x2160";
        if (screen.Width >= 2560 && screen.Height >= 1440) return "2560x1440";
        return "1920x1080";
    }

    Options Collect() {
        var o = new Options {
            Root = rootBox.Text.Trim().Trim('"'), Game = gameBox.Text.Trim().Trim('"'), Msys = MsysRoot(),
            RepoUrl = repoBox != null ? repoBox.Text.Trim() : null, Branch = branchBox != null ? branchBox.Text.Trim() : null,
            DlssModel = dlssModel.Enabled && dlssModel.Checked, Fsr4Models = fsr4Models.Checked,
            DesktopLink = desktopLink.Checked, StartMenuLink = startMenuLink.Checked };
        o.Ini["upscaler"] = Value(upscaler);
        o.Ini["preset"] = Value(preset);
        o.Ini["sharpen"] = sharpen.Checked ? "1" : "0";
        o.Ini["show_fps"] = showFps.Checked ? "1" : "0";
        o.Ini["output_res"] = Value(resolution);
        o.Ini["fullscreen"] = fullscreen.Checked ? "1" : "0";
        o.Ini["live_resolution"] = Value(live);
        o.Ini["model_lod"] = Value(modelLod);
        foreach (var effect in effects) o.Ini[effect.Key] = effect.Value.Checked ? "1" : "0";
        o.Env["BB_FPS"] = Value(frameRate);
        o.Env["BB_LANGUAGE"] = Value(language);
        if (Value(presentMode).Length > 0) o.Env["BB_PRESENT_MODE"] = Value(presentMode);
        if (!string.Equals(o.Msys, @"C:\msys64", StringComparison.OrdinalIgnoreCase)) o.Env["BB_MSYS2"] = o.Msys;
        return o;
    }

    /// bbport.ini: the edited keys rewritten in place, missing ones appended, the rest kept.
    static void WriteIni(string path, Dictionary<string, string> values) {
        var lines = File.Exists(path) ? new List<string>(File.ReadAllLines(path)) : new List<string>();
        var written = new HashSet<string>();
        for (int i = 0; i < lines.Count; ++i) {
            string line = lines[i];
            int eq = line.IndexOf('=');
            if (eq <= 0 || line.TrimStart().StartsWith("#")) continue;
            string key = line.Substring(0, eq).Trim();
            if (values.ContainsKey(key)) { lines[i] = key + "=" + values[key]; written.Add(key); }
        }
        if (lines.Count == 0) lines.Add("# bbport settings (in-game menu: Insert / L3+R3)");
        foreach (var kv in values) if (!written.Contains(kv.Key)) lines.Add(kv.Key + "=" + kv.Value);
        // CRLF, as the game itself writes the file on Windows (text mode).
        File.WriteAllText(path, string.Join("\r\n", lines) + "\r\n");
    }

    static void WriteLauncher(string path, Dictionary<string, string> env) {
        var text = new StringBuilder();
        text.Append("@echo off\r\n");
        text.Append("rem Bloodborne (bbport) launcher, written by setup.bat: run setup.bat again to change it.\r\n");
        text.Append("rem The other settings are in bbport.ini (in-game menu: Insert / L3+R3).\r\n");
        foreach (var kv in env) text.AppendFormat("set \"{0}={1}\"\r\n", kv.Key, kv.Value);
        text.Append("call \"%~dp0run.bat\" %*\r\n");
        text.Append("if errorlevel 1 pause\r\n");
        File.WriteAllText(path, text.ToString());
    }

    /// Settings, launcher, remembered game folder, icon and shortcuts (no build).
    void Configure(Options o) {
        Directory.CreateDirectory(Path.Combine(o.Root, "out"));
        WriteIni(Path.Combine(o.Root, "bbport.ini"), o.Ini);
        Log("Wrote " + Path.Combine(o.Root, "bbport.ini"));
        WriteLauncher(Path.Combine(o.Root, "Bloodborne.cmd"), o.Env);
        Log("Wrote " + Path.Combine(o.Root, "Bloodborne.cmd"));
        File.WriteAllText(Path.Combine(o.Root, @"out\game_dir.txt"), Path.GetFullPath(o.Game));
        string icon = MakeIcon(o);
        if (o.DesktopLink) MakeShortcut(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), o, icon);
        if (o.StartMenuLink) MakeShortcut(Environment.GetFolderPath(Environment.SpecialFolder.Programs), o, icon);
    }

    /// out\bloodborne.ico from the game's sce_sys\icon0.png (a 256x256 PNG inside an ICO).
    string MakeIcon(Options o) {
        string png = Path.Combine(o.Game, @"sce_sys\icon0.png"), ico = Path.Combine(o.Root, @"out\bloodborne.ico");
        try {
            if (!File.Exists(png)) return null;
            byte[] image;
            using (var source = new Bitmap(png))
            using (var small = new Bitmap(source, new Size(256, 256)))
            using (var stream = new MemoryStream()) {
                small.Save(stream, ImageFormat.Png);
                image = stream.ToArray();
            }
            using (var writer = new BinaryWriter(File.Create(ico))) {
                writer.Write((short)0); writer.Write((short)1); writer.Write((short)1);
                writer.Write((byte)0); writer.Write((byte)0); writer.Write((byte)0); writer.Write((byte)0);
                writer.Write((short)1); writer.Write((short)32); writer.Write(image.Length); writer.Write(22);
                writer.Write(image);
            }
            return ico;
        } catch (Exception e) {
            Log("Icon: " + e.Message);
            return null;
        }
    }

    void MakeShortcut(string folder, Options o, string icon) {
        try {
            string path = Path.Combine(folder, "Bloodborne (bbport).lnk");
            Type shellType = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(shellType);
            object link = shellType.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { path });
            Type linkType = link.GetType();
            Action<string, object> set = (name, value) => linkType.InvokeMember(name, System.Reflection.BindingFlags.SetProperty, null, link, new[] { value });
            set("TargetPath", Path.Combine(o.Root, "Bloodborne.cmd"));
            set("WorkingDirectory", o.Root);
            set("Description", "Bloodborne (bbport)");
            if (icon != null) set("IconLocation", icon + ",0");
            linkType.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod, null, link, null);
            Log("Shortcut: " + path);
        } catch (Exception e) {
            Log("Shortcut in " + folder + " failed: " + e.Message);
        }
    }

    // ---- install -----------------------------------------------------------------------------

    bool CheckInputs(bool forInstall) {
        string rootPath = rootBox.Text.Trim().Trim('"');
        if (rootPath.Length == 0) { Warn("Choose the install folder."); return false; }
        if (!checkout && rootPath.Contains(" ") && MessageBox.Show(this, "The install folder contains spaces, which the build tools handle poorly. Continue anyway?",
                Title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return false;
        if (!CheckGame()) { Warn("Choose the game folder: the Bloodborne v1.09 dump with eboot.bin."); return false; }
        if (!forInstall && !File.Exists(Path.Combine(rootPath, @"out\bb-probe.exe"))) {
            Warn("bbport is not built yet: use Install / Update.");
            return false;
        }
        return true;
    }

    void Warn(string text) { MessageBox.Show(this, text, Title, MessageBoxButtons.OK, MessageBoxIcon.Warning); }

    void SaveOnly() {
        if (!CheckInputs(false)) return;
        var o = Collect();
        try {
            Configure(o);
            statusLabel.Text = "Settings saved. Play starts the game.";
        } catch (Exception e) {
            Warn("Saving failed: " + e.Message);
        }
    }

    void StartInstall() {
        if (!CheckInputs(true)) return;
        var o = Collect();
        busy = true;
        UpdateButtons();
        tabs.SelectedTab = logPage;
        progress.Value = 0;
        var thread = new Thread(() => Install(o)) { IsBackground = true };
        thread.Start();
    }

    void Install(Options o) {
        string error = null;
        try {
            int step = 0, steps = 6;
            Step(++step, steps, "MSYS2");
            EnsureMsys2(o);
            Step(++step, steps, "MSYS2 packages (clang, cmake, SDL3, FFmpeg, Vulkan, ...)");
            Must(Bash(o, "pacman -S --needed --noconfirm " + Packages), "installing the MSYS2 packages");
            Step(++step, steps, "Sources");
            EnsureSources(o);
            Step(++step, steps, "Models");
            if (o.DlssModel) Must(Bash(o, "bash tools/fetch_dlss.sh"), "downloading the DLSS model");
            if (o.Fsr4Models) Must(Bash(o, "bash tools/fetch_fsr4_assets.sh"), "downloading the FSR 4 models");
            if (!o.DlssModel && !o.Fsr4Models) Log("(none selected)");
            Step(++step, steps, "Building bbport (several minutes the first time)");
            Must(Bash(o, "bash build.sh"), "building bbport (log above; out/gpu-build.log has the GPU library's)");
            Step(++step, steps, "Settings and shortcuts");
            Configure(o);
        } catch (Exception e) {
            error = e.Message;
        }
        BeginInvoke((Action)delegate { Finished(error); });
    }

    void Finished(string error) {
        busy = false;
        UpdateButtons();
        if (error == null) {
            progress.Value = progress.Maximum;
            statusLabel.Text = "Done. Play starts the game (also from the shortcut or Bloodborne.cmd).";
            Log("Done.");
            if (MessageBox.Show(this, "bbport is installed. Start the game now?", Title, MessageBoxButtons.YesNo,
                                MessageBoxIcon.Information) == DialogResult.Yes) Play();
        } else {
            statusLabel.Text = "Failed: " + error;
            Log("FAILED: " + error);
            MessageBox.Show(this, "Setup failed: " + error + "\n\nThe Install log tab has the details.", Title,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    void Step(int step, int steps, string what) {
        Log("");
        Log(string.Format("== [{0}/{1}] {2}", step, steps, what));
        BeginInvoke((Action)delegate {
            progress.Maximum = steps;
            progress.Value = step - 1;
            statusLabel.Text = string.Format("Step {0} of {1}: {2}", step, steps, what);
        });
    }

    static void Must(int status, string what) {
        if (status != 0) throw new Exception(what + " failed (exit code " + status + ")");
    }

    void EnsureMsys2(Options o) {
        string bash = Path.Combine(o.Msys, @"usr\bin\bash.exe");
        if (File.Exists(bash)) { Log("MSYS2 found at " + o.Msys); return; }
        if (Directory.Exists(o.Msys) && Directory.GetFileSystemEntries(o.Msys).Length > 0)
            throw new Exception(o.Msys + " exists but has no MSYS2 (usr\\bin\\bash.exe); remove it or set BB_MSYS2");
        string installer = Path.Combine(Path.GetTempPath(), "msys2-x86_64-latest.exe");
        Log("Downloading " + Msys2Installer);
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        using (var client = new WebClient()) client.DownloadFile(Msys2Installer, installer);
        Log("Installing MSYS2 to " + o.Msys + " (Windows may ask for permission)");
        string arguments = "in --confirm-command --accept-messages --root " + Quote(o.Msys.Replace('\\', '/'));
        int status;
        try {
            status = Run(installer, arguments, Path.GetTempPath(), null);
        } catch (Win32Exception e) {
            if (e.NativeErrorCode != 740) throw; // ERROR_ELEVATION_REQUIRED
            var elevated = new ProcessStartInfo(installer, arguments) { UseShellExecute = true, Verb = "runas" };
            using (var p = Process.Start(elevated)) { p.WaitForExit(); status = p.ExitCode; }
        }
        Must(status, "installing MSYS2");
        if (!File.Exists(bash)) throw new Exception("MSYS2 did not install to " + o.Msys);
        // First start: keyring and core update (the second run finishes a core update).
        Must(Bash(o, "pacman -Syu --noconfirm"), "updating MSYS2");
        Must(Bash(o, "pacman -Syu --noconfirm"), "updating MSYS2");
    }

    void EnsureSources(Options o) {
        if (File.Exists(Path.Combine(o.Root, "build.sh"))) {
            Log("Sources in " + o.Root);
            if (Directory.Exists(Path.Combine(o.Root, ".git")) || File.Exists(Path.Combine(o.Root, ".git")))
                Must(Bash(o, "git -c safe.directory='*' submodule update --init --recursive"), "fetching the submodules");
            else
                Log("Not a git checkout: the submodules must already be present (a GitHub ZIP lacks them).");
            return;
        }
        if (Directory.Exists(o.Root) && Directory.GetFileSystemEntries(o.Root).Length > 0)
            throw new Exception(o.Root + " is not empty and has no bbport sources; choose an empty or new folder");
        Directory.CreateDirectory(o.Root);
        string url = string.IsNullOrEmpty(o.RepoUrl) ? DefaultRepo : o.RepoUrl;
        string branch = string.IsNullOrEmpty(o.Branch) ? "master" : o.Branch;
        Must(Bash(o, "git clone --recursive -b '" + branch + "' '" + url + "' ."), "cloning " + url);
        if (!File.Exists(Path.Combine(o.Root, "run.bat")))
            throw new Exception("this branch has no Windows support (run.bat); choose the branch that has it");
    }

    int Bash(Options o, string command) {
        Log("$ " + command);
        var env = new Dictionary<string, string> { { "MSYSTEM", "CLANG64" }, { "CHERE_INVOKING", "1" } };
        return Run(Path.Combine(o.Msys, @"usr\bin\bash.exe"), "-lc " + Quote(command), o.Root, env);
    }

    static string Quote(string argument) {
        return "\"" + argument.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }

    int Run(string file, string arguments, string directory, Dictionary<string, string> env) {
        var info = new ProcessStartInfo(file, arguments) {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = directory,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 };
        if (env != null) foreach (var kv in env) info.EnvironmentVariables[kv.Key] = kv.Value;
        using (var p = new Process { StartInfo = info }) {
            p.OutputDataReceived += (s, e) => { if (e.Data != null) Log(e.Data); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) Log(e.Data); };
            p.Start();
            current = p;
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            p.WaitForExit();
            current = null;
            return p.ExitCode;
        }
    }

    // ---- log, buttons, play ------------------------------------------------------------------

    void Log(string line) { pending.Enqueue(line); }

    void Drain() {
        if (pending.IsEmpty) return;
        var text = new StringBuilder();
        string line;
        while (pending.TryDequeue(out line)) text.Append(line.Replace("\r", "")).Append("\r\n");
        logBox.AppendText(text.ToString());
    }

    void UpdateButtons() {
        installButton.Enabled = !busy;
        saveButton.Enabled = !busy;
        playButton.Enabled = !busy;
    }

    void Play() {
        if (!CheckInputs(false)) return;
        try {
            Configure(Collect());
            Process.Start(new ProcessStartInfo("cmd.exe", "/c \"" + LauncherPath() + "\"") {
                UseShellExecute = true, WorkingDirectory = rootBox.Text });
            statusLabel.Text = "Starting the game (its console shows the progress).";
        } catch (Exception e) {
            Warn("Starting the game failed: " + e.Message);
        }
    }

    void OnClosing(object sender, FormClosingEventArgs e) {
        if (!busy) return;
        if (MessageBox.Show(this, "Setup is still running. Stop it and close?", Title, MessageBoxButtons.YesNo,
                            MessageBoxIcon.Warning) != DialogResult.Yes) {
            e.Cancel = true;
            return;
        }
        try { if (current != null && !current.HasExited) current.Kill(); } catch (Exception) { }
    }
}

} // namespace BbportSetup
