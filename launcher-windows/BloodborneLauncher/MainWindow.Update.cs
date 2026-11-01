using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;

namespace BloodborneLauncher;

// Self-update from the upstream GitHub releases. The package zip is flat and ships no bbport.ini,
// launcher-settings.json or game dump, so extracting it over the install keeps the user's files.
public partial class MainWindow
{
    const string UpdateRepo = "DarkIzuku/bloodborne_pc";
    string? updateTag, updateZipUrl, updateSumsUrl;
    string ReleaseTagFile => Path.Combine(root, "out", "release_tag.txt");

    // Only a portable package (launcher exe in the root) updates itself; dev builds and the test harness stay offline.
    bool IsPortableInstall => Environment.ProcessPath is string exe &&
        Path.GetFileName(exe).Equals("BloodborneLauncher.exe", StringComparison.OrdinalIgnoreCase) &&
        string.Equals(Path.GetDirectoryName(exe), root, StringComparison.OrdinalIgnoreCase);

    static HttpClient UpdateHttp(int seconds)
    {
        var http = new HttpClient { Timeout = TimeSpan.FromSeconds(seconds) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("BloodborneLauncher"); // GitHub's API rejects requests without one
        return http;
    }

    // SHA256SUMS lines are "<hex>  <name>" (sha256sum format, optional '*' for binary mode).
    static string? ExpectedSha256(string sums, string fileName) =>
        sums.Split('\n').Select(line => line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
            .FirstOrDefault(parts => parts.Length == 2 && parts[1].TrimStart('*') == fileName)?[0];

    async void CheckForUpdate()
    {
        if (!IsPortableInstall) return;
        try { File.Delete(Environment.ProcessPath + ".old"); } catch { }
        try
        {
            using var http = UpdateHttp(10);
            using var release = JsonDocument.Parse(await http.GetStringAsync($"https://api.github.com/repos/{UpdateRepo}/releases/latest"));
            string tag = release.RootElement.GetProperty("tag_name").GetString() ?? "";
            string installed = File.Exists(ReleaseTagFile) ? File.ReadAllText(ReleaseTagFile).Trim() : "";
            if (tag == "" || tag == installed) return;
            foreach (var asset in release.RootElement.GetProperty("assets").EnumerateArray())
            {
                string name = asset.GetProperty("name").GetString() ?? "";
                string url = asset.GetProperty("browser_download_url").GetString() ?? "";
                if (name.EndsWith("-Windows-x64.zip", StringComparison.OrdinalIgnoreCase)) updateZipUrl = url;
                else if (name.StartsWith("SHA256SUMS", StringComparison.OrdinalIgnoreCase)) updateSumsUrl = url;
            }
            if (updateZipUrl == null || updateSumsUrl == null) return; // never install an unverifiable package
            updateTag = tag;
            UpdateButton.Content = installed == "" ? $"Install latest release ({tag})" : $"Update {installed} → {tag}";
            UpdateButton.Visibility = Visibility.Visible;
        }
        catch { } // offline or rate-limited: the launcher works without the check
    }

    async void Update_Click(object sender, RoutedEventArgs e)
    {
        if (updateTag == null || updateZipUrl == null || updateSumsUrl == null || Environment.ProcessPath is not string exe) return;
        if (gameRunning) { MessageBox.Show(this, "Close the game before updating.", "Update", MessageBoxButton.OK, MessageBoxImage.Information); return; }
        if (MessageBox.Show(this, $"Download and install {updateTag} from github.com/{UpdateRepo}?\n\n" +
                "The launcher and game files are replaced, including anything you edited inside patches\\ or scripts\\. " +
                "Your settings, mods folder and game dump are kept. The launcher restarts when it is done.",
                "Update", MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;

        string label = UpdateButton.Content as string ?? "";
        string zip = Path.Combine(Path.GetTempPath(), $"BloodborneLauncher-update-{Guid.NewGuid():N}.zip");
        string old = exe + ".old";
        bool moved = false;
        var restart = new ProcessStartInfo(exe) { UseShellExecute = true, WorkingDirectory = root }; // loaded before the exe is renamed
        UpdateButton.IsEnabled = false;
        try
        {
            using var http = UpdateHttp(1800);
            string zipName = Path.GetFileName(new Uri(updateZipUrl).LocalPath);
            string expected = ExpectedSha256(await http.GetStringAsync(updateSumsUrl), zipName)
                ?? throw new IOException("The release lists no checksum for " + zipName + ". Nothing was changed.");

            using (var response = await http.GetAsync(updateZipUrl, HttpCompletionOption.ResponseHeadersRead))
            {
                response.EnsureSuccessStatusCode();
                long total = response.Content.Headers.ContentLength ?? 0, done = 0;
                await using var source = await response.Content.ReadAsStreamAsync();
                await using var target = File.Create(zip);
                var buffer = new byte[1 << 20];
                for (int read; (read = await source.ReadAsync(buffer)) > 0;)
                {
                    await target.WriteAsync(buffer.AsMemory(0, read));
                    done += read;
                    UpdateButton.Content = total > 0 ? $"Downloading… {done * 100 / total}%" : $"Downloading… {done >> 20} MB";
                }
            }

            UpdateButton.Content = "Verifying…";
            string actual;
            await using (var file = File.OpenRead(zip)) actual = Convert.ToHexString(await SHA256.HashDataAsync(file));
            if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new IOException("Checksum mismatch: the download is corrupt. Nothing was changed.");

            UpdateButton.Content = "Installing…";
            // Open the archive before renaming: a single-file launcher loads ZipFile from its own exe on first use.
            using (var archive = ZipFile.OpenRead(zip))
            {
                File.Move(exe, old, true); // Windows lets a running exe be renamed, not overwritten
                moved = true;
                await Task.Run(() => archive.ExtractToDirectory(root, overwriteFiles: true)); // rejects entries escaping root
            }
            File.WriteAllText(ReleaseTagFile, updateTag);
        }
        catch (Exception ex)
        {
            // Extraction is not atomic: put the old launcher back; the tag is unchanged, so the button offers a full retry.
            if (moved) try { File.Move(old, exe, true); } catch { }
            UpdateButton.Content = label;
            UpdateButton.IsEnabled = true;
            MessageBox.Show(this, "Update failed: " + ex.Message + "\n\nSome files may already be new. Press the update button again to finish.",
                "Update", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        finally { try { File.Delete(zip); } catch { } }

        try { Process.Start(restart); }
        catch { MessageBox.Show(this, $"{updateTag} is installed. Start the launcher again to use it.", "Update"); }
        Application.Current.Shutdown();
    }
}
