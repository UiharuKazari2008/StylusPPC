namespace AlphaMail
{
    partial class SettingsDialog
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;
        private System.Windows.Forms.MainMenu mainMenu1;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.mainMenu1 = new System.Windows.Forms.MainMenu();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPageMainGui = new System.Windows.Forms.TabPage();
            this.mainGuiSettings = new AlphaMail.MainGuiSettings();
            this.tabControl1.SuspendLayout();
            this.tabPageMainGui.SuspendLayout();
            this.SuspendLayout();
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPageMainGui);
            this.tabControl1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.tabControl1.Location = new System.Drawing.Point(0, 0);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(240, 268);
            this.tabControl1.TabIndex = 0;
            // 
            // tabPageMainGui
            // 
            this.tabPageMainGui.Controls.Add(this.mainGuiSettings);
            this.tabPageMainGui.Location = new System.Drawing.Point(0, 0);
            this.tabPageMainGui.Name = "tabPageMainGui";
            this.tabPageMainGui.Size = new System.Drawing.Size(240, 245);
            this.tabPageMainGui.Text = "Main Display";
            // 
            // mainGuiSettings
            // 
            this.mainGuiSettings.Dock = System.Windows.Forms.DockStyle.Fill;
            this.mainGuiSettings.Location = new System.Drawing.Point(0, 0);
            this.mainGuiSettings.Name = "mainGuiSettings";
            this.mainGuiSettings.Size = new System.Drawing.Size(240, 245);
            this.mainGuiSettings.TabIndex = 0;
            // 
            // Settings
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Dpi;
            this.AutoScroll = true;
            this.ClientSize = new System.Drawing.Size(240, 268);
            this.Controls.Add(this.tabControl1);
            this.Menu = this.mainMenu1;
            this.Name = "Settings";
            this.Text = "Settings";
            this.tabControl1.ResumeLayout(false);
            this.tabPageMainGui.ResumeLayout(false);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPageMainGui;
        private MainGuiSettings mainGuiSettings;
    }
}