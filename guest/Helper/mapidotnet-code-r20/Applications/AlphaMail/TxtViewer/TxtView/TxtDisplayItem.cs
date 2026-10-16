using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;

namespace TxtView
{
    public class TxtDisplayItem
    {
        public enum DisplayStyle
        {
            Normal,
            Bold,
            Italic
        }

        private string caption, message;
        private FontStyle fontStyle;
        private Color color;

        public TxtDisplayItem(string caption,
            string message,
            FontStyle captionStyle,
            Color captionColor)
        { this.caption = caption; this.message = message; this.fontStyle = captionStyle; this.color = captionColor; }

        public string Caption { get { return this.caption; } }
        public string Message { get { return this.message; } }
        public Color CaptionColor { get { return this.color; } }
        public FontStyle CaptionSyle { get { return this.fontStyle; } }
    }


}
