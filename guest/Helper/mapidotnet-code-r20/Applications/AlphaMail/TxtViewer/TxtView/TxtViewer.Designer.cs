namespace TxtView
{
    partial class TxtViewer
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
            this.vScrollBar1 = new System.Windows.Forms.VScrollBar();
            this.txtItems = new System.Windows.Forms.Control();
            this.SuspendLayout();
            // 
            // vScrollBar1
            // 
            this.vScrollBar1.Dock = System.Windows.Forms.DockStyle.Right;
            this.vScrollBar1.LargeChange = 1;
            this.vScrollBar1.Location = new System.Drawing.Point(313, 0);
            this.vScrollBar1.Maximum = 0;
            this.vScrollBar1.Name = "vScrollBar1";
            this.vScrollBar1.Size = new System.Drawing.Size(7, 186);
            this.vScrollBar1.TabIndex = 0;
            this.vScrollBar1.ValueChanged += new System.EventHandler(this.vScrollBar1_ValueChanged);
            // 
            // txtItems
            // 
            this.txtItems.Dock = System.Windows.Forms.DockStyle.Fill;
            this.txtItems.Location = new System.Drawing.Point(0, 0);
            this.txtItems.Name = "txtItems";
            this.txtItems.Size = new System.Drawing.Size(313, 186);
            this.txtItems.TabIndex = 0;
            this.txtItems.KeyDown += new System.Windows.Forms.KeyEventHandler(this.txtItems_KeyDown);
            // 
            // TxtViewer
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(96F, 96F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Dpi;
            this.Controls.Add(this.txtItems);
            this.Controls.Add(this.vScrollBar1);
            this.Name = "TxtViewer";
            this.Size = new System.Drawing.Size(320, 186);
            this.Resize += new System.EventHandler(this.TxtViewer_Resize);
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.VScrollBar vScrollBar1;
        private System.Windows.Forms.Control txtItems;
    }
}
