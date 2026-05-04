using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using SharpestInjector;
using System.Windows;
using System.Linq;
using System.IO;
using System;

namespace NoFocusLossGUI
{
    /// <summary>
    /// Interaction logic for MainWindow.xaml
    /// </summary>
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            Dll32 = PeFile.Parse("NoFocusLoss.dll");
            Dll64 = PeFile.Parse("NoFocusLoss64.dll");
        }

        public List<ProcessInfo> ProcessBindTest = new List<ProcessInfo>();
        PeFile Dll32;
        PeFile Dll64;

        // Keep event handles alive so the DLL's init thread can open them
        private readonly Dictionary<uint, EventWaitHandle> _bypassEvents
            = new Dictionary<uint, EventWaitHandle>();

        private void Refresh(object sender, RoutedEventArgs e)
        {
            ProcessBindTest.Clear();
            Processes.Items.Clear();

            foreach (var process in Process.GetProcesses())
            {
                var proc = Injector.GetProcessInfo(process);
                if (proc.Modules.Count == 0 || proc.WindowHandle == IntPtr.Zero)
                    continue;

                proc.FileName = Path.GetFileName(proc.Modules.First().Value.Path);
                ProcessBindTest.Add(proc);
            }

            ProcessBindTest = ProcessBindTest
                .OrderBy(x => x.ToString(), StringComparer.OrdinalIgnoreCase)
                .ToList();

            foreach (var proc in ProcessBindTest)
                Processes.Items.Add(proc);
        }

        private void Inject(object sender, RoutedEventArgs e)
        {
            var selected = Processes.SelectedItem as ProcessInfo;
            if (selected == null) return;

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;

            // Create a named Windows event BEFORE injection.
            // The DLL's init thread opens this event to know bypass is requested.
            if (BypassScreenshot.IsChecked == true)
            {
                string evtName = $"Local\\NFL_Bypass_{selected.Id}";
                var bypassEvt = new EventWaitHandle(
                    true,                        // signalled = bypass is ON
                    EventResetMode.ManualReset,
                    evtName);

                _bypassEvents[selected.Id] = bypassEvt;

                // Close our handle after 2 s — DLL has read it by then
                var capture = bypassEvt;
                var id      = selected.Id;
                ThreadPool.QueueUserWorkItem(_ =>
                {
                    Thread.Sleep(2000);
                    capture.Close();
                    lock (_bypassEvents) _bypassEvents.Remove(id);
                });
            }

            Injector.Inject(selected, dll);

            Processes.Items.Remove(selected);
            InjectedProcesses.Items.Add(selected);
        }

        private void Unload(object sender, RoutedEventArgs e)
        {
            var selected = InjectedProcesses.SelectedItem as ProcessInfo;
            if (selected == null) return;

            // Clean up any lingering event handle
            lock (_bypassEvents)
            {
                if (_bypassEvents.TryGetValue(selected.Id, out var evt))
                {
                    evt.Close();
                    _bypassEvents.Remove(selected.Id);
                }
            }

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;
            Injector.Unload(selected, dll);

            InjectedProcesses.Items.Remove(selected);
            Processes.Items.Add(selected);
        }
    }
}
