namespace AlphaMail
{
    partial class MainGuiSettings
    {
        /// <summary> 
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

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

        #region Component Designer generated code

        /// <summary> 
        /// Required method for Designer support - do not modify 
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.label1 = new System.Windows.Forms.Label();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPageGeneral = new System.Windows.Forms.TabPage();
            this.numericUpDownLargeBannerIndent = new System.Windows.Forms.NumericUpDown();
            this.label7 = new System.Windows.Forms.Label();
            this.numericUpDownSmallBannerIndent = new System.Windows.Forms.NumericUpDown();
            this.numericUpDownIndent = new System.Windows.Forms.NumericUpDown();
            this.label6 = new System.Windows.Forms.Label();
            this.label5 = new System.Windows.Forms.Label();
            this.tabPageColors = new System.Windows.Forms.TabPage();
            this.colorChooserSelectedBanner = new AlphaMail.ColorChooser();
            this.colorChooserBanner = new AlphaMail.ColorChooser();
            this.label3 = new System.Windows.Forms.Label();
            this.tabPageFonts = new System.Windows.Forms.TabPage();
            this.fontChooserBanner = new AlphaMail.FontChooser();
            this.label4 = new System.Windows.Forms.Label();
            this.fontChooserCaption = new AlphaMail.FontChooser();
            this.label2 = new System.Windows.Forms.Label();
            this.tabControl1.SuspendLayout();
            this.tabPageGeneral.SuspendLayout();
            this.tabPageColors.SuspendLayout();
            this.tabPageFonts.SuspendLayout();
            this.SuspendLayout();
            // 
            // label1
            // 
            this.label1.Location = new System.Drawing.Point(3, 95);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(144, 18);
            this.label1.Text = "Selected banner color:";
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPageGeneral);
            this.tabControl1.Controls.Add(this.tabPageColors);
            this.tabControl1.Controls.Add(this.tabPageFonts);
            this.tabControl1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.tabControl1.Location = new System.Drawing.Point(0, 0);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(240, 245);
            this.tabControl1.TabIndex = 9;
            // 
            // tabPageGeneral
            // 
            this.tabPageGeneral.Controls.Add(this.numericUpDownLargeBannerIndent);
            this.tabPageGeneral.Controls.Add(this.label7);
            this.tabPageGeneral.Controls.Add(this.numericUpDownSmallBannerIndent);
            this.tabPageGeneral.Controls.Add(this.numericUpDownIndent);
            this.tabPageGeneral.Controls.Add(this.label6);
            this.tabPageGeneral.Controls.Add(this.label5);
            this.tabPageGeneral.Location = new System.Drawing.Point(0, 0);
            this.tabPageGeneral.Name = "tabPageGeneral";
            this.tabPageGeneral.Size = new System.Drawing.Size(240, 222);
            this.tabPageGeneral.Text = "General";
            // 
            // numericUpDownLargeBannerIndent
            // 
            this.numericUpDownLargeBannerIndent.Location = new System.Drawing.Point(142, 59);
            this.numericUpDownLargeBannerIndent.Name = "numericUpDownLargeBannerIndent";
            this.numericUpDownLargeBannerIndent.Size = new System.Drawing.Size(58, 22);
            this.numericUpDownLargeBannerIndent.TabIndex = 19;
            this.numericUpDownLargeBannerIndent.ValueChanged += new System.EventHandler(itemChanged);
            // 
            // label7
            // 
            this.label7.Location = new System.Drawing.Point(7, 62);
            this.label7.Name = "label7";
            this.label7.Size = new System.Drawing.Size(129, 16);
            this.label7.Text = "Large banner indent:";
            // 
            // numericUpDownSmallBannerIndent
            // 
            this.numericUpDownSmallBannerIndent.Location = new System.Drawing.Point(142, 31);
            this.numericUpDownSmallBannerIndent.Name = "numericUpDownSmallBannerIndent";
            this.numericUpDownSmallBannerIndent.Size = new System.Drawing.Size(58, 22);
            this.numericUpDownSmallBannerIndent.TabIndex = 17;
            this.numericUpDownSmallBannerIndent.ValueChanged += new System.EventHandler(itemChanged);
            // 
            // numericUpDownIndent
            // 
            this.numericUpDownIndent.Location = new System.Drawing.Point(142, 3);
            this.numericUpDownIndent.Name = "numericUpDownIndent";
            this.numericUpDownIndent.Size = new System.Drawing.Size(58, 22);
            this.numericUpDownIndent.TabIndex = 16;
            this.numericUpDownIndent.ValueChanged += new System.EventHandler(itemChanged);
            // 
            // label6
            // 
            this.label6.Location = new System.Drawing.Point(7, 6);
            this.label6.Name = "label6";
            this.label6.Size = new System.Drawing.Size(100, 16);
            this.label6.Text = "Item indent:";
            // 
            // label5
            // 
            this.label5.Location = new System.Drawing.Point(7, 34);
            this.label5.Name = "label5";
            this.label5.Size = new System.Drawing.Size(129, 16);
            this.label5.Text = "Small banner indent:";
            // 
            // tabPageColors
            // 
            this.tabPageColors.Controls.Add(this.colorChooserSelectedBanner);
            this.tabPageColors.Controls.Add(this.colorChooserBanner);
            this.tabPageColors.Controls.Add(this.label3);
            this.tabPageColors.Controls.Add(this.label1);
            this.tabPageColors.Location = new System.Drawing.Point(0, 0);
            this.tabPageColors.Name = "tabPageColors";
            this.tabPageColors.Size = new System.Drawing.Size(232, 219);
            this.tabPageColors.Text = "Colours";
            // 
            // colorChooserSelectedBanner
            // 
            this.colorChooserSelectedBanner.Location = new System.Drawing.Point(3, 116);
            this.colorChooserSelectedBanner.Name = "colorChooserSelectedBanner";
            this.colorChooserSelectedBanner.Size = new System.Drawing.Size(212, 69);
            this.colorChooserSelectedBanner.TabIndex = 15;
            this.colorChooserSelectedBanner.ColorChanged += itemChanged;
            // 
            // colorChooserBanner
            // 
            this.colorChooserBanner.Location = new System.Drawing.Point(3, 23);
            this.colorChooserBanner.Name = "colorChooserBanner";
            this.colorChooserBanner.Size = new System.Drawing.Size(212, 69);
            this.colorChooserBanner.TabIndex = 13;
            this.colorChooserBanner.ColorChanged += itemChanged;
            // 
            // label3
            // 
            this.label3.Location = new System.Drawing.Point(3, 4);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(128, 16);
            this.label3.Text = "Normal banner color:";
            // 
            // tabPageFonts
            // 
            this.tabPageFonts.Controls.Add(this.fontChooserBanner);
            this.tabPageFonts.Controls.Add(this.label4);
            this.tabPageFonts.Controls.Add(this.fontChooserCaption);
            this.tabPageFonts.Controls.Add(this.label2);
            this.tabPageFonts.Location = new System.Drawing.Point(0, 0);
            this.tabPageFonts.Name = "tabPageFonts";
            this.tabPageFonts.Size = new System.Drawing.Size(240, 222);
            this.tabPageFonts.Text = "Fonts";
            // 
            // fontChooserBanner
            // 
            this.fontChooserBanner.Location = new System.Drawing.Point(3, 23);
            this.fontChooserBanner.Name = "fontChooserBanner";
            this.fontChooserBanner.Size = new System.Drawing.Size(154, 57);
            this.fontChooserBanner.TabIndex = 15;
            this.fontChooserBanner.FontChanged += itemChanged;
            // 
            // label4
            // 
            this.label4.Location = new System.Drawing.Point(3, 83);
            this.label4.Name = "label4";
            this.label4.Size = new System.Drawing.Size(79, 17);
            this.label4.Text = "Caption font:";
            // 
            // fontChooserCaption
            // 
            this.fontChooserCaption.Location = new System.Drawing.Point(3, 103);
            this.fontChooserCaption.Name = "fontChooserCaption";
            this.fontChooserCaption.Size = new System.Drawing.Size(154, 57);
            this.fontChooserCaption.TabIndex = 14;
            this.fontChooserCaption.FontChanged += itemChanged;
            // 
            // label2
            // 
            this.label2.Location = new System.Drawing.Point(3, 4);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(100, 16);
            this.label2.Text = "Banner font:";
            // 
            // MainGuiSettings
            // 
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Inherit;
            this.Controls.Add(this.tabControl1);
            this.Name = "MainGuiSettings";
            this.Size = new System.Drawing.Size(240, 245);
            this.tabControl1.ResumeLayout(false);
            this.tabPageGeneral.ResumeLayout(false);
            this.tabPageColors.ResumeLayout(false);
            this.tabPageFonts.ResumeLayout(false);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPageColors;
        private System.Windows.Forms.TabPage tabPageGeneral;
        private ColorChooser colorChooserBanner;
        private System.Windows.Forms.Label label3;
        private ColorChooser colorChooserSelectedBanner;
        private System.Windows.Forms.NumericUpDown numericUpDownLargeBannerIndent;
        private System.Windows.Forms.Label label7;
        private System.Windows.Forms.NumericUpDown numericUpDownSmallBannerIndent;
        private System.Windows.Forms.NumericUpDown numericUpDownIndent;
        private System.Windows.Forms.Label label6;
        private System.Windows.Forms.Label label5;
        private System.Windows.Forms.TabPage tabPageFonts;
        private FontChooser fontChooserBanner;
        private System.Windows.Forms.Label label4;
        private FontChooser fontChooserCaption;
        private System.Windows.Forms.Label label2;
    }
}
