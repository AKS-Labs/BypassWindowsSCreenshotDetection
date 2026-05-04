using System.Collections.Generic;
using System.Diagnostics;
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

        private void Refresh(object sender, RoutedEventArgs e)
        {
            ProcessBindTest.Clear();

            var strings = new List<string>();
            foreach (var process in Process.GetProcesses())
            {
                var proc = Injector.GetProcessInfo(process);

                if (proc.Modules.Count == 0 || proc.WindowHandle == IntPtr.Zero)
                    continue;                

                string fileName = Path.GetFileName(proc.Modules.First().Value.Path);
                proc.FileName = fileName;

                ProcessBindTest.Add(proc);
            }

            ProcessBindTest = ProcessBindTest.OrderBy(x => x.ToString(), StringComparer.OrdinalIgnoreCase).ToList(); // I tried using data bindings, but MVVM just sucks
            foreach(var proc in ProcessBindTest)
            {
                Processes.Items.Add(proc);
            }
        }

        private void CallExport(ProcessInfo process, PeFile dll, IntPtr hModule, string exportName)
        {
            try
            {
                int rva = dll.GetExportAddress(exportName);
                IntPtr exportAddress = IntPtr.Add(hModule, rva);
                Injector.CallExport(process, exportAddress);
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Failed to call {exportName}: {ex.Message}");
            }
        }

        private void Inject(object sender, RoutedEventArgs e)
        {
            var selected = Processes.SelectedItem as ProcessInfo;

            if (selected == null)
                return;

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;

            IntPtr hModule = Injector.Inject(selected, dll);

            if (hModule != IntPtr.Zero && BypassScreenshot.IsChecked == true)
            {
                CallExport(selected, dll, hModule, "EnableScreenshotBypass");
            }

            Processes.Items.Remove(selected);
            InjectedProcesses.Items.Add(selected);
        }

        private void Unload(object sender, RoutedEventArgs e)
        {
            var selected = InjectedProcesses.SelectedItem as ProcessInfo;

            if (selected == null)
                return;

            PeFile dll = selected.Is64Bit ? Dll64 : Dll32;

            // Cleanly disable the bypass thread before unloading
            // (best-effort – if it fails, unload will still run DLL_PROCESS_DETACH which cleans up)
            try
            {
                // Re-get process info to find current module handle
                var fresh = Injector.GetProcessInfo((int)selected.Id);
                foreach (var mod in fresh.Modules.Values)
                {
                    if (mod.Path.ToUpperInvariant().Contains("NOFOCUSLOSS"))
                    {
                        CallExport(selected, dll, mod.MemoryAddress, "DisableScreenshotBypass");
                        break;
                    }
                }
            }
            catch { }

            Injector.Unload(selected, dll);

            InjectedProcesses.Items.Remove(selected);
            Processes.Items.Add(selected);
        }
    }
}
