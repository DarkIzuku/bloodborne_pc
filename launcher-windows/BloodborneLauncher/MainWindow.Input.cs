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
        ("move_forward","Move forward","W"),("move_back","Move back","S"),("move_left","Move left","A"),("move_right","Move right","D"),
        ("walk","Walk (hold)","Left Alt"),("roll","Dodge / run","Space"),("lock_on","Lock on","Q"),
        ("attack","Attack","Mouse1"),("strong_attack","Strong attack","V"),("strong_modifier","Strong attack modifier","Left Shift"),
        ("transform","Transform weapon","C, Mouse3"),("firearm","Firearm / parry","X, Mouse2"),
        ("blood_vial","Blood vial","R"),("use_item","Use item","F"),("interact","Interact","E"),
        ("switch_item","Switch item","Down"),("switch_right","Switch right weapon","Right"),("switch_left","Switch left weapon","Left"),
        ("dpad_up","D-pad up","Up"),("gestures","Gestures","G"),("personal_effects","Personal effects","T"),
        ("look_up","Camera up","I"),("look_down","Camera down","K"),("look_left","Camera left","J"),("look_right","Camera right","L"),
        ("menu","Menu","Escape"),("confirm","Confirm in menus","Return"),("back","Back in menus","Backspace"),("l3","Press left stick (L3)","Left Ctrl")
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
            if(v.Length>120 || v.Any(c=>c=='=' || c=='\r' || c=='\n'))throw new InvalidOperationException("Invalid key binding: "+row.Action);
            ini["bind."+row.Key]=v.Length==0?"none":v;
        }
    }
    void ResetKeys_Click(object sender,System.Windows.RoutedEventArgs e)
    {
        for(int i=0;i<PcDefaults.Length;i++)keyBindings[i].Binding=PcDefaults[i].binding;
        KeyBindingsGrid.Items.Refresh();
    }
}
