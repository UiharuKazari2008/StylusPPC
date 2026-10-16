using System;
using System.Collections.Generic;
using System.Windows.Forms;

namespace AlphaMailSP
{
    static class Program
    {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        [MTAThread]
        static void Main()
        {
            FormMain m = new FormMain();
            Application.Run(m);
        }
    }
}