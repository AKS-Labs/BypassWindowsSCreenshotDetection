using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Interop;
using SharpestInjector;
using System.Windows;
using System.Linq;
using System.IO;
using System;

namespace NoFocusLossGUI
{
    public partial class MainWindow : Window
    {
        // ── DWM Mica backdrop ────────────────────────────────────────
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int val, int size);
        [DllImport("dwmapi.dll")] static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref MARGINS m);
        [StructLayout(LayoutKind.Sequential)]
        struct MARGINS { public int L, R, T, B; }

        void ApplyMica()
        {
            try
            {
                var hwnd = new WindowInteropHelper(this).Handle;
                // DWMWA_USE_IMMERSIVE_DARK_MODE = 20
                int dark = 1;
                DwmSetWindowAttribute(hwnd, 20, ref dark, 4);
                // DWMWA_SYSTEMBACKDROP_TYPE = 38  (2 = Mica, 3 = Acrylic, 4 = Mica Alt)
                int mica = 2;
                DwmSetWindowAttribute(hwnd, 38, ref mica, 4);
                // Extend frame into full client area so Mica fills window
                var mg = new MARGINS { L = -1, R = -1, T = -1, B = -1 };
                DwmExtendFrameIntoClientArea(hwnd, ref mg);
            }
            catch { /* Older Windows — silently ignore */ }
        }

        // ── Fields ───────────────────────────────────────────────────
        public List<ProcessInfo> ProcessBindTest = new List<ProcessInfo>();
        PeFile Dll32;
        PeFile Dll64;
        private readonly Dictionary<uint, List<EventWaitHandle>> _events
            = new Dictionary<uint, List<EventWaitHandle>>();

        public MainWindow()
        {
            InitializeComponent();
            Dll32 = PeFile.Parse("NoFocusLoss.dll");
            Dll64 = PeFile.Parse("NoFocusLoss64.dll");
        }

        protected override void OnSourceInitialized(EventArgs e)
        {
            base.OnSourceInitialized(e);
            ApplyMica();
        }

        // ── Window chrome buttons ────────────────────────────────────
        private void MinimizeClick(object s, RoutedEventArgs e)  => WindowState = WindowState.Minimized;
        private void CloseClick(object s, RoutedEventArgs e)     => Close();
        private void MaxRestoreClick(object s, RoutedEventArgs e)
        {
            WindowState = WindowState == WindowState.Maximized
                ? WindowState.Normal : WindowState.Maximized;
            MaxRestoreBtn.Content = WindowState == WindowState.Maximized ? "\uE923" : "\uE922";
        }

        // ── Refresh ──────────────────────────────────────────────────
        private void Refresh(object s, RoutedEventArgs e)
        {
            ProcessBindTest.Clear();
            Processes.Items.Clear();
            SetStatus("Scanning processes…");

            foreach (var proc in Process.GetProcesses())
            {
                var info = Injector.GetProcessInfo(proc);
                if (info.Modules.Count == 0 || info.WindowHandle == IntPtr.Zero) continue;
                info.FileName = Path.GetFileName(info.Modules.First().Value.Path);
                ProcessBindTest.Add(info);
            }

            ProcessBindTest = ProcessBindTest
                .OrderBy(x => x.ToString(), StringComparer.OrdinalIgnoreCase).ToList();
            foreach (var p in ProcessBindTest) Processes.Items.Add(p);

            SetStatus($"{ProcessBindTest.Count} processes found — select one and inject");
        }

        // ── Core inject helper ────────────────────────────────────────
        private void DoInject(bool focus, bool screenshot, bool textCopy)
        {
            var selected = Processes.SelectedItem as ProcessInfo;
            if (selected == null) { SetStatus("⚠ Select a process first"); return; }

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;
            var handles = new List<EventWaitHandle>();

            if (focus)      handles.Add(CreateSignal($"Local\\NFL_Focus_{selected.Id}"));
            if (screenshot) handles.Add(CreateSignal($"Local\\NFL_Bypass_{selected.Id}"));
            if (textCopy)   handles.Add(CreateSignal($"Local\\NFL_TextCopy_{selected.Id}"));

            Injector.Inject(selected, dll);

            var capturedHandles = handles;
            var capturedId = selected.Id;
            ThreadPool.QueueUserWorkItem(_ =>
            {
                Thread.Sleep(2000);
                foreach (var h in capturedHandles) h.Close();
                lock (_events) _events.Remove(capturedId);
            });

            Processes.Items.Remove(selected);
            InjectedProcesses.Items.Add(selected);

            var parts = new System.Collections.Generic.List<string>();
            if (focus)      parts.Add("Focus Fix");
            if (screenshot) parts.Add("Screenshot Bypass");
            if (textCopy)   parts.Add("Text Copy");
            SetStatus($"✓ Injected [{string.Join(" + ", parts)}] into {selected}");
        }

        private EventWaitHandle CreateSignal(string name)
        {
            return new EventWaitHandle(true, EventResetMode.ManualReset, name);
        }

        // ── Three inject buttons ──────────────────────────────────────
        private void InjectFocus(object s, RoutedEventArgs e)      => DoInject(focus: true,  screenshot: false, textCopy: false);
        private void InjectScreenshot(object s, RoutedEventArgs e) => DoInject(focus: false, screenshot: true,  textCopy: false);
        private void InjectTextCopy(object s, RoutedEventArgs e)   => DoInject(focus: false, screenshot: false, textCopy: true);
        private void InjectBoth(object s, RoutedEventArgs e)       => DoInject(focus: true,  screenshot: true,  textCopy: true);

        // ── Unload ────────────────────────────────────────────────────
        private void Unload(object s, RoutedEventArgs e)
        {
            var selected = InjectedProcesses.SelectedItem as ProcessInfo;
            if (selected == null) { SetStatus("⚠ Select an injected process first"); return; }

            lock (_events)
            {
                if (_events.TryGetValue(selected.Id, out var evts))
                { foreach (var ev in evts) ev.Close(); _events.Remove(selected.Id); }
            }

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;
            Injector.Unload(selected, dll);
            InjectedProcesses.Items.Remove(selected);
            Processes.Items.Add(selected);
            SetStatus($"Unloaded from {selected}");
        }

        // ── Helpers ───────────────────────────────────────────────────
        private void SetStatus(string msg) => StatusText.Text = msg;
    }
}
