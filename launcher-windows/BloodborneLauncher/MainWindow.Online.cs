using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text;
using System.Windows;

namespace BloodborneLauncher;

public partial class MainWindow
{
    string OnlineIdentity => state.OnlineHost.Trim().ToLowerInvariant() + ":" + state.OnlinePort + "\n" + state.OnlineUsername;
    void LoadOnlineState()
    {
        state.OnlineEnabled = ReadIniValue("online_enabled") is string enabled ? enabled=="1" : state.OnlineEnabled;
        state.OnlineConnected = ReadIniValue("online_connected") is string connected ? connected=="1" : state.OnlineConnected;
        state.OnlineUpnp = ReadIniValue("online_upnp") is string upnp ? upnp=="1" : state.OnlineUpnp;
        state.OnlineHost = ReadIniValue("online_host") ?? state.OnlineHost;
        state.OnlinePort = ReadIniValue("online_port") ?? state.OnlinePort;
        state.OnlineP2pPort = ReadIniValue("online_p2p_port") ?? state.OnlineP2pPort;
        state.OnlineRemember = ReadIniValue("online_remember") is string remember ? remember=="1" : state.OnlineRemember;
        state.OnlineWebApi = ReadIniValue("online_webapi") ?? state.OnlineWebApi;
        state.OnlineGameApi = ReadIniValue("online_game_api") ?? state.OnlineGameApi;
        state.OnlineWebsite = ReadIniValue("online_website") ?? state.OnlineWebsite;
        state.OnlineUsername = ReadIniValue("online_username") ?? state.OnlineUsername;
    }
    void ApplyOnlineUi()
    {
        OnlineEnabledCheck.IsChecked=state.OnlineEnabled; OnlineConnectedCheck.IsChecked=state.OnlineConnected;
        OnlineUpnpCheck.IsChecked=state.OnlineUpnp; OnlineRememberCheck.IsChecked=state.OnlineRemember;
        OnlineHostBox.Text=state.OnlineHost; OnlinePortBox.Text=state.OnlinePort;
        OnlineP2pPortBox.Text=state.OnlineP2pPort;
        OnlineWebApiBox.Text=state.OnlineWebApi; OnlineGameApiBox.Text=state.OnlineGameApi;
        OnlineWebsiteBox.Text=state.OnlineWebsite; OnlineUsernameBox.Text=state.OnlineUsername;
        OnlinePasswordBox.Clear(); OnlineTokenBox.Clear();
        if (state.OnlineRemember)
            try
            {
                string secret=OnlineCredentials.Load(root, OnlineIdentity);
                var stored=string.IsNullOrEmpty(secret) ? null : JsonSerializer.Deserialize<string[]>(secret);
                if (stored?.Length==2) { OnlinePasswordBox.Password=stored[0]; OnlineTokenBox.Password=stored[1]; }
            }
            catch (Exception) { OnlineStatus.Text="Introduce de nuevo la contraseña de esta cuenta."; }
    }
    void CollectOnlineUi()
    {
        state.OnlineEnabled=OnlineEnabledCheck.IsChecked==true; state.OnlineConnected=OnlineConnectedCheck.IsChecked==true;
        state.OnlineUpnp=OnlineUpnpCheck.IsChecked==true; state.OnlineRemember=OnlineRememberCheck.IsChecked==true;
        state.OnlineHost=OnlineHostBox.Text.Trim(); state.OnlinePort=OnlinePortBox.Text.Trim();
        state.OnlineP2pPort=OnlineP2pPortBox.Text.Trim();
        state.OnlineWebApi=OnlineWebApiBox.Text.Trim(); state.OnlineGameApi=OnlineGameApiBox.Text.Trim();
        state.OnlineWebsite=OnlineWebsiteBox.Text.Trim(); state.OnlineUsername=OnlineUsernameBox.Text.Trim();
    }
    void SaveOnlineSettings()
    {
        var values=new Dictionary<string,string> {
            ["online_enabled"]=state.OnlineEnabled?"1":"0", ["online_connected"]=state.OnlineConnected?"1":"0",
            ["online_upnp"]=state.OnlineUpnp?"1":"0", ["online_host"]=state.OnlineHost,
            ["online_port"]=state.OnlinePort, ["online_webapi"]=state.OnlineWebApi,
            ["online_p2p_port"]=state.OnlineP2pPort, ["online_remember"]=state.OnlineRemember?"1":"0",
            ["online_game_api"]=state.OnlineGameApi, ["online_website"]=state.OnlineWebsite,
            ["online_username"]=state.OnlineUsername };
        if (values.Values.Any(v=>v.Any(char.IsControl))) throw new IOException("La configuración online contiene caracteres no válidos.");
        OnlineProfile.P2pPort(state.OnlineP2pPort);
        RewriteIni(Path.Combine(root,"bbport.ini"),values);
        if (state.OnlineRemember && OnlinePasswordBox.Password.Length>0)
            OnlineCredentials.Save(root,OnlineIdentity,JsonSerializer.Serialize(new[]{OnlinePasswordBox.Password,OnlineTokenBox.Password}));
        else OnlineCredentials.Forget(root,OnlineIdentity);
    }
    OnlineProfile CurrentOnlineProfile() => OnlineProfile.Create(state.OnlineHost,state.OnlinePort,
        state.OnlineWebApi,state.OnlineGameApi,state.OnlineWebsite);
    bool ValidateOnlineForLaunch()
    {
        if (!state.OnlineEnabled || !state.OnlineConnected) return true;
        try
        {
            CurrentOnlineProfile();
            OnlineProfile.P2pPort(state.OnlineP2pPort);
            if (Encoding.UTF8.GetByteCount(state.OnlineUsername) is <1 or >16 || state.OnlineUsername.Any(c=>char.IsWhiteSpace(c)||char.IsControl(c)) ||
                OnlinePasswordBox.Password.Length is <1 or >4096 || OnlineTokenBox.Password.Length>4096)
                throw new IOException("Introduce tu nombre de cuenta shadNet (hasta 16 bytes en UTF-8) y su contraseña.");
            return true;
        }
        catch(Exception e) { MessageBox.Show(e.Message,"Online",MessageBoxButton.OK,MessageBoxImage.Warning);return false; }
    }
    void ConfigureOnlineLaunch(ProcessStartInfo start)
    {
        foreach(var name in new[]{"BB_ONLINE","BB_SHADNET_SERVER","BB_SHADNET_WEBAPI","BB_SHADNET_NPID","BB_SHADNET_PASSWORD","BB_SHADNET_TOKEN","BB_UPNP","SHADPS4_P2P_PORT","SHADPS4_HTTP_HOST_OVERRIDES_JSON"}) start.Environment.Remove(name);
        start.Environment["BB_ONLINE"]="0";
        if (!state.OnlineEnabled || !state.OnlineConnected) return;
        var p=CurrentOnlineProfile();
        start.Environment["BB_ONLINE"]="1"; start.Environment["BB_SHADNET_SERVER"]=p.Server;
        start.Environment["BB_SHADNET_WEBAPI"]=p.WebApi; start.Environment["BB_SHADNET_NPID"]=state.OnlineUsername;
        start.Environment["BB_SHADNET_PASSWORD"]=OnlinePasswordBox.Password;
        start.Environment["BB_SHADNET_TOKEN"]=OnlineTokenBox.Password;
        start.Environment["BB_UPNP"]=state.OnlineUpnp?"1":"0";
        string port=OnlineProfile.P2pPort(state.OnlineP2pPort);
        if (port.Length>0) start.Environment["SHADPS4_P2P_PORT"]=port;
    }
    async void OnlineProbe_Click(object sender,RoutedEventArgs e)
    {
        CollectOnlineUi(); OnlineProbeButton.IsEnabled=false;OnlineStatus.Text="Comprobando conexión y rutas del juego…";
        try { OnlineStatus.Text=await CurrentOnlineProfile().ProbeAsync(); }
        catch(Exception error) { OnlineStatus.Text=error.Message; }
        finally { OnlineProbeButton.IsEnabled=true; }
    }
    void OnlineWebsite_Click(object sender,RoutedEventArgs e)
    {
        try
        {
            string url=OnlineProfile.HttpBase(OnlineWebsiteBox.Text,"Página de cuentas");
            Process.Start(new ProcessStartInfo(url){UseShellExecute=true});
        }
        catch(Exception error) { OnlineStatus.Text=error.Message; }
    }
}
