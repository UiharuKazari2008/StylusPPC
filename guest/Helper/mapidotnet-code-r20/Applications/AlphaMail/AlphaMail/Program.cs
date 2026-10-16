using System;
using System.Collections.Generic;
using System.Windows.Forms;
using System.Drawing;
using MailGuis;

namespace AlphaMail
{
    static class Program
    {
        /// <summary>
        /// The main entry point for the application.
        /// </summary>
        [MTAThread]
        static void Main()
        {
            FormMain f = new FormMain();
            Application.Run(f);
        }
    }
}