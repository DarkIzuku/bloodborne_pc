using System.IO;
using System.Windows.Controls;
using System.Globalization;
using System.Collections.ObjectModel;

namespace BloodborneLauncher;
public partial class MainWindow
{
    public sealed class KeyBindingRow
    {
        public string Action { get; set; } = "";
        public string Binding { get; set; } = "";
        public string Key { get; set; } = "";
    }
    readonly ObservableCollection<KeyBindingRow> keyBindings = new();
    static readonly (string key,string label,string binding)[] PcDefaults =
    {
        ("move_forward","Avanzar","W"),("move_back","Retroceder","S"),("move_left","Izquierda","A"),("move_right","Derecha","D"),
        ("walk","Caminar (mantener)","Left Alt"),("roll","Esquivar / correr","Space"),("lock_on","Fijar objetivo","Q"),
        ("attack","Ataque","Mouse1"),("strong_attack","Ataque fuerte","V"),("strong_modifier","Modificador de ataque fuerte","Left Shift"),
        ("transform","Transformar arma","C, Mouse3"),("firearm","Arma de fuego / parry","X, Mouse2"),
        ("blood_vial","Vial de sangre","R"),("use_item","Usar objeto","F"),("interact","Interactuar","E"),
        ("switch_item","Cambiar objeto","Down"),("switch_right","Cambiar arma derecha","Right"),("switch_left","Cambiar arma izquierda","Left"),
        ("dpad_up","Cruceta arriba","Up"),("gestures","Gestos","G"),("personal_effects","Efectos personales","T"),
        ("look_up","Cámara arriba","I"),("look_down","Cámara abajo","K"),("look_left","Cámara izquierda","J"),("look_right","Cámara derecha","L"),
        ("menu","Menú","Escape"),("confirm","Aceptar en menús","Return"),("back","Volver en menús","Backspace"),("l3","Pulsar stick izquierdo","Left Ctrl")
    };
    void LoadInputState()
    {
        state.PcControls=ReadIniValue("pc_controls")!="0";
        state.Widescreen=ReadIniValue("widescreen")!="0";
        state.MouseCamera=ReadIniValue("mouse_camera")!="0";
        state.MouseMenu=ReadIniValue("mouse_menu")!="0";
        state.MouseInvertX=ReadIniValue("mouse_invert_x")=="1";
        state.MouseInvertY=ReadIniValue("mouse_invert_y")=="1";
        state.MouseAutoRotation=ReadIniValue("mouse_auto_rotation")=="1";
        state.MouseSensitivity=int.TryParse(ReadIniValue("mouse_sensitivity"),out int n)?Math.Clamp(n,0,10):5;
        keyBindings.Clear();
        foreach(var d in PcDefaults)keyBindings.Add(new(){Key=d.key,Action=d.label,Binding=ReadIniValue("bind."+d.key)??d.binding});
    }
    void ApplyInputUi()
    {
        PcControlsCheck.IsChecked=state.PcControls;WidescreenCheck.IsChecked=state.Widescreen;
        MouseCameraCheck.IsChecked=state.MouseCamera;MouseMenuCheck.IsChecked=state.MouseMenu;
        MouseInvertXCheck.IsChecked=state.MouseInvertX;MouseInvertYCheck.IsChecked=state.MouseInvertY;
        MouseAutoRotationCheck.IsChecked=state.MouseAutoRotation;MouseSensitivitySlider.Value=state.MouseSensitivity;
        KeyBindingsGrid.ItemsSource=keyBindings;
    }
    void CollectInputUi()
    {
        KeyBindingsGrid.CommitEdit();KeyBindingsGrid.CommitEdit(DataGridEditingUnit.Row,true);
        state.PcControls=PcControlsCheck.IsChecked==true;state.Widescreen=WidescreenCheck.IsChecked==true;
        state.MouseCamera=MouseCameraCheck.IsChecked==true;state.MouseMenu=MouseMenuCheck.IsChecked==true;
        state.MouseInvertX=MouseInvertXCheck.IsChecked==true;state.MouseInvertY=MouseInvertYCheck.IsChecked==true;
        state.MouseAutoRotation=MouseAutoRotationCheck.IsChecked==true;state.MouseSensitivity=(int)MouseSensitivitySlider.Value;
    }
    void SaveInputSettings(Dictionary<string,string> ini)
    {
        ini["pc_controls"]=state.PcControls?"1":"0";ini["widescreen"]=state.Widescreen?"1":"0";
        ini["mouse_camera"]=state.MouseCamera?"1":"0";ini["mouse_menu"]=state.MouseMenu?"1":"0";
        ini["mouse_invert_x"]=state.MouseInvertX?"1":"0";ini["mouse_invert_y"]=state.MouseInvertY?"1":"0";
        ini["mouse_auto_rotation"]=state.MouseAutoRotation?"1":"0";
        ini["mouse_sensitivity"]=state.MouseSensitivity.ToString(CultureInfo.InvariantCulture);
        foreach(var row in keyBindings)
        {
            string v=row.Binding.Trim();
            if(v.Length>120 || v.Any(c=>c=='=' || c=='\r' || c=='\n'))throw new InvalidOperationException("Asignación de tecla inválida: "+row.Action);
            ini["bind."+row.Key]=v.Length==0?"none":v;
        }
    }
    void ResetKeys_Click(object sender,System.Windows.RoutedEventArgs e)
    {
        for(int i=0;i<PcDefaults.Length;i++)keyBindings[i].Binding=PcDefaults[i].binding;
        KeyBindingsGrid.Items.Refresh();
    }
}
