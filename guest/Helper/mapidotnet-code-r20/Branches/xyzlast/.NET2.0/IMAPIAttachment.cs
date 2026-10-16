using System;
using System.Collections.Generic;
using System.Text;
using System.IO;
using OpenNETCF.Runtime.InteropServices.ComTypes;

namespace MAPIdotnet
{
    public interface IMAPIAttachment
    {
        string FileName { get; }

        int Size { get; }

        IMAPIMessage Message { get; }

        Stream OpenAttachment(bool writeAccess);
        IStream OpenAttachmentNative(bool writeAccess);
    }
}
