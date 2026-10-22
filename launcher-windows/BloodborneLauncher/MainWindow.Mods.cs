using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using System.Xml.Linq;

namespace BloodborneLauncher;

public partial class MainWindow
{
    string ModsConfigPath => Path.Combine(root, "mods.json");
    string PatchesConfigPath => Path.Combine(root, "patches.json");
    static readonly HashSet<string> BloodborneIds = new() {
        "CUSA00207", "CUSA00208", "CUSA00900", "CUSA01363", "CUSA03173", "CUSA03023"
    };
    string PatchKey(string file)
    {
        string mods = Path.Combine(root, "mods");
        string relative = Path.GetRelativePath(mods, file).Replace('\\', '/');
        return !relative.StartsWith("../", StringComparison.Ordinal) ? "mods/" + relative : Path.GetFileName(file);
    }

    static JsonObject ReadJson(string path) => !File.Exists(path) ? new() :
        JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? throw new IOException("Invalid mod settings: " + Path.GetFileName(path));

    static HashSet<string> JsonNames(JsonObject config, string key)
    {
        if (config[key] == null) return new();
        if (config[key] is not JsonArray values || values.Any(n => n is not JsonValue v || !v.TryGetValue<string>(out _)))
            throw new IOException("Invalid mod setting: " + key);
        return values.Select(n => n!.GetValue<string>()).ToHashSet();
    }

    // (key, patch name, isEnabled) of the 1.09 eboot patches in dir's XML files, as patches.py external_patches.
    List<(string Key, string Name, bool Default)> PatchesIn(string dir, SearchOption depth)
    {
        var found = new List<(string, string, bool)>();
        if (!Directory.Exists(dir)) return found;
        string builtIn = Path.Combine(root, "patches", "Bloodborne.xml");
        foreach (var file in Directory.GetFiles(dir, "*.xml", depth).Order(StringComparer.OrdinalIgnoreCase))
        {
            if (string.Equals(Path.GetFullPath(file), builtIn, StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                var xml = XDocument.Load(file);
                var ids = xml.Descendants("ID").Select(e => e.Value.Trim()).ToList();
                if (ids.Count > 0 && !ids.Any(BloodborneIds.Contains)) continue;
                foreach (var meta in xml.Descendants("Metadata"))
                    if ((string?)meta.Attribute("AppVer") == "01.09" && ((string?)meta.Attribute("AppElf") ?? "eboot.bin") == "eboot.bin")
                        found.Add(($"{PatchKey(file)}/{(string?)meta.Attribute("Name")}", (string?)meta.Attribute("Name") ?? "",
                                   string.Equals((string?)meta.Attribute("isEnabled"), "true", StringComparison.OrdinalIgnoreCase)));
            }
            catch { }  // patches.py reports unreadable files at launch
        }
        return found;
    }

    bool modSettingsValid = true;
    void LoadModsList()
    {
        try { LoadModsListCore(); modSettingsValid = true; }
        catch (Exception error) {
            modSettingsValid = false;
            ModsList.Children.Clear();
            FooterMessage.Text = error.Message;
        }
    }
    void LoadModsListCore()
    {
        var disabledMods = JsonNames(ReadJson(ModsConfigPath), "disabled");
        var patchConfig = ReadJson(PatchesConfigPath);
        var enabledPatches = JsonNames(patchConfig, "enabled");
        var disabledPatches = JsonNames(patchConfig, "disabled");
        // Rebuilt on every visit to the Mods page; switches not saved yet keep their state.
        var shown = ModsList.Children.OfType<CheckBox>().ToDictionary(c => c.Tag.ToString()!, c => c.IsChecked == true);
        ModsList.Children.Clear();
        CheckBox Add(string tag, string label, bool on, double indent, CheckBox? parent = null)
        {
            var box = new CheckBox { Content = label, Tag = tag, Margin = new Thickness(indent, 4, 0, 0),
                                     IsChecked = shown.TryGetValue(tag, out bool current) ? current : on };
            if (parent != null) box.SetBinding(IsEnabledProperty, new System.Windows.Data.Binding("IsChecked") { Source = parent });
            ModsList.Children.Add(box);
            return box;
        }
        void AddPatches(IEnumerable<(string Key, string Name, bool Default)> patches, double indent, CheckBox? parent)
        {
            foreach (var p in patches)
                Add("patch:" + p.Key, "Patch: " + p.Name, enabledPatches.Contains(p.Key) || (!disabledPatches.Contains(p.Key) && p.Default), indent, parent);
        }
        if (Directory.Exists(Path.Combine(root, "mods")))
            foreach (var dir in Directory.GetDirectories(Path.Combine(root, "mods")).Order(StringComparer.OrdinalIgnoreCase))
            {
                string name = Path.GetFileName(dir);
                var mod = Add("mod:" + name, File.Exists(Path.Combine(dir, "eboot.bin")) ? name + " (eboot.bin)" : name, !disabledMods.Contains(name), 0);
                AddPatches(PatchesIn(dir, SearchOption.AllDirectories), 24, mod);
            }
        AddPatches(PatchesIn(Path.Combine(root, "mods"), SearchOption.TopDirectoryOnly), 0, null);
        AddPatches(PatchesIn(Path.Combine(root, "patches"), SearchOption.TopDirectoryOnly), 0, null);
    }

    void SaveModsList()
    {
        if (!modSettingsValid) throw new IOException("Repair mods.json / patches.json before saving mod settings.");
        var boxes = ModsList.Children.OfType<CheckBox>().Select(c => (Tag: c.Tag.ToString()!, On: c.IsChecked == true)).ToList();
        if (boxes.Count == 0) return;
        var mods = ReadJson(ModsConfigPath);
        var disabledMods = JsonNames(mods, "disabled");
        foreach (var (tag, on) in boxes.Where(b => b.Tag.StartsWith("mod:"))) {
            if (on) disabledMods.Remove(tag[4..]); else disabledMods.Add(tag[4..]);
        }
        mods["disabled"] = new JsonArray(disabledMods.Order().Select(k => (JsonNode?)k).ToArray());
        WriteSafely(ModsConfigPath, mods.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        // Patches not shown (their file is gone) keep their entries.
        var patches = ReadJson(PatchesConfigPath);
        var enabled = JsonNames(patches, "enabled");
        var disabled = JsonNames(patches, "disabled");
        foreach (var (tag, on) in boxes.Where(b => b.Tag.StartsWith("patch:")))
        {
            string key = tag[6..];
            (on ? enabled : disabled).Add(key);
            (on ? disabled : enabled).Remove(key);
        }
        patches["enabled"] = new JsonArray(enabled.Order().Select(k => (JsonNode?)k).ToArray());
        patches["disabled"] = new JsonArray(disabled.Order().Select(k => (JsonNode?)k).ToArray());
        WriteSafely(PatchesConfigPath, patches.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }


}
