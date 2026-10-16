namespace AlphaMail
{
    partial class ColorChooser
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
            this.panelShow = new System.Windows.Forms.Panel();
            this.trackBarH = new System.Windows.Forms.TrackBar();
            this.label1 = new System.Windows.Forms.Label();
            this.label2 = new System.Windows.Forms.Label();
            this.label3 = new System.Windows.Forms.Label();
            this.trackBarL = new System.Windows.Forms.TrackBar();
            this.trackBarS = new System.Windows.Forms.TrackBar();
            this.SuspendLayout();
            // 
            // panelShow
            // 
            this.panelShow.Location = new System.Drawing.Point(170, 3);
            this.panelShow.Name = "panelShow";
            this.panelShow.Size = new System.Drawing.Size(35, 60);
            // 
            // trackBarH
            // 
            this.trackBarH.LargeChange = 36;
            this.trackBarH.Location = new System.Drawing.Point(67, 0);
            this.trackBarH.Maximum = 360;
            this.trackBarH.Name = "trackBarH";
            this.trackBarH.Size = new System.Drawing.Size(99, 19);
            this.trackBarH.TabIndex = 1;
            this.trackBarH.ValueChanged += new System.EventHandler(this.trackBar_ValueChanged);
            // 
            // label1
            // 
            this.label1.Location = new System.Drawing.Point(3, 3);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(40, 15);
            this.label1.Text = "Hue:";
            // 
            // label2
            // 
            this.label2.Location = new System.Drawing.Point(3, 22);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(67, 16);
            this.label2.Text = "Saturation:";
            // 
            // label3
            // 
            this.label3.Location = new System.Drawing.Point(3, 44);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(71, 16);
            this.label3.Text = "Luminance:";
            // 
            // trackBarL
            // 
            this.trackBarL.LargeChange = 25;
            this.trackBarL.Location = new System.Drawing.Point(67, 44);
            this.trackBarL.Maximum = 255;
            this.trackBarL.Name = "trackBarL";
            this.trackBarL.Size = new System.Drawing.Size(99, 19);
            this.trackBarL.TabIndex = 7;
            this.trackBarL.ValueChanged += new System.EventHandler(this.trackBar_ValueChanged);
            // 
            // trackBarS
            // 
            this.trackBarS.LargeChange = 25;
            this.trackBarS.Location = new System.Drawing.Point(67, 22);
            this.trackBarS.Maximum = 255;
            this.trackBarS.Name = "trackBarS";
            this.trackBarS.Size = new System.Drawing.Size(99, 19);
            this.trackBarS.TabIndex = 8;
            this.trackBarS.ValueChanged += new System.EventHandler(this.trackBar_ValueChanged);
            // 
            // ColorChooser
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Dpi;
            this.Controls.Add(this.trackBarS);
            this.Controls.Add(this.trackBarL);
            this.Controls.Add(this.label3);
            this.Controls.Add(this.label2);
            this.Controls.Add(this.label1);
            this.Controls.Add(this.trackBarH);
            this.Controls.Add(this.panelShow);
            this.Name = "ColorChooser";
            this.Size = new System.Drawing.Size(212, 69);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.Panel panelShow;
        private System.Windows.Forms.TrackBar trackBarH;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.TrackBar trackBarL;
        private System.Windows.Forms.TrackBar trackBarS;
    }
}
